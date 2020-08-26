#include "Logger.hh"
#include "RadioConfig.hh"
#include "mac/SmartController.hh"

#define DEBUG 0

#if DEBUG
#define dprintf(...) logEvent(__VA_ARGS__)
#else /* !dprintf */
#define dprintf(...)
#endif /* !DEBUG */

void SendWindow::Entry::operator()()
{
    sendw.controller.retransmitOnTimeout(*this);
}

void SendWindow::recordACK(const MonoClock::time_point &tx_time)
{
    auto now = MonoClock::now();

    ack_delay.update(now, (now - tx_time).get_real_secs());

    retransmission_delay = std::max(controller.getMinRetransmissionDelay(),
        controller.getRetransmissionDelaySlop()*ack_delay.getValue());
}

void RecvWindow::operator()()
{
    std::lock_guard<spinlock_mutex> lock(this->mutex);

    if (timer_for_ack) {
        controller.ack(*this);
    } else {
        need_selective_ack = true;
        timer_for_ack = true;

        dprintf("ARQ: starting full ACK timer: node=%u",
            (unsigned) node.id);
        controller.timer_queue_.run_in(*this,
            controller.ack_delay_ - controller.sack_delay_);
    }
}

SmartController::SmartController(std::shared_ptr<Net> net,
                                 std::shared_ptr<PHY> phy,
                                 double slot_size,
                                 Seq::uint_type max_sendwin,
                                 Seq::uint_type recvwin,
                                 const std::vector<evm_thresh_t> &evm_thresholds)
  : Controller(net)
  , phy_(phy)
  , slot_size_(slot_size)
  , max_sendwin_(max_sendwin)
  , recvwin_(recvwin)
  , evm_thresholds_(evm_thresholds)
  , short_per_window_(100e-3)
  , long_per_window_(400e-3)
  , long_stats_window_(400e-3)
  , mcsidx_min_(0)
  , mcsidx_max_(0)
  , mcsidx_init_(0)
  , mcsidx_up_per_threshold_(0.04)
  , mcsidx_down_per_threshold_(0.10)
  , mcsidx_alpha_(0.5)
  , mcsidx_prob_floor_(0.1)
  , ack_delay_(100e-3)
  , ack_delay_estimation_window_(1)
  , retransmission_delay_(500e-3)
  , min_retransmission_delay_(200e-3)
  , retransmission_delay_slop_(1.1)
  , sack_delay_(50e-3)
  , explicit_nak_win_(0)
  , explicit_nak_win_duration_(0.0)
  , selective_ack_(false)
  , selective_ack_feedback_delay_(0.0)
  , max_retransmissions_({})
  , demod_always_ordered_(false)
  , enforce_ordering_(false)
  , move_along_(true)
  , decrease_retrans_mcsidx_(false)
  , gen_(std::random_device()())
  , dist_(0, 1.0)
{
    if (evm_thresholds.size() != phy->mcs_table.size())
        throw std::out_of_range("EVM threshold table and PHY MCS table must be the same size");

    // Calculate samples needed to modulate the largest packet we will ever see
    // at each MCS
    size_t max_pkt_size = rc.mtu + sizeof(struct ether_header);

    max_packet_samples_.resize(phy->mcs_table.size());

    for (mcsidx_t mcsidx = 0; mcsidx < phy->mcs_table.size(); ++mcsidx)
        max_packet_samples_[mcsidx] =  phy->getModulatedSize(mcsidx, max_pkt_size);

    timer_queue_.start();
}

SmartController::~SmartController()
{
    timer_queue_.stop();
}

bool SmartController::pull(std::shared_ptr<NetPacket> &pkt)
{
get_packet:
    // Get a packet to send. We look for a packet on our internal queue first.
    if (!getPacket(pkt))
        return false;

    // Handle broadcast packets
    if (pkt->hdr.nexthop == kNodeBroadcast) {
        pkt->mcsidx = mcsidx_broadcast_;
        pkt->g = broadcast_gain.getLinearGain();

        return true;
    }

    // Get node ID of destination
    NodeId nexthop = pkt->hdr.nexthop;

    // If we have received a packet from the destination, add an ACK.
    RecvWindow *recvwptr = maybeGetReceiveWindow(nexthop);

    if (recvwptr) {
        RecvWindow                      &recvw = *recvwptr;
        std::lock_guard<spinlock_mutex> lock(recvw.mutex);

        // The packet we are ACK'ing had better be no more than 1 more than the
        // max sequence number we've received.
        if(recvw.ack > recvw.max + 1)
            logEvent("ARQ: INVARIANT VIOLATED: received packet outside window: ack=%u; mac=%u",
                (unsigned) recvw.ack,
                (unsigned) recvw.max);

        pkt->hdr.flags.ack = 1;
        pkt->ehdr().ack = recvw.ack;

#if DEBUG
        if (pkt->ehdr().data_len == 0)
            dprintf("ARQ: send delayed ack: node=%u; ack=%u",
                (unsigned) nexthop,
                (unsigned) recvw.ack);
        else
            dprintf("ARQ: send ack: node=%u; ack=%u",
                (unsigned) nexthop,
                (unsigned) recvw.ack);
#endif

        // Append selective ACK if needed
        if (recvw.need_selective_ack)
            appendFeedback(*pkt, recvw);
    } else if (pkt->ehdr().data_len != 0)
        dprintf("ARQ: send: node=%u; seq=%u",
            (unsigned) nexthop,
            (unsigned) pkt->hdr.seq);

    // Update our send window if this packet has data
    if (pkt->ehdr().data_len != 0) {
        SendWindow                      &sendw = getSendWindow(nexthop);
        Node                            &dest = (*net_)[nexthop];
        std::lock_guard<spinlock_mutex> lock(sendw.mutex);

        // It is possible that the send window shifts after we pull a packet
        // but before we get to this point. For example, an ACK could be
        // received in between the time we release the lock on the receive
        // window and this point. If that happens, we get another packet
        if (pkt->hdr.seq < sendw.unack) {
            pkt.reset();
            goto get_packet;
        }

        // This checks that the sequence number of the packet we are sending is
        // in our send window.
        if (pkt->hdr.seq < sendw.unack || pkt->hdr.seq >= sendw.unack + sendw.win) {
            logEvent("ARQ: INVARIANT VIOLATED: asked to send packet outside window: nexthop=%u; seq=%u; unack=%u; win=%u",
                (unsigned) nexthop,
                (unsigned) pkt->hdr.seq,
                (unsigned) sendw.unack,
                (unsigned) sendw.win);
            pkt.reset();
            goto get_packet;
        }

        // Save the packet in our send window.
        sendw[pkt->hdr.seq] = pkt;
        sendw[pkt->hdr.seq].timestamp = MonoClock::now();

        // If this packet is a retransmission, increment the retransmission
        // count, otherwise set it to 0.
        if (pkt->internal_flags.retransmission)
            ++pkt->nretrans;

        // Update send window metrics
        if (pkt->hdr.seq > sendw.max)
            sendw.max = pkt->hdr.seq;

        // If we have locally updated our send window, tell the receiver.
        if (sendw.locally_updated) {
            logEvent("ARQ: Setting unack: unack=%u",
                (unsigned) sendw.unack);
            pkt->appendSetUnack(sendw.unack);
            sendw.locally_updated = false;
        }

        // Apply TX params. If the destination can transmit, proceed as usual.
        // Otherwise, use the default MCS.
        if (dest.can_transmit) {
            // If this is a retransmission, the packet has a deadline, and it
            // was transmitted at the current MCS, decrease the MCS in the hope
            // that we can get this packet through before its deadline passes.
            if (decrease_retrans_mcsidx_ &&
                pkt->internal_flags.retransmission &&
                pkt->deadline &&
                pkt->mcsidx == sendw.mcsidx &&
                pkt->mcsidx > mcsidx_min_)
                --pkt->mcsidx;
            else
                pkt->mcsidx = sendw.mcsidx;

            pkt->g = dest.g;
        } else {
            pkt->mcsidx = mcsidx_init_;
            pkt->g = dest.g;
        }
    } else {
        // Apply broadcast TX params
        pkt->mcsidx = mcsidx_broadcast_;
        pkt->g = ack_gain.getLinearGain();
    }

    return true;
}

