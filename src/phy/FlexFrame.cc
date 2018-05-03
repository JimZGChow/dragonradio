#include <liquid/liquid.h>

#include "FlexFrame.hh"
#include "Liquid.hh"

union PHYHeader {
    Header        h;
    // FLEXFRAME_H_USER in liquid.internal.h
    unsigned char bytes[14];
};

FlexFrame::Modulator::Modulator(FlexFrame& phy) :
    _phy(phy),
    _g(1.0)
{
    std::lock_guard<std::mutex> lck(liquid_mutex);

    flexframegenprops_init_default(&_fgprops);
    _fg = flexframegen_create(&_fgprops);
}

FlexFrame::Modulator::Modulator(FlexFrame& phy,
                                crc_scheme check,
                                fec_scheme fec0,
                                fec_scheme fec1,
                                modulation_scheme ms) :
    _phy(phy),
    _g(1.0)
{
    std::lock_guard<std::mutex> lck(liquid_mutex);

    flexframegenprops_init_default(&_fgprops);
    _fgprops.check = check;
    _fgprops.fec0 = fec0;
    _fgprops.fec1 = fec1;
    _fgprops.mod_scheme = ms;

    _fg = flexframegen_create(&_fgprops);
}

FlexFrame::Modulator::~Modulator()
{
    flexframegen_destroy(_fg);
}

void FlexFrame::Modulator::setSoftTXGain(float dB)
{
    _g = powf(10.0f, dB/20.0f);
}

void FlexFrame::Modulator::print(void)
{
    flexframegen_print(_fg);
}

crc_scheme FlexFrame::Modulator::get_check(void)
{
    return static_cast<crc_scheme>(_fgprops.check);
}

void FlexFrame::Modulator::set_check(crc_scheme check)
{
    _fgprops.check = check;
    update_props();
}

fec_scheme FlexFrame::Modulator::get_fec0(void)
{
    return static_cast<fec_scheme>(_fgprops.fec0);
}

void FlexFrame::Modulator::set_fec0(fec_scheme fec0)
{
    _fgprops.fec0 = fec0;
    update_props();
}

fec_scheme FlexFrame::Modulator::get_fec1(void)
{
    return static_cast<fec_scheme>(_fgprops.fec1);
}

void FlexFrame::Modulator::set_fec1(fec_scheme fec1)
{
    _fgprops.fec1 = fec1;
    update_props();
}

modulation_scheme FlexFrame::Modulator::get_mod_scheme(void)
{
    return static_cast<modulation_scheme>(_fgprops.mod_scheme);
}

void FlexFrame::Modulator::set_mod_scheme(modulation_scheme ms)
{
    _fgprops.mod_scheme = ms;
    update_props();
}

void FlexFrame::Modulator::update_props(void)
{
    flexframegen_setprops(_fg, &_fgprops);
}

// Number of samples generated by a call to flexframegen_write_samples.
const size_t NGEN = 2;

// Initial sample buffer size
const size_t MODBUF_SIZE = 16384;

