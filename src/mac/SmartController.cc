#include "RadioConfig.hh"
#include "mac/SmartController.hh"

#define DEBUG 0

#if DEBUG
#define dprintf(...) fprintf(stderr, __VA_ARGS__)
#else /* !dprintf */
#define dprintf(...)
#endif /* !DEBUG */

void SendWindow::operator()()
{
    controller.retransmit(*this);
}

void RecvWindow::operator()()
{
    controller.ack(*this);
}

SmartController::SmartController(std::shared_ptr<Net> net,
                                 Seq::uint_type max_sendwin,
                                 Seq::uint_type recvwin)
  : Controller(net)
  , max_sendwin_(max_sendwin)
  , recvwin_(recvwin)
{
    timer_queue_.start();
}

SmartController::~SmartController()
{
    timer_queue_.stop();
}

bool SmartController::pull(std::shared_ptr<NetPacket>& pkt)
{
    // Get a packet to send. We look for a packet on our internal queue first.
    if (!getPacket(pkt))
        return false;

    // Get node ID of destination
    NodeId nexthop = pkt->nexthop;

    // If we have received a packet from the destination, ACK.
    RecvWindow *recvwptr = maybeGetReceiveWindow(nexthop);

    if (recvwptr) {
        RecvWindow                      &recvw = *recvwptr;
        ExtendedHeader                  &ehdr = pkt->getExtendedHeader();
        std::lock_guard<spinlock_mutex> lock(recvw.mutex);

        // The poacket we are ACK'ing had better be no more than 1 more than the
        // max sequence number we've received.
        assert(recvw.ack <= recvw.max + 1);

        pkt->setFlag(kACK);
        ehdr.ack = recvw.ack;

        if (pkt->data_len == 0)
            dprintf("Sending ACK %u\n",
                (unsigned) recvw.ack);
        else
            dprintf("Sending %u with ACK %u\n",
                (unsigned) pkt->seq,
                (unsigned) recvw.ack);

        // We're sending an ACK, so cancel the ACK timer
        timer_queue_.cancel(recvw);
    } else if (pkt->data_len != 0)
        dprintf("Sending %u\n", (unsigned) pkt->seq);

    // Update our send window if this packet has data
    if (pkt->data_len != 0) {
        SendWindow                      &sendw = getSendWindow(nexthop);
        std::lock_guard<spinlock_mutex> lock(sendw.mutex);

        // This asserts that the sequence number of the packet we are sending is
        // in our send window.
        assert(pkt->seq >= sendw.unack && pkt->seq < sendw.unack + sendw.win);

        // Save the packet in our send window.
        sendw[pkt->seq] = pkt;

        // Start the retransmit timer if it is not already running.
        if (!timer_queue_.running(sendw)) {
            dprintf("Starting retransmission timer\n");
            timer_queue_.run_in(sendw, (*net_)[nexthop].retransmission_delay);
        }

        // Update send window metrics
        if (pkt->seq > sendw.max)
            sendw.max = pkt->seq;
    }

    return true;
}

