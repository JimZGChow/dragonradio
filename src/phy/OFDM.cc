#include <liquid/liquid.h>

#include "Logger.hh"
#include "phy/Liquid.hh"
#include "phy/OFDM.hh"

union PHYHeader {
    Header h;
    // OFDMFLEXFRAME_H_USER in liquid.internal.h
    unsigned char bytes[8];
};

OFDM::Modulator::Modulator(OFDM& phy)
  : LiquidModulator(phy)
  , myphy_(phy)
{
    std::lock_guard<std::mutex> lck(liquid_mutex);

    ofdmflexframegenprops_init_default(&fgprops_);
    fg_ = ofdmflexframegen_create(myphy_.M_,
                                  myphy_.cp_len_,
                                  myphy_.taper_len_,
                                  myphy_.p_,
                                  &fgprops_);

#if LIQUID_VERSION_NUMBER >= 1003001
    ofdmflexframegenprops_s header_props { phy.header_mcs_.check
                                         , phy.header_mcs_.fec0
                                         , phy.header_mcs_.fec1
                                         , phy.header_mcs_.ms
                                         };

    ofdmflexframegen_set_header_props(fg_, &header_props);
    ofdmflexframegen_set_header_len(fg_, sizeof(Header));
#endif /* LIQUID_VERSION_NUMBER >= 1003001 */
}

OFDM::Modulator::~Modulator()
{
    ofdmflexframegen_destroy(fg_);
}

void OFDM::Modulator::print(void)
{
    ofdmflexframegen_print(fg_);
}

void OFDM::Modulator::update_props(const TXParams &params)
{
    if (fgprops_.check != params.mcs.check ||
        fgprops_.fec0 != params.mcs.fec0 ||
        fgprops_.fec1 != params.mcs.fec1 ||
        fgprops_.mod_scheme != params.mcs.ms) {
        fgprops_.check = params.mcs.check;
        fgprops_.fec0 = params.mcs.fec0;
        fgprops_.fec1 = params.mcs.fec1;
        fgprops_.mod_scheme = params.mcs.ms;

        ofdmflexframegen_setprops(fg_, &fgprops_);
    }
}

// Initial sample buffer size
const size_t MODBUF_SIZE = 16384;

void OFDM::Modulator::modulate(ModPacket& mpkt, std::shared_ptr<NetPacket> pkt)
{
    PHYHeader header;

    memset(&header, 0, sizeof(header));

    pkt->toHeader(header.h);

    pkt->resize(std::max((size_t) pkt->size(), myphy_.min_pkt_size_));

    update_props(*(pkt->tx_params));
    ofdmflexframegen_reset(fg_);
    ofdmflexframegen_assemble(fg_, header.bytes, pkt->data(), pkt->size());

    // Buffer holding generated IQ samples
    auto iqbuf = std::make_unique<IQBuf>(MODBUF_SIZE);
    // Number of symbols generated
    const size_t NGEN = myphy_.M_ + myphy_.cp_len_;
    // Number of generated samples in the buffer
    size_t nsamples = 0;
    // Local copy of gain
    const float g = pkt->g;
    // Flag indicating when we've reached the last symbol
    bool last_symbol = false;

    while (!last_symbol) {
#if LIQUID_VERSION_NUMBER >= 1003000
        last_symbol = ofdmflexframegen_write(fg_,
          reinterpret_cast<liquid_float_complex*>(&(*iqbuf)[nsamples]), NGEN);
#else /* LIQUID_VERSION_NUMBER < 1003000 */
        last_symbol = ofdmflexframegen_writesymbol(fg_,
          reinterpret_cast<liquid_float_complex*>(&(*iqbuf)[nsamples]));
#endif /* LIQUID_VERSION_NUMBER < 1003000 */

        // Apply soft gain. Note that this is where nsamples is incremented.
        for (unsigned int i = 0; i < NGEN; i++)
            (*iqbuf)[nsamples++] *= g;

        // If we can't add another NGEN samples to the current IQ buffer, resize
        // it.
        if (nsamples + NGEN > iqbuf->size())
            iqbuf->resize(2*iqbuf->size());
    }

    // Resize the final buffer to the number of samples generated.
    iqbuf->resize(nsamples);

    // Fill in the ModPacket
    mpkt.samples = std::move(iqbuf);
    mpkt.pkt = std::move(pkt);
}

OFDM::Demodulator::Demodulator(OFDM& phy)
  : LiquidDemodulator(phy)
  , myphy_(phy)
{
    std::lock_guard<std::mutex> lck(liquid_mutex);

    fs_ = ofdmflexframesync_create(myphy_.M_,
                                   myphy_.cp_len_,
                                   myphy_.taper_len_,
                                   myphy_.p_,
                                   &LiquidDemodulator::liquid_callback,
                                   this);

#if LIQUID_VERSION_NUMBER >= 1003001
    ofdmflexframegenprops_s header_props { phy.header_mcs_.check
                                         , phy.header_mcs_.fec0
                                         , phy.header_mcs_.fec1
                                         , phy.header_mcs_.ms
                                         };

    ofdmflexframesync_set_header_props(fs_, &header_props);
    ofdmflexframesync_set_header_len(fs_, sizeof(Header));
    ofdmflexframesync_decode_header_soft(fs_, phy.soft_header_);
    ofdmflexframesync_decode_payload_soft(fs_, phy.soft_payload_);
#endif /* LIQUID_VERSION_NUMBER >= 1003001 */
}

OFDM::Demodulator::~Demodulator()
{
    ofdmflexframesync_destroy(fs_);
}

void OFDM::Demodulator::print(void)
{
    ofdmflexframesync_print(fs_);
}

void OFDM::Demodulator::reset(Clock::time_point timestamp, size_t off)
{
    ofdmflexframesync_reset(fs_);

    demod_start_ = timestamp;
    demod_off_ = off;
}

void OFDM::Demodulator::demodulate(std::complex<float>* data,
                                   size_t count,
                                   std::function<void(std::unique_ptr<RadioPacket>)> callback)
{
    callback_ = callback;

    ofdmflexframesync_execute(fs_, reinterpret_cast<liquid_float_complex*>(data), count);
}

std::unique_ptr<PHY::Demodulator> OFDM::make_demodulator(void)
{
    return std::unique_ptr<PHY::Demodulator>(static_cast<PHY::Demodulator*>(new Demodulator(*this)));
}

std::unique_ptr<PHY::Modulator> OFDM::make_modulator(void)
{
    return std::unique_ptr<PHY::Modulator>(static_cast<PHY::Modulator*>(new Modulator(*this)));
}