void SmartController::received(std::shared_ptr<RadioPacket> &&pkt)
{
    // Skip packets with invalid header
    if (pkt->internal_flags.invalid_header)
        return;

    // Skip packets that aren't for us
    if (pkt->hdr.nexthop != kNodeBroadcast &&
        pkt->hdr.nexthop != net_->getMyNodeId())
        return;

    // Get a reference to the sending node. This will add a new node to the
    // network if it doesn't already exist.
    Node &node = (*net_)[pkt->hdr.curhop];

    // Get node ID of source
    NodeId prevhop = pkt->hdr.curhop;

    if (pkt->hdr.flags.has_data) {
        RecvWindow                      &recvw = getReceiveWindow(prevhop, pkt->hdr.seq, pkt->hdr.flags.syn);
        std::lock_guard<spinlock_mutex> lock(recvw.mutex);

        // Update metrics. EVM and RSSI should be valid as long as the header is
        // valid.
        recvw.long_evm.update(pkt->timestamp, pkt->evm);
        recvw.long_rssi.update(pkt->timestamp, pkt->rssi);

        // Immediately NAK data packets with a bad payload if they contain data.
        // We can't do anything else with the packet.
        if (pkt->internal_flags.invalid_payload) {
            // Update the max seq number we've received
            if (pkt->hdr.seq > recvw.max) {
                recvw.max = pkt->hdr.seq;
                recvw.max_timestamp = pkt->timestamp;
            }

            // Send a NAK
            nak(recvw, pkt->hdr.seq);

            // We're done with this packet since it has a bad payload
            return;
        }
    } else  {
        RecvWindow *recvwptr = maybeGetReceiveWindow(prevhop);

        // Update metrics. EVM and RSSI should be valid as long as the header is
        // valid. We won't create a receive window if we don't already have one
        // because this packet has no data payload.
        if (recvwptr) {
            RecvWindow                      &recvw = *recvwptr;
            std::lock_guard<spinlock_mutex> lock(recvw.mutex);

            recvw.long_evm.update(pkt->timestamp, pkt->evm);
            recvw.long_rssi.update(pkt->timestamp, pkt->rssi);
        }

        // We're done with this packet if it has a bad payload
        if (pkt->internal_flags.invalid_payload)
            return;
    }

    // Process control info
    if (pkt->hdr.flags.has_control) {
        handleCtrlHello(*pkt, node);
        handleCtrlTimestampEchos(*pkt, node);
    }

    // Handle broadcast packets
    if (pkt->hdr.nexthop == kNodeBroadcast) {
        // Clear all control information, leaving only data payload behind.
        pkt->clearControl();

        // Send the packet along if it has data
        if (pkt->ehdr().data_len != 0)
            radio_out.push(std::move(pkt));

        return;
    }

    // If this packet was not destined for us, we are done
    if (pkt->hdr.nexthop != net_->getMyNodeId())
        return;

    // Handle ACK/NAK
    SendWindow *sendwptr = maybeGetSendWindow(prevhop);

    if (sendwptr) {
        SendWindow                      &sendw = *sendwptr;
        std::lock_guard<spinlock_mutex> lock(sendw.mutex);
        MonoClock::time_point           tfeedback = MonoClock::now() - selective_ack_feedback_delay_;
        std::optional<Seq>              nak;

        // Handle any NAK
        nak = handleNAK(*pkt, sendw);

        // If packets are always demodulated in order, then when we see an
        // explicit NAK, we can assume all packets up to and including the
        // NAK'ed packet should have been received. In this case, look at
        // feedback at least up to the sequence number that was NAK'ed. We add a
        // tiny amount of slop, 0.001 sec, to make sure we *include* the NAK'ed
        // packet.
        if (demod_always_ordered_ && nak)
            tfeedback = std::max(tfeedback, sendw[*nak].timestamp + 0.001);

        // Handle ACK
        if (pkt->hdr.flags.ack) {
            // Handle statistics reported by the receiver. We do this before
            // looking at ACK's because we use the statistics to decide whether
            // to move up our MCS.
            handleReceiverStats(*pkt, sendw);

            if (pkt->ehdr().ack > sendw.unack) {
                dprintf("ARQ: ack: node=%u; seq=[%u,%u)",
                    (unsigned) node.id,
                    (unsigned) sendw.unack,
                    (unsigned) pkt->ehdr().ack);

                // Don't assert this because the sender could crash us with bad
                // data! We protected against this case in the following loop.
                //assert(pkt->ehdr().ack <= sendw.max + 1);

                // Move the send window along. It's possible the sender sends an
                // ACK for something we haven't sent, so we must guard against
                // that here as well
                for (; sendw.unack < pkt->ehdr().ack && sendw.unack <= sendw.max; ++sendw.unack) {
                    // Handle the ACK
                    handleACK(sendw, sendw.unack);

                    // Update our packet error rate to reflect successful TX
                    if (sendw.unack >= sendw.per_end)
                        txSuccess(sendw);
                }

                // unack is the NEXT un-ACK'ed packet, i.e., the packet we  are
                // waiting to hear about next. Note that it is possible for the
                // sender to ACK a packet we've already decided was bad, e.g., a
                // retranmission, so we must be careful not to "rewind" the PER
                // window here by blindly setting sendw.per_end = unack without
                // the test.
                if (sendw.unack > sendw.per_end)
                    sendw.per_end = sendw.unack;
            }

            // Handle selective ACK. We do this *after* handling the ACK,
            // because a selective ACK tells us about packets *beyond* that
            // which was ACK'ed.
            handleSelectiveACK(*pkt, sendw, tfeedback);

            // If the NAK is for a retransmitted packet, count it as a
            // transmission failure. We need to check for this case because a
            // NAK for a retransmitted packet will have already been counted
            // toward our PER the first time the packet was NAK'ed. If the
            // packet has already been re-transmitted, don't record a failure.
            if (nak) {
                SendWindow::Entry &entry = sendw[*nak];

                if (entry.pkt && sendw.mcsidx >= entry.pkt->mcsidx && entry.pkt->nretrans > 0) {
                    txFailure(sendw);

                    logEvent("ARQ: txFailure nak of retransmission: node=%u; seq=%u; mcsidx=%u",
                        (unsigned) node.id,
                        (unsigned) *nak,
                        (unsigned) entry.pkt->mcsidx);
                }
            }

            // Update MCS based on new PER
            updateMCS(sendw);

            // Advance the send window. It is possible that packets immediately
            // after the packet that the sender just ACK'ed have timed out and
            // been dropped, so advanceSendWindow must look for dropped packets
            // and attempt to push the send window up towards max.
            advanceSendWindow(sendw);
        }
    }

    // If this packet doesn't contain any data, we are done
    if (pkt->ehdr().data_len == 0) {
        dprintf("ARQ: recv: node=%u; ack=%u",
            (unsigned) prevhop,
            (unsigned) pkt->ehdr().ack);
        return;
    }

#if DEBUG
    if (hdr.flags.ack)
        dprintf("ARQ: recv: node=%u; seq=%u; ack=%u",
            (unsigned) prevhop,
            (unsigned) pkt->hdr.seq,
            (unsigned) pkt->ehdr().ack);
    else
        dprintf("ARQ: recv: node=%u; seq=%u",
            (unsigned) prevhop,
            (unsigned) pkt->hdr.seq);
#endif

    // Fill our receive window
    RecvWindow                      &recvw = getReceiveWindow(prevhop, pkt->hdr.seq, pkt->hdr.flags.syn);
    std::lock_guard<spinlock_mutex> lock(recvw.mutex);

    // If this is a SYN packet, ACK immediately to open up the window.
    //
    // Otherwise, start the ACK timer if it is not already running. Even if this
    // is a duplicate packet, we need to send an ACK because the duplicate may
    // be a retransmission, i.e., our previous ACK could have been lost.
    if (pkt->hdr.flags.syn)
        ack(recvw);
    else
        startSACKTimer(recvw);

    // Handle sender setting unack
    handleSetUnack(*pkt, recvw);

    // Drop this packet if it is before our receive window
    if (pkt->hdr.seq < recvw.ack) {
        dprintf("ARQ: recv OUTSIDE WINDOW (DUP): node=%u; seq=%u",
            (unsigned) prevhop,
            (unsigned) pkt->hdr.seq);
        return;
    }

    // If the packet is after our receive window, we need to advance the receive
    // window.
    if (pkt->hdr.seq >= recvw.ack + recvw.win) {
        logEvent("ARQ: recv OUTSIDE WINDOW (ADVANCE): node=%u; seq=%u",
            (unsigned) prevhop,
            (unsigned) pkt->hdr.seq);

        // We want to slide the window forward so pkt->hdr.seq is the new max
        // packet. We therefore need to "forget" all packets in our current
        // window with sequence numbers less than pkt->hdr.seq - recvw.win. It's
        // possible this number is greater than our max received sequence
        // number, so we must account for that as well!
        Seq new_ack = pkt->hdr.seq + 1 - recvw.win;
        Seq forget = new_ack > recvw.max ? recvw.max + 1 : new_ack;

        // Go ahead and deliver packets that will be left outside our window.
        for (auto seq = recvw.ack; seq < forget; ++seq) {
            RecvWindow::Entry &entry = recvw[seq];

            // Go ahead and deliver the packet
            if (entry.pkt && !entry.delivered)
                radio_out.push(std::move(entry.pkt));

            // Release the packet
            entry.reset();
        }

        recvw.ack = new_ack;
    } else if (recvw[pkt->hdr.seq].received) {
        // Drop this packet if we have already received it
        dprintf("ARQ: recv DUP: node=%u; seq=%u",
            (unsigned) prevhop,
            (unsigned) pkt->hdr.seq);
        return;
    }

    // Update the max seq number we've received
    if (pkt->hdr.seq > recvw.max) {
        recvw.max = pkt->hdr.seq;
        recvw.max_timestamp = pkt->timestamp;
    }

    // Clear packet control information now that it's already been processed.
    pkt->clearControl();

    // If this is the next packet we expected, send it now and update the
    // receive window
    if (pkt->hdr.seq == recvw.ack) {
        recvw.ack++;
        radio_out.push(std::move(pkt));
    } else if (!enforce_ordering_ && !pkt->isTCP()) {
        // If this is not a TCP packet, insert it into our receive window, but
        // also go ahead and send it.
        radio_out.push(std::move(pkt));
        recvw[pkt->hdr.seq].alreadyDelivered();
    } else {
        // Insert the packet into our receive window
        recvw[pkt->hdr.seq] = std::move(pkt);
    }

    // Now drain the receive window until we reach a hole
    for (auto seq = recvw.ack; seq <= recvw.max; ++seq) {
        RecvWindow::Entry &entry = recvw[seq];

        if (!entry.received)
            break;

        if (!entry.delivered)
            radio_out.push(std::move(entry.pkt));

        entry.reset();
        ++recvw.ack;
    }
}