void SmartController::received(std::shared_ptr<RadioPacket>&& pkt)
{
    if (!pkt->isFlagSet(kBroadcast) && pkt->nexthop != net_->getMyNodeId())
        return;

    // Process control info
    if (pkt->isFlagSet(kControl)) {
        for(auto it = pkt->begin(); it != pkt->end(); ++it) {
            switch (it->type) {
                case ControlMsg::Type::kHello:
                {
                    Node &node = net_->addNode(pkt->curhop);

                    node.is_gateway = it->hello.is_gateway;

                    dprintf("Received HELLO from %u\n", (unsigned) pkt->curhop);
                }
                break;

                default:
                    break;
            }
        }
    }

    // If this packet was not destined for us, we are done
    if (pkt->nexthop != net_->getMyNodeId())
        return;

    // Get node ID of source and the extended header
    NodeId         prevhop = pkt->curhop;
    ExtendedHeader &ehdr = pkt->getExtendedHeader();

    // Handle ACK/NAK
    SendWindow *sendwptr = maybeGetSendWindow(prevhop);

    if (sendwptr) {
        SendWindow                      &sendw = *sendwptr;
        std::lock_guard<spinlock_mutex> lock(sendw.mutex);

        if (pkt->isFlagSet(kACK)) {
            if (ehdr.ack > sendw.unack) {
                dprintf("Got ACK %d\n", (int) ehdr.ack);

                // Don't assert this because the sender could crash us with bad
                // data! We protected against this case in the following loop.
                //assert(ehdr.ack <= sendw.max + 1);

                // Move the send window along. It's possibl the sender sends an
                // ACK for something we haven't sent, so we must guard against
                // that here as well
                for (; sendw.unack < ehdr.ack && sendw.unack <= sendw.max; ++sendw.unack)
                    sendw[sendw.unack].reset();

                // If the max packet we sent was ACK'ed, cancel the retransmit
                // timer.
                if (sendw.unack == sendw.max + 1) {
                    dprintf("Canceling retransmission timer\n");
                    timer_queue_.cancel(sendw);
                }

                // Increase the send window. We really only need to do this
                // after the initial ACK, but it doesn't hurt to do it every
                // time...
                sendw.win = sendw.maxwin;

                // Now that our window is open, try to send packets from this
                // node again. We calculate the number of free slots in our
                // window and splice at most that many packets onto the front
                // of the network queue.
                //
                // INVARIANT: We assume packets come from the network in
                // sequence order!
                if (spliceq_) {
                    size_t nslots = std::min(sendw.pending.size(), (size_t) (sendw.unack + sendw.win - sendw.max - 1));
                    auto   first  = sendw.pending.cbegin();
                    auto   last   = std::next(sendw.pending.cbegin(), nslots);

                    spliceq_->splice_front(sendw.pending, first, last);
                }
            }
        } else if (pkt->isFlagSet(kNAK)) {
            if (ehdr.ack >= sendw.unack) {
                if (!sendw[ehdr.ack])
                    fprintf(stderr, "Received NAK from node %d for seq %d, but can't find that packet!\n",
                        (int) prevhop,
                        (int) ehdr.ack);
                else {
                    // We need to make an explicit new reference to the
                    // shared_ptr because push takes ownership of its argument.
                    std::shared_ptr<NetPacket> pkt = sendw[ehdr.ack];

                    if (spliceq_)
                        spliceq_->push_front(std::move(pkt));

                    dprintf("Got NAK %d\n", (int) ehdr.ack);
                }
            }
        }
    }

    // If this packet doesn't contain any data, we are done
    if (pkt->data_len == 0) {
        dprintf("Received ACK %u\n",
            (unsigned) ehdr.ack);
        return;
    }

    dprintf("Received %u with ACK %u\n",
        (unsigned) pkt->seq,
        (unsigned) ehdr.ack);

    // Fill our receive window
    RecvWindow                      &recvw = getReceiveWindow(prevhop, pkt->seq);
    std::lock_guard<spinlock_mutex> lock(recvw.mutex);

    // Start the ACK timer if it is not already running. Even if this is a
    // duplicate packet, we need to send an ACK because the duplicate may be a
    // retransmission, i.e., our previous ACK could have been lost.
    if (!timer_queue_.running(recvw)) {
        dprintf("Starting ACK delay timer\n");
        timer_queue_.run_in(recvw, (*net_)[prevhop].ack_delay);
    }

    // Drop this packet if we have already received it
    if (pkt->seq < recvw.ack) {
        dprintf("Received DUP %u\n",
            (unsigned) pkt->seq);
        return;
    }

    // Drop this packet if it is outside our receive window
    if (pkt->seq > recvw.ack + recvw.win) {
        dprintf("Received OUTSIDE receive window %u\n",
            (unsigned) pkt->seq);
        return;
    }

    // Resize the packet to truncate non-data bytes
    pkt->resize(sizeof(ExtendedHeader) + pkt->data_len);

    // Update the max seq number we've received
    if (pkt->seq > recvw.max)
        recvw.max = pkt->seq;

    // If this is the next packet we expected, send it now and update the
    // receive window
    if (pkt->seq == recvw.ack) {
        recvw.ack++;
        radio_out.push(std::move(pkt));
    } else {
        // Otherwise insert the packet into our receive window
        recvw[pkt->seq] = std::move(pkt);
    }

    // Now drain the receive window until we reach a hole
    for (auto seq = recvw.ack; seq <= recvw.max; ++seq) {
        if (!recvw[seq])
            break;

        radio_out.push(std::move(recvw[seq]));
        recvw[seq].reset();
        ++recvw.ack;
    }
}

void SmartController::retransmit(SendWindow &sendw)
{
    std::lock_guard<spinlock_mutex> lock(sendw.mutex);

    // We may have received an ACK, in which case we don't need to re-transmit
    if (sendw.unack <= sendw.max) {
        dprintf("Retransmitting %u\n",
            (unsigned) sendw.unack);

        // We need to make an explicit new reference to the shared_ptr because
        // push takes ownership of its argument.
        std::shared_ptr<NetPacket> pkt = sendw[sendw.unack];

        // INVARIANT: packets come from the network in sequence order. If this
        // invariant holds, then we will never have a "hole" in our send window,
        // and this assertion must hold.
        assert(pkt);

        if (spliceq_)
            spliceq_->push_front(std::move(pkt));
    }
}

