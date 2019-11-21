#include <xsimd/xsimd.hpp>
#include <xsimd/stl/algorithms.hpp>

#include "Logger.hh"
#include "RadioConfig.hh"
#include "WorkQueue.hh"
#include "dsp/NCO.hh"
#include "liquid/PHY.hh"

// Initial modulation buffer size
const size_t kInitialModbufSize = 16384;

Liquid::PHY::PHY(std::shared_ptr<SnapshotCollector> collector,
                 NodeId node_id,
                 const MCS &header_mcs,
                 const std::vector<std::pair<MCS, AutoGain>> &mcstab,
                 bool soft_header,
                 bool soft_payload)
  : ::PHY(collector, node_id)
  , header_mcs_(header_mcs)
  , soft_header_(soft_header)
  , soft_payload_(soft_payload)
{
    mcs_table.resize(mcstab.size());
    mcs_table_.resize(mcstab.size());

    for (unsigned i = 0; i < mcs_table_.size(); ++i) {
        mcs_table_[i] = mcstab[i].first;
        mcs_table[i] = { &mcs_table_[i], mcstab[i].second, true };
    }
}

void Liquid::PHY::PacketModulator::modulate(std::shared_ptr<NetPacket> pkt,
                                            const float g,
                                            ModPacket &mpkt)
{
    MonoClock::time_point now = MonoClock::now();

    assert(pkt->mcsidx < phy_.mcs_table.size());
    setPayloadMCS(*reinterpret_cast<const MCS*>(phy_.mcs_table[pkt->mcsidx].mcs));
    assemble(&pkt->hdr, pkt->data(), pkt->size());

    // Buffer holding generated IQ samples
    auto iqbuf = std::make_shared<IQBuf>(kInitialModbufSize);
    // Number of generated samples in the buffer
    size_t nsamples = 0;
    // Max number of samples generated by modulateSamples
    const size_t kMaxModSamples = maxModulatedSamples();
    // Number of samples written
    size_t nw;
    // Flag indicating when we've reached the last symbol
    bool last_symbol;

    do {
        last_symbol = modulateSamples(&(*iqbuf)[nsamples], nw);

        // Apply soft gain.
        if (g != 1.0)
            xsimd::transform(iqbuf->data() + nsamples,
                             iqbuf->data() + nsamples + nw,
                             iqbuf->data() + nsamples,
                [&](const auto& x) { return x*g; });

        // We have nw additional samples
        nsamples += nw;

        // If we can't add another nw samples to the current IQ buffer, resize
        // it.
        if (nsamples + kMaxModSamples > iqbuf->size())
            iqbuf->resize(2*iqbuf->size());
    } while (!last_symbol);

    // Resize the final buffer to the number of samples generated.
    iqbuf->resize(nsamples);

    // Pass the modulated packet to the 0dBFS estimator if requested
    AutoGain &autogain = phy_.mcs_table[pkt->mcsidx].autogain;

    if (autogain.needCalcAutoSoftGain0dBFS())
        work_queue.submit(&AutoGain::autoSoftGain0dBFS, &autogain, g, iqbuf);

    // Fill in the ModPacket
    mpkt.offset = iqbuf->delay;
    mpkt.nsamples = iqbuf->size() - iqbuf->delay;
    mpkt.mod_latency = (MonoClock::now() - now).get_real_secs();
    mpkt.samples = std::move(iqbuf);
    mpkt.pkt = std::move(pkt);
}

Liquid::PHY::PacketDemodulator::PacketDemodulator(PHY &phy,
                                                  const MCS &header_mcs,
                                                  bool soft_header,
                                                  bool soft_payload)
  : Liquid::Demodulator(header_mcs, soft_header, soft_payload)
  , ::PHY::PacketDemodulator(phy)
  , channel_()
  , resamp_rate_(1.0)
  , internal_oversample_fact_(1)
  , timestamp_(0.0)
  , offset_(0)
  , sample_start_(0)
  , sample_end_(0)
  , sample_(0)
  , logger_(logger)
{
}

