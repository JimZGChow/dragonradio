#ifndef PACKETMODULATOR_H_
#define PACKETMODULATOR_H_

#include "phy/ModPacket.hh"

/** @brief A packet modulator. */
class PacketModulator
{
public:
    PacketModulator(void) : maxPacketSize_(0) {};
    virtual ~PacketModulator() {};

    /** @brief Get the low-water mark. */
    virtual size_t getLowWaterMark(void) = 0;

    /** @brief Set the low-water mark, i.e., the minimum number of samples we
     * want to have available at all times.
     */
    virtual void setLowWaterMark(size_t mark) = 0;

    /** @brief Pop a list of modulated packet such that the total number of
     * modulated samples is maxSamples or fewer.
     * @param pkts A reference to a list to which the popped packets will be
     * appended.
     * @param maxSample The maximum number of samples to pop.
     */
    virtual void pop(std::list<std::unique_ptr<ModPacket>>& pkts, size_t maxSamples) = 0;

    /** @brief Set maximum packet size. */
    void setMaxPacketSize(size_t maxPacketSize)
    {
        maxPacketSize_ = maxPacketSize;
    }

    /** @brief Get maximum packet size. */
    size_t getMaxPacketSize(void)
    {
        return maxPacketSize_;
    }

protected:
    /** @brief Maximum number of possible samples in a modulated packet. */
    size_t maxPacketSize_;
};

#endif /* PACKETMODULATOR_H_ */