void SmartController::ack(RecvWindow &recvw)
{
    if (!spliceq_)
        return;

    std::lock_guard<spinlock_mutex> lock(recvw.mutex);

    dprintf("Sending delayed ACK %u\n", (unsigned) recvw.ack);

    // Create an ACK-only packet. Why don't we set the ACK field here!? Because
    // it will be filled out when the packet flows back through the controller
    // on its way out the radio. We are just providing the opportunity for an
    // ACK by injecting a packet without a data payload at the head of the
    // queue.
    auto pkt = std::make_shared<NetPacket>(sizeof(ExtendedHeader));
    Node &dest = (*net_)[recvw.node_id];

    pkt->curhop = net_->getMyNodeId();
    pkt->nexthop = recvw.node_id;
    pkt->flags = 0;
    pkt->seq = 0;
    pkt->data_len = 0;
    pkt->src = net_->getMyNodeId();
    pkt->dest = recvw.node_id;
    pkt->check = dest.check;
    pkt->fec0 = dest.fec0;
    pkt->fec1 = dest.fec1;
    pkt->ms = dest.ms;
    pkt->g = dest.g;

    spliceq_->push_front(std::move(pkt));
}

void SmartController::broadcastHello(void)
{
    if (!spliceq_)
        return;

    dprintf("Sending HELLO\n");

    auto pkt = std::make_shared<NetPacket>(sizeof(ExtendedHeader));

    pkt->curhop = net_->getMyNodeId();
    pkt->nexthop = 0;
    pkt->flags = 0;
    pkt->seq = 0;
    pkt->data_len = 0;
    pkt->src = net_->getMyNodeId();
    pkt->dest = 0;
    pkt->check = LIQUID_CRC_32;
    pkt->fec0 = LIQUID_FEC_CONV_V27;
    pkt->fec1 = LIQUID_FEC_NONE;
    pkt->ms = LIQUID_MODEM_QPSK;
    pkt->g = 1.0;

    pkt->setFlag(kBroadcast);

    ControlMsg msg;

    msg.type = ControlMsg::Type::kHello;
    msg.hello.is_gateway = rc->is_gateway;

    pkt->appendControl(msg);

    spliceq_->push_front(std::move(pkt));
}

bool SmartController::getPacket(std::shared_ptr<NetPacket>& pkt)
{
    for (;;) {
        // Get a packet from the network
        if (!net_in.pull(pkt))
            return false;

        assert(pkt);

        SendWindow                      &sendw = getSendWindow(pkt->nexthop);
        std::lock_guard<spinlock_mutex> lock(sendw.mutex);

        // If packet has no payload, we can always send it---it has control
        // information.
        if (pkt->data_len == 0)
            return true;

        // If this packet comes before our window, drop it. It could have snuck
        // in as a retransmission just before the send window moved forward.
        if (pkt->seq < sendw.unack)
            continue;

        // If this packet is in our send window, send it.
        if (pkt->seq < sendw.unack + sendw.win)
            return true;

        // Otherwise, we need to save it for later
        dprintf("Postponing send of out-of-window packet %u\n",
            (unsigned) pkt->seq);
        sendw.pending.emplace_back(std::move(pkt));
    }
}

SendWindow *SmartController::maybeGetSendWindow(NodeId node_id)
{
    std::lock_guard<spinlock_mutex> lock(send_mutex_);
    auto                            it = send_.find(node_id);

    if (it != send_.end())
        return &(it->second);
    else
        return nullptr;
}

SendWindow &SmartController::getSendWindow(NodeId node_id)
{
    std::lock_guard<spinlock_mutex> lock(send_mutex_);
    auto                            it = send_.find(node_id);


    if (it != send_.end())
        return it->second;
    else {
        SendWindow &sendw = send_.emplace(std::piecewise_construct,
                                          std::forward_as_tuple(node_id),
                                          std::forward_as_tuple(node_id, *this, max_sendwin_)).first->second;

        return sendw;
    }
}

RecvWindow *SmartController::maybeGetReceiveWindow(NodeId node_id)
{
    std::lock_guard<spinlock_mutex> lock(recv_mutex_);
    auto                            it = recv_.find(node_id);

    if (it != recv_.end())
        return &(it->second);
    else
        return nullptr;
}

RecvWindow &SmartController::getReceiveWindow(NodeId node_id, Seq seq)
{
    std::lock_guard<spinlock_mutex> lock(recv_mutex_);
    auto                            it = recv_.find(node_id);

    if (it != recv_.end())
        return it->second;
    else {
        RecvWindow &recvw = recv_.emplace(std::piecewise_construct,
                                          std::forward_as_tuple(node_id),
                                          std::forward_as_tuple(node_id, *this, recvwin_)).first->second;

        recvw.ack = recvw.max = seq;

        return recvw;
    }
}