void SmartController::transmitted(std::list<std::unique_ptr<ModPacket>> &mpkts)
{
    for (auto it = mpkts.begin(); it != mpkts.end(); ++it) {
        NetPacket &pkt = *(*it)->pkt;

        if (pkt.hdr.nexthop != kNodeBroadcast && pkt.ehdr().data_len != 0) {
            SendWindow                      &sendw = getSendWindow(pkt.hdr.nexthop);
            std::lock_guard<spinlock_mutex> lock(sendw.mutex);

            // Start the retransmit timer if it is not already running.
            startRetransmissionTimer(sendw[pkt.hdr.seq]);
        }

        // Cancel the selective ACK timer when we actually have sent a selective ACK
        if (pkt.internal_flags.has_selective_ack) {
            RecvWindow                      &recvw = *maybeGetReceiveWindow(pkt.hdr.nexthop);
            std::lock_guard<spinlock_mutex> lock(recvw.mutex);

            timer_queue_.cancel(recvw);
        }
    }
}

void SmartController::retransmitOnTimeout(SendWindow::Entry &entry)
{
    SendWindow                      &sendw = entry.sendw;
    std::lock_guard<spinlock_mutex> lock(sendw.mutex);

    if (!entry.pkt) {
        logEvent("AMC: attempted to retransmit ACK'ed packet on timeout: node=%u",
            (unsigned) sendw.node.id);
        return;
    }

    // Record the packet error as long as receiving node can transmit
    if (sendw.node.can_transmit && sendw.mcsidx >= entry.pkt->mcsidx) {
        txFailure(sendw);

        logEvent("AMC: txFailure retransmission: node=%u; seq=%u; mcsidx=%u; short per=%f",
            (unsigned) sendw.node.id,
            (unsigned) entry.pkt->hdr.seq,
            (unsigned) entry.pkt->mcsidx,
            sendw.short_per.getValue());

        updateMCS(sendw);
    }

    // Actually retransmit (or drop) the packet
    retransmitOrDrop(entry);
}

