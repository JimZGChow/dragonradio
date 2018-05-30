#include <uhd/utils/thread_priority.hpp>

#include "Logger.hh"
#include "SlottedMAC.hh"

SlottedMAC::SlottedMAC(std::shared_ptr<USRP> usrp,
                       std::shared_ptr<PHY> phy,
                       std::shared_ptr<PacketModulator> modulator,
                       std::shared_ptr<PacketDemodulator> demodulator,
                       double bandwidth,
                       double slot_size,
                       double guard_size)
  : MAC(usrp, phy, modulator, demodulator, bandwidth)
  , slot_size_(slot_size)
  , guard_size_(guard_size)
  , done_(false)
{
}

SlottedMAC::~SlottedMAC()
{
}

double SlottedMAC::getSlotSize(void)
{
    return slot_size_;
}

void SlottedMAC::setSlotSize(double t)
{
    slot_size_ = t;
    reconfigure();
}

double SlottedMAC::getGuardSize(void)
{
    return guard_size_;
}

void SlottedMAC::setGuardSize(double t)
{
    guard_size_ = t;
    reconfigure();
}

void SlottedMAC::reconfigure(void)
{
    rx_slot_samps_ = rx_rate_*slot_size_;
    tx_slot_samps_ = tx_rate_*(slot_size_ - guard_size_);

    modulator_->setLowWaterMark(tx_slot_samps_);

    demodulator_->setWindowParameters(0.5*guard_size_*rx_rate_,
                                      (slot_size_ - 0.5*guard_size_)*tx_rate_);
}

void SlottedMAC::rxWorker(void)
{
    Clock::time_point t_now;        // Current time
    Clock::time_point t_cur_slot;   // Time at which current slot starts
    Clock::time_point t_next_slot;  // Time at which next slot starts
    double            t_slot_pos;   // Offset into the current slot (sec)

    uhd::set_thread_priority_safe();

    while (!done_) {
        // Set up streaming starting at *next* slot
        t_now = Clock::now();
        t_slot_pos = fmod(t_now.get_real_secs(), slot_size_);
        t_next_slot = t_now + slot_size_ - t_slot_pos;

        usrp_->startRXStream(t_next_slot);

        while (!done_) {
            // Update times
            t_now = Clock::now();
            t_cur_slot = t_next_slot;
            t_next_slot += slot_size_;

            // Read samples for current slot
            auto curSlot = std::make_shared<IQBuf>(rx_slot_samps_ + usrp_->getMaxRXSamps());

            demodulator_->push(curSlot);

            usrp_->burstRX(t_cur_slot, rx_slot_samps_, *curSlot);
        }

        usrp_->stopRXStream();
    }
}

void SlottedMAC::txSlot(Clock::time_point when, size_t maxSamples)
{
    std::list<std::shared_ptr<IQBuf>>     txBuf;
    std::list<std::unique_ptr<ModPacket>> modBuf;

    modulator_->pop(modBuf, maxSamples);

    if (!modBuf.empty()) {
        for (auto it = modBuf.begin(); it != modBuf.end(); ++it) {
            if (logger) {
                Header hdr;

                hdr.curhop = (*it)->pkt->curhop;
                hdr.nexthop = (*it)->pkt->nexthop;
                hdr.seq = (*it)->pkt->seq;

                logger->logSend(when,
                                hdr,
                                (*it)->pkt->src,
                                (*it)->pkt->dest,
                                (*it)->samples);
            }

            txBuf.emplace_back(std::move((*it)->samples));
        }

        usrp_->burstTX(when, txBuf);
    }
}