std::unique_ptr<ModPacket> FlexFrame::Modulator::modulate(std::unique_ptr<NetPacket> pkt)
{
    PHYHeader header;

    memset(&header, 0, sizeof(header));

    header.h.src = pkt->src;
    header.h.dest = pkt->dest;
    header.h.pkt_id = pkt->pkt_id;
    header.h.pkt_len = pkt->size();

    pkt->resize(std::max((size_t) pkt->size(), _phy._minPacketSize));

    flexframegen_reset(_fg);
    flexframegen_assemble(_fg, header.bytes, pkt->data(), pkt->size());

    // Buffer holding generated IQ samples
    auto iqbuf = std::make_unique<IQBuf>(MODBUF_SIZE);
    // Number of generated samples in the buffer
    size_t nsamples = 0;
    // Local copy of gain
    const float g = _g;
    // Flag indicating when we've reached the last symbol
    bool last_symbol = false;

    while (!last_symbol) {
        last_symbol = flexframegen_write_samples(_fg,
          reinterpret_cast<liquid_float_complex*>(&(*iqbuf)[nsamples]));

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

    // Construct and return the ModPacket
    auto mpkt = std::make_unique<ModPacket>();

    mpkt->samples = std::move(iqbuf);
    mpkt->pkt = std::move(pkt);

    return mpkt;
}

FlexFrame::Demodulator::Demodulator(FlexFrame& phy) :
  _phy(phy)
{
    std::lock_guard<std::mutex> lck(liquid_mutex);

    _fs = flexframesync_create(&Demodulator::liquid_callback, this);
}

FlexFrame::Demodulator::~Demodulator()
{
    flexframesync_destroy(_fs);
}

void FlexFrame::Demodulator::print(void)
{
    flexframesync_print(_fs);
}

void FlexFrame::Demodulator::reset(Clock::time_point timestamp, size_t off)
{
    flexframesync_reset(_fs);

    _demod_start = timestamp;
    _demod_off = off;
}

void FlexFrame::Demodulator::demodulate(std::complex<float>* data,
                                        size_t count,
                                        std::function<void(std::unique_ptr<RadioPacket>)> callback)
{
    _callback = callback;

    flexframesync_execute(_fs, reinterpret_cast<liquid_float_complex*>(data), count);
}

int FlexFrame::Demodulator::liquid_callback(unsigned char *  _header,
                                            int              _header_valid,
                                            unsigned char *  _payload,
                                            unsigned int     _payload_len,
                                            int              _payload_valid,
                                            framesyncstats_s _stats,
                                            void *           _userdata)
{
    reinterpret_cast<FlexFrame::Demodulator*>(_userdata)->
        callback(_header,
                 _header_valid,
                 _payload,
                 _payload_len,
                 _payload_valid,
                 _stats);
    // The flexframsync code doesn't actually use the callback's return value
    // for anything!
    return 0;
}

void FlexFrame::Demodulator::callback(unsigned char *  _header,
                                      int              _header_valid,
                                      unsigned char *  _payload,
                                      unsigned int     _payload_len,
                                      int              _payload_valid,
                                      framesyncstats_s _stats)
{
    Header* h = reinterpret_cast<Header*>(_header);

    if (_phy._logger) {
        auto buf = std::make_shared<buffer<std::complex<float>>>(_stats.num_framesyms);
        memcpy(buf->data(), _stats.framesyms, _stats.num_framesyms*sizeof(std::complex<float>));
        _phy._logger->logRecv(_demod_start,
                              _header_valid,
                              _payload_valid,
                              *h,
                              _demod_off + _stats.start_counter,
                              _demod_off + _stats.end_counter,
                              std::move(buf));
    }

    // Update demodulation offset. The framesync object is reset after the
    // callback is called, which sets its internal counters to 0.
    _demod_off += _stats.end_counter;

    if (!_header_valid) {
        printf("HEADER INVALID\n");
        _callback(nullptr);
        return;
    }

    if (!_payload_valid) {
        printf("PAYLOAD INVALID\n");
        _callback(nullptr);
        return;
    }

    if (!_phy._net->wantPacket(h->dest)) {
        _callback(nullptr);
        return;
    }

    if (h->pkt_len == 0) {
        _callback(nullptr);
        return;
    }

    auto pkt = std::make_unique<RadioPacket>(_payload, h->pkt_len);

    pkt->src = h->src;
    pkt->dest = h->dest;
    pkt->pkt_id = h->pkt_id;

    _callback(std::move(pkt));
}

/** CRC */
const crc_scheme CHECK = LIQUID_CRC_32;

/** Inner FEC */
const fec_scheme FEC_INNER = LIQUID_FEC_CONV_V29;

/** Outer FEC */
const fec_scheme FEC_OUTER = LIQUID_FEC_RS_M8;

/** Modulation */
const modulation_scheme MODSCHEME = LIQUID_MODEM_QPSK;

std::unique_ptr<PHY::Demodulator> FlexFrame::make_demodulator(void)
{
    return std::unique_ptr<PHY::Demodulator>(static_cast<PHY::Demodulator*>(new Demodulator(*this)));
}

std::unique_ptr<PHY::Modulator> FlexFrame::make_modulator(void)
{
    auto modulator = std::unique_ptr<PHY::Modulator>(static_cast<PHY::Modulator*>(
      new Modulator(*this, CHECK, FEC_INNER, FEC_OUTER, MODSCHEME)));

    modulator->setSoftTXGain(-10.0f);

    return modulator;
}