void SmartController::ack(RecvWindow &recvw)
{
    if (!netq_)
        return;

    if (!net_->me().can_transmit)
        return;

    // Create an ACK-only packet. Why don't we set the ACK field here!? Because
    // it will be filled out when the packet flows back through the controller
    // on its way out the radio. We are just providing the opportunity for an
    // ACK by injecting a packet without a data payload at the head of the
    // queue.
    auto pkt = std::make_shared<NetPacket>(sizeof(ExtendedHeader));

    pkt->hdr.curhop = net_->getMyNodeId();
    pkt->hdr.nexthop = recvw.node.id;
    pkt->hdr.flags = {0};
    pkt->hdr.seq = {0};
    pkt->ehdr().data_len = 0;
    pkt->ehdr().src = net_->getMyNodeId();
    pkt->ehdr().dest = recvw.node.id;

    // Append selective ACK control messages
    appendFeedback(*pkt, recvw);

    netq_->push_hi(std::move(pkt));
}

void SmartController::nak(RecvWindow &recvw, Seq seq)
{
    if (!netq_)
        return;

    if (!net_->me().can_transmit)
        return;

    // If we have a zero-sized NAK window, don't send any NAK's.
    if (recvw.explicit_nak_win.size() == 0)
        return;

    // Limit number of explicit NAK's we send
    auto now = MonoClock::now();

    if (recvw.explicit_nak_win[recvw.explicit_nak_idx] + explicit_nak_win_duration_ > now)
        return;

    recvw.explicit_nak_win[recvw.explicit_nak_idx] = now;
    recvw.explicit_nak_idx = (recvw.explicit_nak_idx + 1) % explicit_nak_win_;

    // Send the explicit NAK
    logEvent("ARQ: send nak: node=%u; nak=%u",
        (unsigned) recvw.node.id,
        (unsigned) seq);

    // Create an ACK-only packet. Why don't we set the ACK field here!? Because
    // it will be filled out when the packet flows back through the controller
    // on its way out the radio. We are just providing the opportunity for an
    // ACK by injecting a packet without a data payload at the head of the
    // queue.
    auto pkt = std::make_shared<NetPacket>(sizeof(ExtendedHeader));

    pkt->hdr.curhop = net_->getMyNodeId();
    pkt->hdr.nexthop = recvw.node.id;
    pkt->hdr.flags = {0};
    pkt->hdr.seq = {0};
    pkt->ehdr().data_len = 0;
    pkt->ehdr().src = net_->getMyNodeId();
    pkt->ehdr().dest = recvw.node.id;

    // Append NAK control message
    pkt->appendNak(seq);

    // Append selective ACK control messages
    appendFeedback(*pkt, recvw);

    netq_->push_hi(std::move(pkt));
}

void SmartController::broadcastHello(void)
{
    if (!netq_)
        return;

    if (!net_->me().can_transmit)
        return;

    dprintf("ARQ: broadcast HELLO");

    auto pkt = std::make_shared<NetPacket>(sizeof(ExtendedHeader));

    pkt->hdr.curhop = net_->getMyNodeId();
    pkt->hdr.nexthop = kNodeBroadcast;
    pkt->hdr.flags = {0};
    pkt->hdr.seq = {0};
    pkt->ehdr().data_len = 0;
    pkt->ehdr().src = net_->getMyNodeId();
    pkt->ehdr().dest = kNodeBroadcast;

    // Append hello message
    ControlMsg::Hello msg;
    Node              &me = net_->me();

    msg.is_gateway = me.is_gateway;

    pkt->appendHello(msg);

    // Echo most recently heard timestamps if we are the time master
    std::optional<NodeId> time_master = net_->getTimeMaster();

    if (time_master && *time_master == net_->getMyNodeId())
        net_->foreach([&] (Node &node) {
            std::lock_guard<std::mutex> lock(node.timestamps_mutex);
            auto                        last_timestamp = node.timestamps.rbegin();

            if (node.id != net_->getMyNodeId() && last_timestamp != node.timestamps.rend()) {
                logEvent("TIMESYNC: Echoing timestamp: node=%u; t_sent=%f; t_recv=%f",
                    (unsigned) node.id,
                    (double) last_timestamp->first.get_real_secs(),
                    (double) last_timestamp->second.get_real_secs());

                pkt->appendTimestampEcho(node.id,
                                         last_timestamp->first,
                                         last_timestamp->second);
            }
        });

    // Send a timestamped HELLO
    if (netq_) {
        pkt->mcsidx = mcsidx_broadcast_;
        pkt->g = 1.0;
        pkt->internal_flags.timestamp = 1;
        netq_->push_hi(std::move(pkt));
    }
}

void SmartController::resetMCSTransitionProbabilities(void)
{
    std::lock_guard<spinlock_mutex> lock(send_mutex_);

    for (auto it = send_.begin(); it != send_.end(); ++it) {
        SendWindow                      &sendw = it->second;
        std::lock_guard<spinlock_mutex> lock(sendw.mutex);

        std::vector<double>&v = sendw.mcsidx_prob;

        std::fill(v.begin(), v.end(), 1.0);
    }
}

void SmartController::retransmitOrDrop(SendWindow::Entry &entry)
{
    assert(entry.pkt);

    if (entry.shouldDrop(max_retransmissions_))
        drop(entry);
    else
        retransmit(entry);
}

/** NOTE: The lock on the send window to which entry belongs MUST be held before
 * calling retransmit.
 */
void SmartController::retransmit(SendWindow::Entry &entry)
{
    // Squelch a retransmission when the destination can't transmit because we
    // won't be able to hear an ACK anyway.
    if (!entry.sendw.node.can_transmit) {
        // We need to restart the retransmission timer here so that the packet
        // will be retransmitted if the destination can transmit in the future.
        timer_queue_.cancel(entry);
        startRetransmissionTimer(entry);
        return;
    }

    if (!entry.pkt) {
        logEvent("AMC: attempted to retransmit ACK'ed packet");
        return;
    }

    logEvent("ARQ: retransmit: node=%u; seq=%u; mcsidx=%u",
        (unsigned) entry.pkt->hdr.nexthop,
        (unsigned) entry.pkt->hdr.seq,
        (unsigned) entry.pkt->mcsidx);

    // The retransmit timer will be restarted when the packet is actually sent,
    // so don't re-start it here! Doing so can lead to a cascade of retransmit
    // timers firing when there are a large number of outstanding transmissions
    // and we suddenly need to ratchet down the MCS. Instead, we cancel the
    // timer here and allow it to be restarted upon transmission. We need to
    // cancel the timer because retransmission could be triggered but something
    // OTHER than a retransmission time-out, e.g., an explicit NAK, and if we
    // don't cancel it, we can end up retransmitting the same packet twice,
    // e.g., once due to the explicit NAK, and again due to a retransmission
    // timeout.
    //
    // The one case where we DO start the transmit timer here is when the MAC
    // cannot currently send a packet, in which case we DO NOT re-queue the
    // packet, but instead just restart the retransmission timer.
    timer_queue_.cancel(entry);

    if (net_->me().can_transmit) {
        // We need to make an explicit new reference to the shared_ptr because
        // push takes ownership of its argument.
        std::shared_ptr<NetPacket> pkt = entry;

        // Clear any control information in the packet
        pkt->clearControl();

        // Mark the packet as a retransmission
        pkt->internal_flags.retransmission = 1;

        // Re-queue the packet. The ACK and MCS will be set properly upon
        // retransmission.
        if (netq_)
            netq_->repush(std::move(pkt));
    } else
        startRetransmissionTimer(entry);
}

