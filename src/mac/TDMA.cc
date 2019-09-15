#include "Clock.hh"
#include "USRP.hh"
#include "RadioConfig.hh"
#include "Util.hh"
#include "mac/TDMA.hh"

TDMA::TDMA(std::shared_ptr<USRP> usrp,
           std::shared_ptr<PHY> phy,
           std::shared_ptr<Controller> controller,
           std::shared_ptr<SnapshotCollector> collector,
           std::shared_ptr<Channelizer> channelizer,
           std::shared_ptr<Synthesizer> synthesizer,
           double slot_size,
           double guard_size,
           double slot_modulate_lead_time,
           double slot_send_lead_time,
           size_t nslots)
  : SlottedMAC(usrp,
               phy,
               controller,
               collector,
               channelizer,
               synthesizer,
               slot_size,
               guard_size,
               slot_modulate_lead_time,
               slot_send_lead_time)
  , nslots_(nslots)
  , tdma_schedule_(nslots)
{
    rx_thread_ = std::thread(&TDMA::rxWorker, this);
    tx_thread_ = std::thread(&TDMA::txWorker, this);
    tx_slot_thread_ = std::thread(&TDMA::txSlotWorker, this);
    tx_notifier_thread_ = std::thread(&TDMA::txNotifier, this);
}

TDMA::~TDMA()
{
    stop();
}

void TDMA::stop(void)
{
    done_ = true;

    txed_slots_cond_.notify_all();

    if (rx_thread_.joinable())
        rx_thread_.join();

    if (tx_thread_.joinable())
        tx_thread_.join();

    if (tx_slot_thread_.joinable())
        tx_slot_thread_.join();

    if (tx_notifier_thread_.joinable())
        tx_notifier_thread_.join();
}

void TDMA::reconfigure(void)
{
    SlottedMAC::reconfigure();

    for (size_t i = 0; i < nslots_; ++i)
        tdma_schedule_[i] = schedule_.canTransmit(i);

    frame_size_ = nslots_*slot_size_;

    // Determine whether or not we have a slot
    Clock::time_point t_now = Clock::now();
    Clock::time_point t_next_slot;
    size_t            next_slotidx;
    Clock::time_point t_following_slot;
    size_t            following_slotidx;

    can_transmit_ = findNextSlot(t_now,
                                 t_next_slot, next_slotidx,
                                 t_following_slot, following_slotidx);
}

void TDMA::txSlotWorker(void)
{
    slot_queue        q;
    Clock::time_point t_now;              // Current time
    Clock::time_point t_prev_slot;        // Previous, completed slot
    Clock::time_point t_next_slot;        // Time at which our next slot starts
    Clock::time_point t_following_slot;   // Time at which our following slot starts
    size_t            next_slotidx;       // Slot index of next slot
    size_t            following_slotidx;  // Slot index of following slot
    size_t            noverfill = 0;      // Number of overfilled samples;

    makeThisThreadHighPriority();

    while (!done_) {
        t_prev_slot = Clock::time_point { 0.0 };

        while (!done_) {
            // Figure out when our next send slot is.
            t_now = Clock::now();

            if (!findNextSlot(t_now,
                              t_next_slot, next_slotidx,
                              t_following_slot, following_slotidx)) {
                // Sleep for 100ms if we don't yet have a slot
                doze(100e-3);
                continue;
            }

            // Finalize next slot. After this returns, we have EXCLUSIVE access
            // to the slot.
            auto slot = finalizeSlot(q, t_next_slot);

            // Schedule modulation of the following slot
            if (slot)
                noverfill = slot->length() < slot->max_samples ? 0 : slot->length() - slot->max_samples;
            else
                noverfill = 0;

            // Schedule modulation of following slot
            if (!approx(t_following_slot, t_prev_slot)) {
                modulateSlot(q,
                             t_following_slot,
                             noverfill,
                             following_slotidx);

                t_prev_slot = t_following_slot;
            }

            // Transmit next slot
            if (slot)
                txSlot(std::move(slot));

            // If we had a TX error, restart the TX loop
            if (usrp_->getTXLateCount() != 0)
                break;

            // Sleep until it's time to send the following slot
            double delta;

            t_now = Clock::now();
            delta = (t_following_slot - t_now).get_real_secs() - slot_send_lead_time_;
            if (delta > 0.0)
                doze(delta);
        }

        // Attempt to deal with TX errors
        logEvent("MAC: attempting to reset TX loop");
        doze(slot_size_/2.0);
    }

    missedRemainingSlots(q);
}

bool TDMA::findNextSlot(Clock::time_point t,
                        Clock::time_point &t_next,
                        size_t &next_slotidx,
                        Clock::time_point &t_following,
                        size_t &following_slotidx)
{
    double t_slot_pos; // Offset into the current slot (sec)
    size_t cur_slot;   // Current slot index
    size_t tx_slot;    // Slots before we can TX

    t_slot_pos = fmod(t, slot_size_);
    cur_slot = fmod(t, frame_size_) / slot_size_;

    for (tx_slot = 1; tx_slot <= nslots_; ++tx_slot) {
        if (tdma_schedule_[(cur_slot + tx_slot) % nslots_]) {
            t_next = t + (tx_slot*slot_size_ - t_slot_pos);
            cur_slot = next_slotidx = (cur_slot + tx_slot) % nslots_;
            goto following;
        }
    }

    return false;

following:
    for (tx_slot = 1; tx_slot <= nslots_; ++tx_slot) {
        if (tdma_schedule_[(cur_slot + tx_slot) % nslots_]) {
            t_following = t_next + tx_slot*slot_size_;
            following_slotidx = (cur_slot + tx_slot) % nslots_;
            return true;
        }
    }

    assert(false);
    return false;
}
