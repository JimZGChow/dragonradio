#ifndef FLEXFRAME_H_
#define FLEXFRAME_H_

#include "liquid/FlexFrame.hh"
#include "phy/LiquidPHY.hh"

/** @brief A %PHY thats uses the liquid-usrp flexframegen code. */
class FlexFrame : public LiquidPHY {
public:
    /** @brief Modulate IQ data using a liquid-usrp flexframe. */
    class Modulator : public LiquidPHY::Modulator, protected Liquid::FlexFrameModulator
    {
    public:
        Modulator(FlexFrame& phy)
          : LiquidPHY::Modulator(phy)
          , Liquid::FlexFrameModulator()
        {
        }

        virtual ~Modulator() = default;
    };

    /** @brief Demodulate IQ data using a liquid-usrp flexframe. */
    class Demodulator : public LiquidPHY::Demodulator, protected Liquid::FlexFrameDemodulator
    {
    public:
        Demodulator(FlexFrame &phy)
          : Liquid::Demodulator(phy.soft_header_,
                                phy.soft_payload_)
          , LiquidPHY::Demodulator(phy)
          , Liquid::FlexFrameDemodulator(phy.soft_header_, phy.soft_payload_)
        {
        }

        virtual ~Demodulator() = default;

        bool isFrameOpen(void) override final
        {
            return Liquid::FlexFrameDemodulator::isFrameOpen();
        }
    };

    FlexFrame(std::shared_ptr<SnapshotCollector> collector,
              NodeId node_id,
              const MCS &header_mcs,
              bool soft_header,
              bool soft_payload,
              size_t min_packet_size)
      : LiquidPHY(collector, node_id, header_mcs, soft_header, soft_payload, min_packet_size)
    {
    }

    virtual ~FlexFrame() = default;

    unsigned getMinRXRateOversample(void) const override
    {
        return 2;
    }

    unsigned getMinTXRateOversample(void) const override
    {
        return 2;
    }

protected:
    std::shared_ptr<PHY::Demodulator> mkDemodulatorInternal(void) override
    {
        return std::make_shared<Demodulator>(*this);
    }

    std::shared_ptr<PHY::Modulator> mkModulatorInternal(void) override
    {
        return std::make_shared<Modulator>(*this);
    }

    std::unique_ptr<Liquid::Modulator> mkLiquidModulator(void) override
    {
        return std::make_unique<Liquid::FlexFrameModulator>();
    }
};

#endif /* FLEXFRAME_H_ */