void SmartController::drop(SendWindow::Entry &entry)
{
    SendWindow &sendw = entry.sendw;

    // If the packet has already been ACK'd, forget it
    if (!entry)
        return;

    // Drop the packet
    if (logger)
        logger->logDrop(Clock::now(),
                        entry.pkt->nretrans,
                        entry.pkt->hdr,
                        entry.pkt->ehdr(),
                        entry.pkt->mgen_flow_uid.value_or(0),
                        entry.pkt->mgen_seqno.value_or(0),
                        entry.pkt->mcsidx,
                        entry.pkt->size());

    logEvent("ARQ: dropping packet: node=%u; seq=%u",
        (unsigned) sendw.node.id,
        (unsigned) entry.pkt->hdr.seq);

    // Cancel retransmission timer
    timer_queue_.cancel(entry);

    // Release the packet
    entry.reset();

    // Advance send window if we can
    Seq old_unack = sendw.unack;

    advanceSendWindow(sendw);

    // See if we locally updated the send window. If so, we need to tell the
    // receiver, so set the locally_updated flag.
    if (sendw.unack > old_unack)
        sendw.locally_updated = true;
}

void SmartController::advanceSendWindow(SendWindow &sendw)
{
    // Advance send window if we can
    while (sendw.unack <= sendw.max && !sendw[sendw.unack])
        ++sendw.unack;

    // Increase the send window. We really only need to do this after the
    // initial ACK, but it doesn't hurt to do it every time...
    sendw.win = sendw.maxwin;

    // Indicate that this node's send window is now open
    if (sendw.seq < sendw.unack + sendw.win)
        netq_->setSendWindowStatus(sendw.node.id, true);
}

void SmartController::startRetransmissionTimer(SendWindow::Entry &entry)
{
    // Start the retransmit timer if the packet has not already been ACK'ed and
    // the timer is not already running
    if (entry.pkt && !timer_queue_.running(entry)) {
        dprintf("ARQ: starting retransmission timer: node=%u; seq=%u",
            (unsigned) entry.sendw.node.id,
            (unsigned) entry.pkt->hdr.seq);
        timer_queue_.run_in(entry, entry.sendw.retransmission_delay);
    }
}

void SmartController::startSACKTimer(RecvWindow &recvw)
{
    // Start the selective ACK timer if it is not already running.
    if (!timer_queue_.running(recvw)) {
        dprintf("ARQ: starting SACK timer: node=%u",
            (unsigned) recvw.node.id);

        recvw.need_selective_ack = false;
        recvw.timer_for_ack = false;
        timer_queue_.run_in(recvw, sack_delay_);
    }
}

void SmartController::handleCtrlHello(RadioPacket &pkt, Node &node)
{
    for(auto it = pkt.begin(); it != pkt.end(); ++it) {
        switch (it->type) {
            case ControlMsg::Type::kHello:
            {
                node.is_gateway = it->hello.is_gateway;

                dprintf("ARQ: HELLO: node=%u",
                    (unsigned) pkt.hdr.curhop);

                logEvent("ARQ: Discovered neighbor: node=%u; gateway=%s",
                    (unsigned) pkt.hdr.curhop,
                    node.is_gateway ? "true" : "false");
            }
            break;

            case ControlMsg::Type::kTimestamp:
            {
                MonoClock::time_point t_sent;
                MonoClock::time_point t_recv;

                t_sent = it->timestamp.t_sent.to_mono_time();
                t_recv = pkt.timestamp;

		{
                    std::lock_guard<std::mutex> lock(node.timestamps_mutex);

                    node.timestamps.emplace_back(std::make_pair(t_sent, t_recv));
                }

                logEvent("TIMESYNC: Timestamp: node=%u; t_sent=%f; t_recv=%f",
                    (unsigned) pkt.hdr.curhop,
                    (double) t_sent.get_real_secs(),
                    (double) t_recv.get_real_secs());
            }
            break;

            default:
                break;
        }
    }
}

void SmartController::handleCtrlTimestampEchos(RadioPacket &pkt, Node &node)
{
    // If the transmitter is the time master, record our echoed timestamps.
    std::optional<NodeId> time_master = net_->getTimeMaster();

    if (node.id != net_->getMyNodeId() && time_master && node.id == *time_master) {
        for(auto it = pkt.begin(); it != pkt.end(); ++it) {
            switch (it->type) {
                case ControlMsg::Type::kTimestampEcho:
                {
                    if (it->timestamp_echo.node == net_->getMyNodeId()) {
                        MonoClock::time_point t_sent;
                        MonoClock::time_point t_recv;

                        t_sent = it->timestamp_echo.t_sent.to_mono_time();
                        t_recv = it->timestamp_echo.t_recv.to_mono_time();

                        {
                            std::lock_guard<std::mutex> lock(echoed_timestamps_mutex_);

                            echoed_timestamps_.emplace_back(std::make_pair(t_sent, t_recv));
                        }

                        logEvent("TIMESYNC: Timestamp echo: node=%u; t_sent=%f; t_recv=%f",
                            (unsigned) pkt.hdr.curhop,
                            (double) t_sent.get_real_secs(),
                            (double) t_recv.get_real_secs());
                    }
                }
                break;

                default:
                    break;
            }
        }
    }
}

inline void apendSelectiveACK(NetPacket &pkt,
                              RecvWindow &recvw,
                              Seq begin,
                              Seq end)
{
    logEvent("ARQ: send selective ack: node=%u; seq=[%u, %u)",
        (unsigned) recvw.node.id,
        (unsigned) begin,
        (unsigned) end);
    pkt.appendSelectiveAck(begin, end);
}

