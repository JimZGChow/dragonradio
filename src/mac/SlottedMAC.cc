#include <uhd/utils/thread_priority.hpp>

#include "Logger.hh"
#include "SlottedMAC.hh"

SlottedMAC::SlottedMAC(std::shared_ptr<USRP> usrp,
                       std::shared_ptr<PHY> phy,
                       std::shared_ptr<Controller> controller,
                       std::shared_ptr<SnapshotCollector> collector,
                       std::shared_ptr<Channelizer> channelizer,
                       std::shared_ptr<Synthesizer> synthesizer,
                       double slot_size,
                       double guard_size,
                       double slot_modulate_lead_time,
                       double slot_send_lead_time)
  : MAC(usrp,
        phy,
        controller,
        collector,
        channelizer,
        synthesizer)
  , slot_size_(slot_size)
  , guard_size_(guard_size)
  , slot_modulate_lead_time_(slot_modulate_lead_time)
  , slot_send_lead_time_(slot_send_lead_time)
  , rx_slot_samps_(0)
  , rx_bufsize_(0)
  , tx_slot_samps_(0)
  , tx_full_slot_samps_(0)
  , next_slot_start_of_burst_(true)
  , logger_(logger)
  , done_(false)
{
}

SlottedMAC::~SlottedMAC()
{
    // Mark all remaining packets in un-finalized slots as missed
    std::lock_guard<spinlock_mutex> lock(slots_mutex_);

    while (!slots_.empty()) {
        missedSlot(*slots_.front());
        slots_.pop();
    }
}

void SlottedMAC::reconfigure(void)
{
    MAC::reconfigure();

    rx_slot_samps_ = rx_rate_*slot_size_;
    rx_bufsize_ = usrp_->getRecommendedBurstRXSize(rx_slot_samps_);
    tx_slot_samps_ = tx_rate_*(slot_size_ - guard_size_);
    tx_full_slot_samps_ = tx_rate_*slot_size_;

    if (usrp_->getTXRate() == usrp_->getRXRate())
        tx_fc_off_ = std::nullopt;
    else
        tx_fc_off_ = usrp_->getTXFrequency() - usrp_->getRXFrequency();

    synthesizer_->setMaxPacketSize(tx_slot_samps_);
}

void SlottedMAC::rxWorker(void)
{
    Clock::time_point t_now;        // Current time
    Clock::time_point t_cur_slot;   // Time at which current slot starts
    Clock::time_point t_next_slot;  // Time at which next slot starts
    double            t_slot_pos;   // Offset into the current slot (sec)
    unsigned          seq = 0;      // Current IQ buffer sequence number

    uhd::set_thread_priority_safe();

    while (!done_) {
        // Set up streaming starting at *next* slot
        t_now = Clock::now();
        t_slot_pos = fmod(t_now, slot_size_);
        t_next_slot = t_now + slot_size_ - t_slot_pos;

        // Bump the sequence number to indicate a discontinuity
        seq++;

        usrp_->startRXStream(Clock::to_mono_time(t_next_slot));

        while (!done_) {
            // Update times
            t_now = Clock::now();
            t_cur_slot = t_next_slot;
            t_next_slot += slot_size_;

            // Create buffer for slot
            auto curSlot = std::make_shared<IQBuf>(rx_bufsize_);

            curSlot->seq = seq++;

            // Push the buffer if we're snapshotting
            bool do_snapshot;

            if (snapshot_collector_)
                do_snapshot = snapshot_collector_->push(curSlot);
            else
                do_snapshot = false;

            // Put the buffer into the channelizer's queue so it can start
            // working now
            channelizer_->push(curSlot);

            // Read samples for current slot. The demodulator will do its thing
            // as we continue to read samples.
            bool ok = usrp_->burstRX(Clock::to_mono_time(t_cur_slot), rx_slot_samps_, *curSlot);

            // Update snapshot offset by finalizing this snapshot slot
            if (do_snapshot)
                snapshot_collector_->finalizePush();

            // If there was an RX error, break and set up the RX stream again.
            if (!ok)
                break;
        }

        usrp_->stopRXStream();
    }
}

void SlottedMAC::modulateSlot(Clock::time_point when,
                              size_t prev_overfill,
                              bool owns_next_slot)
{
    assert(prev_overfill <= tx_slot_samps_);
    assert(prev_overfill <= tx_full_slot_samps_);

    size_t max_samples = owns_next_slot ? tx_full_slot_samps_ - prev_overfill : tx_slot_samps_ - prev_overfill;
    auto   slot = std::make_shared<Synthesizer::Slot>(when, prev_overfill, max_samples, owns_next_slot);

    // Tell the synthesizer to synthesize for this slot
    synthesizer_->modulate(slot);

    std::lock_guard<spinlock_mutex> lock(slots_mutex_);

    slots_.emplace(std::move(slot));
}

