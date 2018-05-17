#ifndef PARALLELPACKETMODULATOR_H_
#define PARALLELPACKETMODULATOR_H_

#include <condition_variable>
#include <mutex>
#include <queue>

#include "PacketModulator.hh"
#include "phy/PHY.hh"
#include "net/Net.hh"

/** @brief A parallel packet modulator. */
class ParallelPacketModulator : public PacketModulator, public Element
{
public:
    ParallelPacketModulator(std::shared_ptr<Net> net,
                            std::shared_ptr<PHY> phy,
                            size_t nthreads);
    virtual ~ParallelPacketModulator();

    size_t getLowWaterMark(void) override;

    void setLowWaterMark(size_t mark) override;

    void pop(std::list<std::unique_ptr<ModPacket>>& pkts, size_t maxSamples) override;

    /** @brief Stop modulating. */
    void stop(void);

    /** @brief Input port for packets. */
    NetIn<Pull> sink;

private:
    /** @brief Our network. */
    std::shared_ptr<Net> net_;

    /** @brief Our PHY. */
    std::shared_ptr<PHY> phy_;

    /** @brief Flag indicating if we should stop processing packets */
    bool done_;

    /** @brief Thread running modWorker */
    std::vector<std::thread> mod_threads_;

    /** @brief Number of modulated samples we want to have on-hand at all times. */
    size_t low_water_mark_;

    /** @brief Number of modulated samples we have */
    size_t nsamples_;

    /** @brief Mutex to serialize access to the network */
    std::mutex net_mutex_;

    /* @brief Mutex protecting queue of modulated packets */
    std::mutex pkt_mutex_;

    /* @brief Condition variable used to signal modulation workers */
    std::condition_variable producer_cond_;

    /* @brief Queue of modulated packets */
    std::queue<std::unique_ptr<ModPacket>> pkt_q_;

    /** @brief Thread modulating packets */
    void modWorker(void);
};

#endif /* PARALLELPACKETMODULATOR_H_ */