void SmartController::appendFeedback(NetPacket &pkt, RecvWindow &recvw)
{
    // Append statistics
    pkt.appendReceiverStats(recvw.long_evm.getValue(), recvw.long_rssi.getValue());

    // Append selective ACKs
    if (!selective_ack_)
        return;

    bool in_run = false; // Are we in the middle of a run of ACK's?
    Seq  begin = recvw.ack;
    Seq  end = recvw.ack;
    int  nsacks = 0;

    // The ACK in the (extended) header will handle ACK'ing recvw.ack, so we
    // need to start looking for selective ACK's at recvw.ack + 1. Recall that
    // recvw.ack is the next sequence number we should ACK, meaning we have
    // successfully received (or given up) on all packets with sequence numbers
    // <= recvw.ack. In particular, this means that recvw.ack + 1 should NOT be
    // ACK'ed, because otherwise recvw.ack would be equal to recvw.ack + 1!
    for (Seq seq = recvw.ack + 1; seq <= recvw.max; ++seq) {
        if (recvw[seq].received) {
            if (!in_run) {
                in_run = true;
                begin = seq;
            }

            end = seq;
        } else {
            if (in_run) {
                apendSelectiveACK(pkt, recvw, begin, end + 1);
                nsacks++;

                in_run = false;
            }
        }
    }

    // Close out any final run
    if (in_run) {
        apendSelectiveACK(pkt, recvw, begin, end + 1);
        nsacks++;
    }

    // If we cannot ACK recvw.max, add an empty selective ACK range marking then
    // end up our received packets. This will inform the sender that the last
    // stretch of packets WAS NOT received.
    if (end < recvw.max) {
        apendSelectiveACK(pkt, recvw, recvw.max+1, recvw.max+1);
        nsacks++;
    }

    // If we have too many selective ACK's, keep as many as we can, but keep the
    // *latest* selective ACKs.
    if (pkt.size() > rc.mtu) {
        // How many SACK's do we need to remove?
        constexpr size_t sack_size = ctrlsize(ControlMsg::kSelectiveAck);
        int              nremove;
        int              nkeep;

        nremove = (pkt.size() - rc.mtu + sack_size - 1) /
                      sack_size;

        if (nremove > nsacks)
            nremove = nsacks;

        if (nremove > 0) {
            nkeep = nsacks - nremove;

            logEvent("ARQ: pruning SACKs: node=%u; nremove=%d; nkeep=%d",
                (unsigned) recvw.node.id,
                nremove,
                nkeep);

            unsigned char *sack_start = pkt.data() + pkt.size() -
                                            nsacks*sack_size;

            memmove(sack_start, sack_start+nremove*sack_size, nkeep*sack_size);
            pkt.setControlLen(pkt.getControlLen() - nremove*sack_size);
            pkt.resize(pkt.size() - nremove*sack_size);
        }
    }

    // Mark this packet as containing a selective ACK
    pkt.internal_flags.has_selective_ack = 1;

    // We no longer need a selective ACK
    recvw.need_selective_ack = false;
}

void SmartController::handleReceiverStats(RadioPacket &pkt, SendWindow &sendw)
{
    for(auto it = pkt.begin(); it != pkt.end(); ++it) {
        switch (it->type) {
            case ControlMsg::Type::kReceiverStats:
            {
                sendw.long_evm = it->receiver_stats.long_evm;
                sendw.long_rssi = it->receiver_stats.long_rssi;
            }
            break;

            default:
                break;
        }
    }
}

void SmartController::handleACK(SendWindow &sendw, const Seq &seq)
{
    SendWindow::Entry &entry = sendw[seq];

    // If this packet is outside our send window, we're done.
    if (seq < sendw.unack || seq >= sendw.unack + sendw.win) {
        logEvent("ARQ: ack for packet outside send window: node=%u; seq=%u; unack=%u; end=%u",
            (unsigned) sendw.node.id,
            (unsigned) seq,
            (unsigned) sendw.unack,
            (unsigned) sendw.unack + sendw.win);

        return;
    }

    // If this packet has already been ACK'ed, we're done.
    if (!entry.pkt) {
        dprintf("ARQ: ack for already ACK'ed packet: node=%u; seq=%u",
            (unsigned) sendw.node.id,
            (unsigned) seq);

        return;
    }

    // Record ACK delay
    sendw.recordACK(entry.timestamp);

    // Cancel retransmission timer for ACK'ed packet
    timer_queue_.cancel(entry);

    // Release the packet since it's been ACK'ed
    entry.reset();
}

std::optional<Seq> SmartController::handleNAK(RadioPacket &pkt,
                                              SendWindow &sendw)
{
    std::optional<Seq> result;

    for(auto it = pkt.begin(); it != pkt.end(); ++it) {
        switch (it->type) {
            case ControlMsg::Type::kNak:
            {
                SendWindow::Entry &entry = sendw[it->nak];

                // If this packet is outside our send window, ignore the NAK.
                if (it->nak < sendw.unack || it->nak >= sendw.unack + sendw.win || !entry.pkt) {
                    logEvent("ARQ: nak for packet outside send window: node=%u; seq=%u; unack=%u; end=%u",
                        (unsigned) sendw.node.id,
                        (unsigned) it->nak,
                        (unsigned) sendw.unack,
                        (unsigned) sendw.unack + sendw.win);
                // If this packet has already been ACK'ed, ignore the NAK.
                } else if (!entry.pkt) {
                    logEvent("ARQ: nak for already ACK'ed packet: node=%u; seq=%u",
                        (unsigned) sendw.node.id,
                        (unsigned) it->nak);
                } else {
                    // Log the NAK
                    logEvent("ARQ: nak: node=%u; seq=%u",
                        (unsigned) sendw.node.id,
                        (unsigned) it->nak);

                    result = it->nak;
                }
            }
            break;

            default:
                break;
        }
    }

    return result;
}

void SmartController::handleSelectiveACK(RadioPacket &pkt,
                                         SendWindow &sendw,
                                         MonoClock::time_point tfeedback)
{
    Node &node = sendw.node;
    Seq  nextSeq = sendw.unack;
    bool sawACKRun = false;

    for(auto it = pkt.begin(); it != pkt.end(); ++it) {
        switch (it->type) {
            case ControlMsg::Type::kSelectiveAck:
            {
                if (!sawACKRun)
                    logEvent("ARQ: selective ack: node=%u; per_end=%u",
                        (unsigned) node.id,
                        (unsigned) sendw.per_end);

                // Record the gap between the last packet in the previous ACK
                // run and the first packet in this ACK run as failures.
                if (nextSeq < it->ack.begin) {
                    logEvent("ARQ: selective nak: node=%u; seq=[%u,%u)",
                        (unsigned) node.id,
                        (unsigned) nextSeq,
                        (unsigned) it->ack.begin);

                    for (Seq seq = nextSeq; seq < it->ack.begin; ++seq) {
                        if (seq >= sendw.per_end) {
                            if (sendw[seq]) {
                                if (sendw[seq].timestamp < tfeedback) {
                                    // Record TX failure for PER
                                    txFailure(sendw);

                                    logEvent("ARQ: txFailure selective nak: node=%u; seq=%u",
                                        (unsigned) node.id,
                                        (unsigned) seq);

                                    // Retransmit the NAK'ed packet
                                    retransmit(sendw[seq]);

                                    // Move PER window forward
                                    sendw.per_end = seq + 1;
                                }
                            } else
                                // Move PER window forward
                                sendw.per_end = seq + 1;
                        }
                    }
                }

                // Mark every packet in this ACK run as a success.
                logEvent("ARQ: selective ack: node=%u; seq=[%u,%u)",
                    (unsigned) node.id,
                    (unsigned) it->ack.begin,
                    (unsigned) it->ack.end);

                for (Seq seq = it->ack.begin; seq < it->ack.end; ++seq) {
                    // Handle the ACK
                    if (seq >= sendw.unack)
                        handleACK(sendw, seq);

                    // Update our packet error rate to reflect successful TX
                    if (seq >= sendw.per_end && sendw[seq].timestamp < tfeedback) {
                        // Record TX success for PER
                        txSuccess(sendw);

                        // Move PER window forward
                        sendw.per_end = seq + 1;
                    }
                }

                // We've now handled at least one ACK run
                sawACKRun = true;
                nextSeq = it->ack.end;
            }
            break;

            default:
                break;
        }
    }
}

