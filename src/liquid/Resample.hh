#ifndef LIQUID_RESAMPLE_HH_
#define LIQUID_RESAMPLE_HH_

#include <complex>
#include <functional>
#include <memory>

#include <liquid/liquid.h>

#include "IQBuffer.hh"
#include "dsp/Resample.hh"

namespace Liquid {

using C = std::complex<float>;
using F = float;

struct ResamplerParams {
    using update_t = std::function<void(void)>;

    ResamplerParams(update_t update)
      : m(7)
      , fc(0.4f)
      , As(60.0f)
      , npfb(64)
      , update_(update)
    {
    }

    ~ResamplerParams() = default;

    ResamplerParams() = delete;

    unsigned get_m(void)
    {
        return m;
    }

    void set_m(unsigned m_new)
    {
        m = m_new;
        update_();
    }

    float get_fc(void)
    {
        return fc;
    }

    void set_fc(float fc_new)
    {
        fc = fc_new;
        update_();
    }

    float get_As(void)
    {
        return fc;
    }

    void set_As(float As_new)
    {
        As = As_new;
        update_();
    }

    unsigned get_npfb(void)
    {
        return npfb;
    }

    void set_npfb(unsigned npfb_new)
    {
        npfb = npfb_new;
        update_();
    }

    /** @brief Prototype filter semi-length */
    unsigned int m;

    /** @brief Prototype filter cutoff frequency */
    float fc;

    /** @brief Stop-band attenuation for resamplers */
    float As;

    /** @brief Number of filters in polyphase filterbank */
    unsigned npfb;

protected:
    /** @brief Callback called when variables are modified via set* */
    update_t update_;
};

template<class I, class O, class C>
class MultiStageResampler : public Resampler<I,O> {
public:
   /** @brief Create a liquid multi-stage resampler
    * @param rate Resampling rate
    * @param m Prototype filter semi-length
    * @param fc Prototype filter cutoff frequency, in range (0, 0.5)
    * @param As Stop-band attenuation
    * @param npfb Number of filters in polyphase filterbank
    */
    MultiStageResampler(float rate,
                        unsigned m,
                        float fc,
                        float As,
                        unsigned npfb)
    {
        static_assert(sizeof(I) == 0, "Only specializations of MultiStageResampler can be used");
    }

    MultiStageResampler(MultiStageResampler &&resamp)
    {
        static_assert(sizeof(I) == 0, "Only specializations of MultiStageResampler can be used");
    }

    virtual ~MultiStageResampler()
    {
        static_assert(sizeof(I) == 0, "Only specializations of MultiStageResampler can be used");
    }

    double getRate(void) const override final
    {
        static_assert(sizeof(I) == 0, "Only specializations of MultiStageResampler can be used");
        return 0;
    }

    double getDelay(void) const override final
    {
        static_assert(sizeof(I) == 0, "Only specializations of MultiStageResampler can be used");
        return 0;
    }

    size_t neededOut(size_t count) const override final
    {
        static_assert(sizeof(I) == 0, "Only specializations of MultiStageResampler can be used");
        return 0;
    }

    void reset(void) override final
    {
        static_assert(sizeof(I) == 0, "Only specializations of MultiStageResampler can be used");
    }

    size_t resample(const std::complex<float> *in, size_t count, std::complex<float> *out) override final
    {
        static_assert(sizeof(I) == 0, "Only specializations of MultiStageResampler can be used");
        return 0;
    }

    void print(void)
    {
        static_assert(sizeof(I) == 0, "Only specializations of MultiStageResampler can be used");
    }
};

template<>
class MultiStageResampler<C,C,F> : public Resampler<C,C> {
public:
   /** @brief Create a liquid multi-stage resampler
    * @param rate Resampling rate
    * @param m Prototype filter semi-length
    * @param fc Prototype filter cutoff frequency, in range (0, 0.5)
    * @param As Stop-band attenuation
    * @param npfb Number of filters in polyphase filterbank
    */
    MultiStageResampler(float rate,
                        unsigned m,
                        float fc,
                        float As,
                        unsigned npfb)
    {
        resamp_ = msresamp_crcf_create(rate, m, fc, As, npfb);
        rate_ = msresamp_crcf_get_rate(resamp_);
        delay_ = msresamp_crcf_get_delay(resamp_);
    }

    MultiStageResampler(const MultiStageResampler &) = delete;

    MultiStageResampler(MultiStageResampler &&resamp)
    {
        msresamp_crcf_destroy(resamp_);
        resamp_ = resamp.resamp_;
        resamp.resamp_ = nullptr;

        rate_ = resamp.rate_;
        delay_ = resamp.delay_;
    }

    virtual ~MultiStageResampler()
    {
        if (resamp_)
            msresamp_crcf_destroy(resamp_);
    }

    MultiStageResampler& operator =(const MultiStageResampler&) = delete;

    MultiStageResampler &operator =(MultiStageResampler &&resamp)
    {
        msresamp_crcf_destroy(resamp_);
        resamp_ = resamp.resamp_;
        resamp.resamp_ = nullptr;

        rate_ = resamp.rate_;
        delay_ = resamp.delay_;

        return *this;
    }

    double getRate(void) const override final
    {
        return rate_;
    }

    double getDelay(void) const override final
    {
        return delay_;
    }

    size_t neededOut(size_t count) const override final
    {
        return 1 + 2*rate_*count;
    }

    virtual void reset(void) override final
    {
        return msresamp_crcf_reset(resamp_);
    }

    virtual size_t resample(const std::complex<float> *in, size_t count, std::complex<float> *out) override final;

    using Resampler::resample;

    void print(void)
    {
        return msresamp_crcf_print(resamp_);
    }

protected:
    msresamp_crcf resamp_;
    double rate_;
    double delay_;
};

}

#endif /* LIQUID_RESAMPLE_HH_ */
