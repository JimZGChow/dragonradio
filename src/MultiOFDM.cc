#include "Liquid.hh"
#include "MultiOFDM.hh"

/** Number of channels */
const unsigned int NUM_CHANNELS = 1;

/** Number of OFDM subcarriers */
const unsigned int M = 480;

/** OFDM cyclic prefix length */
const unsigned int CP_LEN = 6;

/** OFDM taper prefix length */
const unsigned int TP_LEN = 4;

/** OFDM subcarrier allocation */
unsigned char *SUBCAR = nullptr;

/** Inner FEC */
const int FEC_INNER = LIQUID_FEC_CONV_V29;

/** Outer FEC */
const int FEC_OUTER = LIQUID_FEC_RS_M8;

/** Modulation */
const int MOD = LIQUID_MODEM_QPSK;

// liquid fixes the header size at 8 bytes
static_assert(sizeof(Header) <= 8, "sizeof(Header) must be no more than 8 bytes");

union PHYHeader {
    Header        h;
    // OFDMFLEXFRAME_H_USER in liquid.internal.h
    unsigned char bytes[8];
};

MultiOFDM::Modulator::Modulator(MultiOFDM& phy) :
    _phy(phy),
    // Corresponds to ~-14dB
    _g(0.2)
{
    std::lock_guard<std::mutex> lck(liquid_mutex);

    // modem setup (list is for parallel demodulation)
    _mctx = std::make_unique<multichanneltx>(NUM_CHANNELS, M, CP_LEN, TP_LEN, SUBCAR);
}

MultiOFDM::Modulator::~Modulator()
{
}

void MultiOFDM::Modulator::setSoftTXGain(float dB)
{
    _g = powf(10.0f, dB/20.0f);
}

// Number of samples generated by a call to GenerateSamples.
const size_t NGEN = 2;

// Initial sample buffer size
const size_t MODBUF_SIZE = 16384;

std::unique_ptr<ModPacket> MultiOFDM::Modulator::modulate(std::unique_ptr<NetPacket> pkt)
{
    PHYHeader header;

    memset(&header, 0, sizeof(header));

    header.h.src = pkt->src;
    header.h.dest = pkt->dest;
    header.h.pkt_id = pkt->pkt_id;
    header.h.pkt_len = pkt->payload.size();

    pkt->payload.resize(std::max((size_t) pkt->payload.size(), _phy._minPacketSize));

    _mctx->UpdateData(0, header.bytes, &(pkt->payload)[0], pkt->payload.size(), MOD, FEC_INNER, FEC_OUTER);

    // Buffer holding generated IQ samples
    auto iqbuf = std::make_unique<IQBuf>(MODBUF_SIZE);
    // Number of generated samples in the buffer
    size_t nsamples = 0;
    // Local copy of gain
    const float g = _g;

    while (!_mctx->IsChannelReadyForData(0)) {
        _mctx->GenerateSamples(&(*iqbuf)[nsamples]);

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

MultiOFDM::Demodulator::Demodulator(MultiOFDM& phy) :
    _phy(phy)
{
    std::lock_guard<std::mutex> lck(liquid_mutex);

    // modem setup (list is for parallel demodulation)
    framesync_callback callback[1] = { &Demodulator::liquidRxCallback };
    void               *userdata[1] = { this };

    mcrx = std::make_unique<multichannelrx>(NUM_CHANNELS, M, CP_LEN, TP_LEN, SUBCAR, userdata, callback);
}

MultiOFDM::Demodulator::~Demodulator()
{
}

void MultiOFDM::Demodulator::demodulate(std::unique_ptr<IQQueue> buf)
{
    mcrx->Reset();

    for (auto it = buf->begin(); it != buf->end(); ++it)
        mcrx->Execute(&(*it)[0], it->size());
}

int MultiOFDM::Demodulator::liquidRxCallback(unsigned char *  _header,
                                             int              _header_valid,
                                             unsigned char *  _payload,
                                             unsigned int     _payload_len,
                                             int              _payload_valid,
                                             framesyncstats_s _stats,
                                             void *           _userdata,
                                             liquid_float_complex* G,
                                             liquid_float_complex* G_hat,
                                             unsigned int M)
{
    return reinterpret_cast<Demodulator*>(_userdata)->rxCallback(_header,
                                                                 _header_valid,
                                                                 _payload,
                                                                 _payload_len,
                                                                 _payload_valid,
                                                                 _stats,
                                                                 G,
                                                                 G_hat,
                                                                 M);
}

int MultiOFDM::Demodulator::rxCallback(unsigned char *  _header,
                                       int              _header_valid,
                                       unsigned char *  _payload,
                                       unsigned int     _payload_len,
                                       int              _payload_valid,
                                       framesyncstats_s _stats,
                                       liquid_float_complex* G,
                                       liquid_float_complex* G_hat,
                                       unsigned int M)
{
    Header* h = reinterpret_cast<Header*>(_header);

    if (!_header_valid) {
        printf("HEADER INVALID\n");
        return 0;
    }

    if (!_payload_valid) {
        printf("PAYLOAD INVALID\n");
        return 0;
    }

    if (!_phy._net->wantPacket(h->dest))
        return 0;

    if (h->pkt_len == 0)
        return 1;

    auto pkt = std::make_unique<RadioPacket>(_payload, h->pkt_len);

    pkt->src = h->src;
    pkt->dest = h->dest;
    pkt->pkt_id = h->pkt_id;

    _phy._net->sendPacket(std::move(pkt));

    return 0;
}

std::unique_ptr<PHY::Demodulator> MultiOFDM::make_demodulator(void)
{
    return std::unique_ptr<PHY::Demodulator>(static_cast<PHY::Demodulator*>(new Demodulator(*this)));
}

std::unique_ptr<PHY::Modulator> MultiOFDM::make_modulator(void)
{
    auto modulator = std::unique_ptr<PHY::Modulator>(static_cast<PHY::Modulator*>(new Modulator(*this)));

    modulator->setSoftTXGain(-12.0f);

    return modulator;
}