int Liquid::PHY::PacketDemodulator::callback(unsigned char *  header_,
                                             int              header_valid_,
                                             int              header_test_,
                                             unsigned char *  payload_,
                                             unsigned int     payload_len_,
                                             int              payload_valid_,
                                             framesyncstats_s stats_)
{
    Header* h = reinterpret_cast<Header*>(header_);

    // Save samples of frame start and end
    unsigned frame_start = sample_ + stats_.start_counter;
    unsigned frame_end = sample_ + stats_.end_counter;

    // Perform test to see if we want to continue demodulating this packet.
    if (header_test_) {
        if (phy_.wantPacket(header_valid_, h))
            return 1;
        else {
            // Update sample count. The framesync object is reset if we decline
            // to demodulate the frame, which sets its internal counters to 0.
            sample_ = frame_end;

            return 0;
        }
    }

    // Update sample count. The framesync object is reset after the callback is
    // called, which sets its internal counters to 0.
    sample_ = frame_end;

    // Create the packet and fill it out
    std::unique_ptr<RadioPacket> pkt = phy_.mkRadioPacket(header_valid_,
                                                          payload_valid_,
                                                          h,
                                                          payload_len_,
                                                          payload_);

    if (!pkt)
        return 0;

    pkt->evm = stats_.evm;
    pkt->rssi = stats_.rssi;
    pkt->cfo = stats_.cfo;
    pkt->channel = channel_;

    // The start and end variables contain full-rate sample offsets of the frame
    // start and end relative to the beginning of the slot.
    ssize_t               start = offset_ + resamp_rate_*static_cast<signed>(frame_start - sample_start_);
    ssize_t               end = offset_ + resamp_rate_*static_cast<signed>(frame_end - sample_start_);
    MonoClock::time_point timestamp = timestamp_ + start / rx_rate_;

    pkt->timestamp = timestamp;

    callback_(std::move(pkt));

    if (snapshot_off_)
        phy_.getSnapshotCollector()->selfTX(*snapshot_off_ + start,
                                            *snapshot_off_ + end,
                                            channel_.fc,
                                            channel_.bw);

    if (logger_ &&
        logger_->getCollectSource(Logger::kRecvPackets) &&
        (header_valid_ || rc.log_invalid_headers)) {
        std::shared_ptr<buffer<std::complex<float>>> buf = nullptr;

        if (logger_->getCollectSource(Logger::kRecvSymbols)) {
            buf = std::make_shared<buffer<std::complex<float>>>(stats_.num_framesyms);
            memcpy(buf->data(), stats_.framesyms, stats_.num_framesyms*sizeof(std::complex<float>));
        }

        // Find MCS index
        Liquid::MCS             mcs(static_cast<crc_scheme>(stats_.check),
                                    static_cast<fec_scheme>(stats_.fec0),
                                    static_cast<fec_scheme>(stats_.fec1),
                                    static_cast<modulation_scheme>(stats_.mod_scheme));
        std::optional<mcsidx_t> mcsidx = 0;

        for (mcsidx_t i = 0; i < phy_.mcs_table.size(); ++i) {
            if (mcs == *reinterpret_cast<const MCS*>(phy_.mcs_table[i].mcs)) {
                mcsidx = i;
                break;
            }
        }

        logger_->logRecv(Clock::to_wall_time(timestamp_),
                         start,
                         end,
                         header_valid_,
                         payload_valid_,
                         *h,
                         h->curhop,
                         h->nexthop,
                         mcsidx ? *mcsidx : 0,
                         stats_.evm,
                         stats_.rssi,
                         stats_.cfo,
                         channel_.fc,
                         rx_rate_,
                         (MonoClock::now() - timestamp).get_real_secs(),
                         payload_len_,
                         std::move(buf));
    }

    return 0;
}

void Liquid::PHY::PacketDemodulator::reset(const Channel &channel)
{
    reset();

    channel_ = channel;
    resamp_rate_ = 1.0;
    timestamp_ = MonoClock::time_point { 0.0 };
    snapshot_off_ = std::nullopt;
    offset_ = 0;
    sample_start_ = 0;
    sample_end_ = 0;
    sample_ = 0;
}

void Liquid::PHY::PacketDemodulator::timestamp(const MonoClock::time_point &timestamp,
                                               std::optional<ssize_t> snapshot_off,
                                               ssize_t offset,
                                               float rate,
                                               float rx_rate)
{
    resamp_rate_ = internal_oversample_fact_/rate;
    rx_rate_ = rx_rate;
    timestamp_ = timestamp;
    snapshot_off_ = snapshot_off;
    offset_ = offset;
    sample_start_ = sample_end_;
}

void Liquid::PHY::PacketDemodulator::demodulate(const std::complex<float>* data,
                                                size_t count,
                                                std::function<void(std::unique_ptr<RadioPacket>)> callback)
{
    callback_ = callback;
    demodulateSamples(data, count);
    sample_end_ += count;
}

size_t Liquid::PHY::getModulatedSize(mcsidx_t mcsidx, size_t n)
{
    std::unique_ptr<Liquid::Modulator> mod = mkLiquidModulator();

    assert(mcsidx < mcs_table.size());
    mod->setPayloadMCS(mcs_table_[mcsidx]);

    Header                     hdr = {0};
    std::vector<unsigned char> body(sizeof(ExtendedHeader) + n);

    mod->assemble(&hdr, body.data(), body.size());

    return mod->assembledSize();
}