void SmartController::handleSetUnack(RadioPacket &pkt, RecvWindow &recvw)
{
    for(auto it = pkt.begin(); it != pkt.end(); ++it) {
        switch (it->type) {
            case ControlMsg::Type::kSetUnack:
            {
                Seq next_ack = it->unack.unack;

                if (next_ack > recvw.ack) {
                    logEvent("ARQ: set next ack: node=%u; next_ack=%u",
                        (unsigned) recvw.node.id,
                        (unsigned) next_ack);

                    for (Seq seq = recvw.ack; seq < next_ack; ++seq)
                        recvw[seq].reset();

                    recvw.ack = next_ack;
                }
            }
            break;

            default:
                break;
        }
    }
}

void SmartController::txSuccess(SendWindow &sendw)
{
    sendw.short_per.update(0.0);
    sendw.long_per.update(0.0);
}

void SmartController::txFailure(SendWindow &sendw)
{
    sendw.short_per.update(1.0);
    sendw.long_per.update(1.0);
}

void SmartController::updateMCS(SendWindow &sendw)
{
    Node   &node = sendw.node;
    double short_per = sendw.short_per.getValue();
    double long_per = sendw.long_per.getValue();

    if (short_per != sendw.prev_short_per || long_per != sendw.prev_long_per) {
        logEvent("AMC: updateMCS: node=%u; short per=%f (%u samples); long per=%f (%u samples)",
            node.id,
            short_per,
            sendw.short_per.getNSamples(),
            long_per,
            sendw.long_per.getNSamples());

        sendw.prev_short_per = short_per;
        sendw.prev_long_per = long_per;
    }

    // First for high PER, then test for low PER
    if (   sendw.short_per.getNSamples() >= sendw.short_per.getWindowSize()
        && short_per > mcsidx_down_per_threshold_) {
        // Perform hysteresis on future MCS increases by decreasing the
        // probability that we will transition to this MCS index.
        sendw.mcsidx_prob[sendw.mcsidx] =
            std::max(sendw.mcsidx_prob[sendw.mcsidx]*mcsidx_alpha_,
                     mcsidx_prob_floor_);

        logEvent("AMC: Transition probability for MCS: node=%u; index=%u; prob=%f",
            node.id,
            (unsigned) sendw.mcsidx,
            sendw.mcsidx_prob[sendw.mcsidx]);

        // Decrease MCS until we hit rock bottom or we hit an MCS that produces
        // packets too large to fit in a slot.
        unsigned n = 0; // Number of MCS levels to decrease

        while (sendw.mcsidx > n &&
               sendw.mcsidx - n > mcsidx_min_ &&
               phy_->mcs_table[sendw.mcsidx-(n+1)].valid) {
            // Increment number of MCS levels we will move down
            ++n;

            // If we don't have both an EVM threshold and EVM feedback from the
            // sender, stop. Otherwise, use our EVM information to decide if we
            // should decrease the MCS level further.
            evm_thresh_t &next_evm_threshold = evm_thresholds_[sendw.mcsidx-n];

            if (!next_evm_threshold || !sendw.long_evm || (*sendw.long_evm < *next_evm_threshold))
                break;
        }

        // Move down n MCS levels
        if (n != 0)
            moveDownMCS(sendw, n);
        else
            resetPEREstimates(sendw);
    } else if (   sendw.long_per.getNSamples() >= sendw.long_per.getWindowSize()
               && long_per < mcsidx_up_per_threshold_) {
        double old_prob = sendw.mcsidx_prob[sendw.mcsidx];

        // Set transition probability of current MCS index to 1.0 since we
        // successfully passed the long PER test
        sendw.mcsidx_prob[sendw.mcsidx] = 1.0;

        if (sendw.mcsidx_prob[sendw.mcsidx] != old_prob)
            logEvent("AMC: Transition probability for MCS: node=%u; index=%u; prob=%f",
                node.id,
                (unsigned) sendw.mcsidx,
                sendw.mcsidx_prob[sendw.mcsidx]);

        // Now we see if we can actually increase the MCS index.
        if (mayMoveUpMCS(sendw))
            moveUpMCS(sendw);
        else
            resetPEREstimates(sendw);
    }
}

bool SmartController::mayMoveUpMCS(const SendWindow &sendw)
{
    // We can't move up if we're at the top of the MCS hierarchy...
    if (sendw.mcsidx == mcsidx_max_ || sendw.mcsidx == phy_->mcs_table.size() - 1)
        return false;

    // There are two cases where we may move up an MCS level:
    //
    // 1) The next-higher MCS has an EVM threshold that we meet
    // 2) The next-higher MCS *does not* have an EVM threshold, but we pass
    //    the probabilistic transition test.
    evm_thresh_t &next_evm_threshold = evm_thresholds_[sendw.mcsidx+1];

    if (next_evm_threshold) {
        if (sendw.long_evm) {
            logEvent("ARQ: EVM threshold: evm_threshold=%f, evm=%f",
                *next_evm_threshold,
                *sendw.long_evm);

            return *sendw.long_evm < *next_evm_threshold;
        } else
            return false;
    }

    return dist_(gen_) < sendw.mcsidx_prob[sendw.mcsidx+1];
}

void SmartController::moveDownMCS(SendWindow &sendw, unsigned n)
{
    Node &node = sendw.node;

    if (rc.verbose && !rc.debug)
        fprintf(stderr, "Moving down modulation scheme\n");

    assert(sendw.mcsidx >= n);

    logEvent("AMC: Moving down modulation scheme: node=%u; mcsidx=%u; short per=%f; swin=%lu; lwin=%lu",
        node.id,
        (unsigned) sendw.mcsidx,
        sendw.short_per.getValue(),
        sendw.short_per.getWindowSize(),
        sendw.long_per.getWindowSize());

    setMCS(sendw, sendw.mcsidx - n);

    const MCS *mcs = phy_->mcs_table[sendw.mcsidx].mcs;

    logEvent("AMC: Moved down modulation scheme: node=%u; mcsidx=%u; mcs=%s; unack=%u; init_seq=%u; swin=%lu; lwin=%lu",
        node.id,
        (unsigned) sendw.mcsidx,
        mcs->description().c_str(),
        (unsigned) sendw.unack,
        (unsigned) sendw.per_end,
        sendw.short_per.getWindowSize(),
        sendw.long_per.getWindowSize());
}

