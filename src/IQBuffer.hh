#ifndef IQBUFFER_H_
#define IQBUFFER_H_

#include <atomic>
#include <complex>
#include <deque>
#include <memory>
#include <vector>

#if !defined(NOUHD)
#include <uhd/types/time_spec.hpp>
#endif /* !defined(NOUHD) */

#include "buffer.hh"

#if !defined(NOUHD)
#include "Clock.hh"
#endif /* !defined(NOUHD) */

/** @brief A buffer of IQ samples */
struct IQBuf : buffer<std::complex<float>> {
public:
    IQBuf(size_t sz)
      : buffer(sz)
      , delay(0)
      , complete(false)
      , in_snapshot(false)
      , snapshot_off(0)
      , undersample(0)
      , oversample(0)
    {
        nsamples.store(0, std::memory_order_release);
    }

    IQBuf(const IQBuf &other)
      : buffer(other)
      , delay(other.delay)
      , complete(other.complete)
      , in_snapshot(other.in_snapshot)
      , snapshot_off(other.snapshot_off)
      , undersample(other.undersample)
      , oversample(other.oversample)
    {
        nsamples.store(other.nsamples.load());
    }

    IQBuf(IQBuf &&other)
      : buffer(std::move(other))
      , delay(other.delay)
      , complete(other.complete)
      , in_snapshot(other.in_snapshot)
      , snapshot_off(other.snapshot_off)
      , undersample(other.undersample)
      , oversample(other.oversample)
    {
        nsamples.store(other.nsamples.load());
    }

    IQBuf(const buffer<std::complex<float>> &other)
      : buffer(other)
      , delay(0)
      , complete(false)
      , in_snapshot(false)
      , snapshot_off(0)
      , undersample(0)
      , oversample(0)
    {
        nsamples.store(0, std::memory_order_release);
    }

    IQBuf(buffer<std::complex<float>> &&other)
      : buffer(std::move(other))
      , delay(0)
      , complete(true)
      , in_snapshot(false)
      , snapshot_off(0)
      , undersample(0)
      , oversample(0)
    {
        nsamples.store(0, std::memory_order_release);
    }

    IQBuf(const std::complex<float> *data, size_t n)
      : buffer(data, n)
      , delay(0)
      , complete(true)
      , in_snapshot(false)
      , snapshot_off(0)
      , undersample(0)
      , oversample(0)
    {
        nsamples.store(0, std::memory_order_release);
    }

    ~IQBuf() noexcept {}

    IQBuf& operator=(const IQBuf&) = delete;
    IQBuf& operator=(IQBuf&&) = delete;

#if !defined(NOUHD)
    /** @brief Timestamp of the first sample */
    Clock::time_point timestamp;
#endif /* !defined(NOUHD) */

    /** @brief Sample center frequency */
    float fc;

    /** @brief Sample rate */
    float fs;

    /** @brief Signal delay */
    size_t delay;

    /** @brief Number of samples received so far. */
    /** This value is valid untile the buffer is marked complete. */
    std::atomic<size_t> nsamples;

    /** @brief Flag that is true when receive is completed. */
    bool complete;

    /** @brief Is this buffer part of a snapshot?. */
    bool in_snapshot;

    /** @brief Offset from beginning of the current snapshot. */
    size_t snapshot_off;

    /** @brief Number of undersamples at the beginning of the buffer. That is,
     * this is how many samples we missed at the beginning of the receive.
     */
    size_t undersample;

    /** @brief Number oversamples at the end of buffer. */
    size_t oversample;
};

#endif /* IQBUFFER_H_ */