std::shared_ptr<Synthesizer::Slot> SlottedMAC::finalizeSlot(Clock::time_point when)
{
    std::shared_ptr<Synthesizer::Slot> slot;
    Clock::time_point                  deadline;

    for (;;) {
        // Get the next slot
        {
            std::lock_guard<spinlock_mutex> lock(slots_mutex_);

            // If we don't have any slots synthesized, we can't send anything
            if (slots_.empty())
                return nullptr;

            // Check deadline of next slot
            deadline = slots_.front()->deadline;

            // If the next slot needs to be transmitted or tossed, pop it,
            // otherwise return nullptr since we need to wait longer
            if (deadline < when || approx(deadline, when)) {
                slot = std::move(slots_.front());
                slots_.pop();
            } else
                return nullptr;
        }

        // Close the slot. We grab the slot's mutex to guarantee that all
        // synthesizer threads have seen that the slot is closed---this serves
        // as a barrier. After this, no synthesizer will touch the slot, so we
        // are guaranteed exclusive access.
        {
            std::lock_guard<spinlock_mutex> lock(slot->mutex);

            slot->closed.store(true, std::memory_order_relaxed);
        }

        // Finalize the slot
        synthesizer_->finalize(*slot);

        // If the slot's deadline has passed, try the next slot. Otherwise,
        // return the slot.
        if (approx(deadline, when)) {
            return slot;
        } else {
            logEvent("MAC: MISSED SLOT DEADLINE: deadline=%f; slot=%f; now=%f",
                (double) deadline.get_real_secs(),
                (double) when.get_real_secs(),
                (double) Clock::now().get_real_secs());

            // Stop any current TX burst. Also, the next slot is definitely the
            // start of a burst since we missed this slot.
            usrp_->stopTXBurst();
            next_slot_start_of_burst_ = true;

            // Re-queue packets that were modulated for this slot
            missedSlot(*slot);
        }
    }
}

void SlottedMAC::txSlot(std::shared_ptr<Synthesizer::Slot> &&slot)
{
    // If the slot doesn't contain any IQ data to send, we're done
    if (slot->mpkts.empty()) {
        if (!next_slot_start_of_burst_)
            usrp_->stopTXBurst();

        next_slot_start_of_burst_ = true;
        return;
    }

    // Transmit the packets via the USRP
    bool end_of_burst = slot->nsamples < slot->max_samples || !slot->overfill;

    usrp_->burstTX(Clock::to_mono_time(slot->deadline) + slot->delay/tx_rate_,
                   next_slot_start_of_burst_,
                   end_of_burst,
                   slot->iqbufs);

    next_slot_start_of_burst_ = end_of_burst;

    // Log the transmissions
    if (logger_ && logger_->getCollectSource(Logger::kSentPackets)) {
        // Log the sent packets
        for (auto it = slot->mpkts.begin(); it != slot->mpkts.end(); ++it) {
            Header hdr;

            hdr.curhop = (*it)->pkt->curhop;
            hdr.nexthop = (*it)->pkt->nexthop;
            hdr.seq = (*it)->pkt->seq;

            logger_->logSend(Clock::to_wall_time((*it)->samples->timestamp),
                             hdr,
                             (*it)->pkt->src,
                             (*it)->pkt->dest,
                             (*it)->pkt->tx_params->mcs.check,
                             (*it)->pkt->tx_params->mcs.fec0,
                             (*it)->pkt->tx_params->mcs.fec1,
                             (*it)->pkt->tx_params->mcs.ms,
                             tx_fc_off_ ? *tx_fc_off_ : (*it)->channel.fc,
                             tx_rate_,
                             (*it)->pkt->size(),
                             (*it)->samples);
        }
    }

    // Inform the controller of the transmission
    for (auto it = slot->mpkts.begin(); it != slot->mpkts.end(); ++it)
        controller_->transmitted((*it)->pkt);

    // Tell the snapshot collector about local self-transmissions
    if (snapshot_collector_) {
        for (auto it = slot->mpkts.begin(); it != slot->mpkts.end(); ++it)
            snapshot_collector_->selfTX(Clock::to_mono_time(slot->deadline) + (*it)->start/tx_rate_,
                                        rx_rate_,
                                        tx_rate_,
                                        (*it)->channel.bw,
                                        (*it)->nsamples,
                                        tx_fc_off_ ? *tx_fc_off_ : (*it)->channel.fc);
    }
}

void SlottedMAC::missedSlot(Synthesizer::Slot &slot)
{
    std::lock_guard<spinlock_mutex> lock(slot.mutex);

    // Close the slot
    slot.closed.store(true, std::memory_order_relaxed);

    // Re-queue packets that were modulated for this slot
    for (auto it = slot.mpkts.begin(); it != slot.mpkts.end(); ++it) {
        if (!(*it)->pkt->isInternalFlagSet(kIsTimestamp))
            controller_->missed(std::move((*it)->pkt));
    }
}