void SmartController::moveUpMCS(SendWindow &sendw)
{
    Node &node = sendw.node;

    if (rc.verbose && !rc.debug)
        fprintf(stderr, "Moving up modulation scheme\n");

    logEvent("AMC: Moving up modulation scheme: node=%u; mcsidx=%u; long per=%f; swin=%lu; lwin=%lu",
        node.id,
        (unsigned) sendw.mcsidx,
        sendw.long_per.getValue(),
        sendw.short_per.getWindowSize(),
        sendw.long_per.getWindowSize());

    setMCS(sendw, sendw.mcsidx + 1);

    const MCS *mcs = phy_->mcs_table[sendw.mcsidx].mcs;

    logEvent("AMC: Moved up modulation scheme: node=%u; mcsidx=%u; mcs=%s; unack=%u; init_seq=%u; swin=%lu; lwin=%lu",
        node.id,
        (unsigned) sendw.mcsidx,
        mcs->description().c_str(),
        (unsigned) sendw.unack,
        (unsigned) sendw.per_end,
        sendw.short_per.getWindowSize(),
        sendw.long_per.getWindowSize());
}

void SmartController::setMCS(SendWindow &sendw, size_t mcsidx)
{
    Node &node = sendw.node;

    assert(mcsidx >= 0);
    assert(mcsidx < phy_->mcs_table.size());

    // Move MCS up until we reach a valid MCS
    while (mcsidx < phy_->mcs_table.size() - 1 && !phy_->mcs_table[mcsidx].valid)
        ++mcsidx;

    sendw.mcsidx = mcsidx;
    sendw.per_end = sendw.seq;

    resetPEREstimates(sendw);

    node.mcsidx = sendw.mcsidx;

    const MCS *mcs = phy_->mcs_table[sendw.mcsidx].mcs;

    netq_->updateMCS(node.id, mcs);
}

void SmartController::resetPEREstimates(SendWindow &sendw)
{
    sendw.short_per.setWindowSize(std::max(1.0, short_per_window_*min_channel_bandwidth_/max_packet_samples_[sendw.mcsidx]));
    sendw.short_per.reset(0);

    sendw.long_per.setWindowSize(std::max(1.0, long_per_window_*min_channel_bandwidth_/max_packet_samples_[sendw.mcsidx]));
    sendw.long_per.reset(0);
}

bool SmartController::getPacket(std::shared_ptr<NetPacket>& pkt)
{
    for (;;) {
        // We use a lock here to protect against a race between getting a packet
        // and updating the send window status of the destination. Without this
        // lock, it's possible that we receive two packets for the same
        // destination before we are able to close it's send window while
        // waiting for an ACK.
        std::unique_lock<std::mutex> net_lock(net_mutex_);

        // Get a packet from the network
        if (!net_in.pull(pkt))
            return false;

        assert(pkt);

        // We can always send a broadcast packet
        if (pkt->hdr.nexthop == kNodeBroadcast)
            return true;

        SendWindow                      &sendw = getSendWindow(pkt->hdr.nexthop);
        std::lock_guard<spinlock_mutex> lock(sendw.mutex);

        // If packet has no payload, we can always send it---it has control
        // information.
        if (pkt->ehdr().data_len == 0)
            return true;

        // Set the packet sequence number if it doesn't yet have one.
        if (!pkt->internal_flags.has_seq) {
            // If we can't fit this packet in our window, move the window along
            // by dropping the oldest packet.
            if (   sendw.seq >= sendw.unack + sendw.win
                && sendw[sendw.unack].mayDrop(max_retransmissions_)) {
                logEvent("ARQ: MOVING WINDOW ALONG: node=%u",
                    (unsigned) pkt->hdr.nexthop);
                drop(sendw[sendw.unack]);
            }

            pkt->hdr.seq = sendw.seq++;
            pkt->internal_flags.has_seq = 1;

            // If this is the first packet we are sending to the destination,
            // set its SYN flag
            if (sendw.new_window) {
                pkt->hdr.flags.syn = 1;
                sendw.new_window = false;
            }

            // Close the send window if it's full and we're not supposed to
            // "move along." However, if the send window is only 1 packet,
            // ALWAYS close it since we're waiting for the ACK to our SYN!
            if (   sendw.seq >= sendw.unack + sendw.win
                && ((sendw[sendw.unack] && !sendw[sendw.unack].mayDrop(max_retransmissions_)) || !move_along_ || sendw.win == 1))
                netq_->setSendWindowStatus(pkt->hdr.nexthop, false);

            return true;
        } else {
            // If this packet comes before our window, drop it. It could have
            // snuck in as a retransmission just before the send window moved
            // forward. Try again!
            if (pkt->hdr.seq < sendw.unack) {
                pkt.reset();
                continue;
            }

            // Otherwise it had better be in our window becasue we added it back
            // when our window expanded due to an ACK!
            if(pkt->hdr.seq >= sendw.unack + sendw.win) {
                logEvent("ARQ: INVARIANT VIOLATED: got packet outside window: seq=%u; unack=%u; win=%u",
                    (unsigned) pkt->hdr.seq,
                    (unsigned) sendw.unack,
                    (unsigned) sendw.win);

                pkt.reset();
                continue;
            }

            // See if this packet should be dropped. The network queue won't
            // drop a packet with a sequence number, because we need to drop a
            // packet with a sequence number in the controller to ensure the
            // send window is properly adjusted.
            if (pkt->shouldDrop(MonoClock::now())) {
                drop(sendw[pkt->hdr.seq]);
                pkt.reset();
                continue;
            }

            return true;
        }
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
        Node       &dest = (*net_)[node_id];
        SendWindow &sendw = send_.emplace(std::piecewise_construct,
                                          std::forward_as_tuple(node_id),
                                          std::forward_as_tuple(dest,
                                                                *this,
                                                                max_sendwin_,
                                                                retransmission_delay_)).first->second;

        sendw.mcsidx_prob.resize(phy_->mcs_table.size(), 1.0);

        setMCS(sendw, mcsidx_init_);

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

RecvWindow &SmartController::getReceiveWindow(NodeId node_id, Seq seq, bool isSYN)
{
    std::lock_guard<spinlock_mutex> lock(recv_mutex_);
    auto                            it = recv_.find(node_id);

    // XXX If we have a receive window for this source use it. The exception is
    // when we either see a SYN packet or a sequence number that is outside the
    // receive window. In that case, assume the sender restarted and re-create
    // the receive window. This could cause an issue if we see a re-transmission
    // of the first packet after the sender has advanced its window. This should
    // not happen because the sender will only open up its window if it has seen
    // its SYN packet ACK'ed.
    if (it != recv_.end()) {
        RecvWindow                      &recvw = it->second;
        std::lock_guard<spinlock_mutex> lock(recvw.mutex);

        if (!isSYN || (seq >= recvw.max - recvw.win && seq < recvw.ack + recvw.win))
            return recvw;
        else {
            // This is a new connection, so cancel selective ACK timer for the
            // old receive window
            timer_queue_.cancel(recvw);

            // Delete the old receive window
            recv_.erase(it);
        }
    }

    Node       &src = (*net_)[node_id];
    RecvWindow &recvw = recv_.emplace(std::piecewise_construct,
                                      std::forward_as_tuple(node_id),
                                      std::forward_as_tuple(src, *this, seq, recvwin_, explicit_nak_win_)).first->second;

    recvw.long_evm.setTimeWindow(long_stats_window_);
    recvw.long_rssi.setTimeWindow(long_stats_window_);

    return recvw;
}
