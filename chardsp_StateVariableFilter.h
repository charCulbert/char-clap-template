#pragma once

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace chardsp
{
enum class FilterMode
{
    lowpass,
    bandpass,
    highpass
};

template <typename SampleType>
struct StateVariableFilterOutputs
{
    SampleType lowpass = SampleType {};
    SampleType bandpass = SampleType {};
    SampleType highpass = SampleType {};
    SampleType notch = SampleType {};
};

// Two-integrator-loop topology-preserving-transform state-variable filter.
template <typename SampleType>
struct StateVariableFilter
{
    static_assert (std::is_same_v<SampleType, float> || std::is_same_v<SampleType, double>,
                   "StateVariableFilter supports float or double");

    void prepare (SampleType newSampleRate) noexcept
    {
        sampleRate = std::isfinite (newSampleRate) && newSampleRate > 0.0f
                         ? newSampleRate
                         : static_cast<SampleType> (48000.0);
        mode = FilterMode::lowpass;
        setCutoffAndQ (static_cast<SampleType> (20000.0),
                       static_cast<SampleType> (0.5));
        reset();
    }

    void reset() noexcept
    {
        z0 = SampleType {};
        z1 = SampleType {};
    }

    void setMode (FilterMode nextMode) noexcept { mode = nextMode; }

    void setCutoffAndQ (SampleType cutoffHz, SampleType q) noexcept
    {
        const auto processorFrequency = static_cast<double> (sampleRate);
        const auto requestedCutoff =
            std::isnan (cutoffHz) ? 0.0 : static_cast<double> (cutoffHz);
        const auto filterFrequency =
            std::clamp (requestedCutoff, 0.0, processorFrequency * 0.48);
        const auto f =
            std::tan (3.14159265358979323846 * filterFrequency / processorFrequency);
        const auto requestedQ = std::isnan (q) ? 0.0 : static_cast<double> (q);
        const auto inverseQ = 1.0 / std::max (0.000001, requestedQ);
        d = static_cast<SampleType> (1.0 / ((1.0 + inverseQ * f) + f * f));
        a = static_cast<SampleType> (f);
        g1 = static_cast<SampleType> (f + inverseQ);
    }

    SampleType process (SampleType input) noexcept
    {
        const auto outputs = processAll (input);

        if (mode == FilterMode::highpass)
            return outputs.highpass;

        if (mode == FilterMode::bandpass)
            return outputs.bandpass;

        return outputs.lowpass;
    }

    StateVariableFilterOutputs<SampleType> processAll (SampleType input) noexcept
    {
        input = std::isfinite (input) ? input : SampleType {};

        if (input == SampleType {})
        {
            snapTinyStateToZero();

            if (z0 == SampleType {} && z1 == SampleType {})
                return {};
        }

        const auto high = ((input - g1 * z0) - z1) * d;
        const auto band = a * high + z0;
        const auto low = a * band + z1;
        z0 = a * high + band;
        z1 = a * band + low;
        return { low, band, high, low + high };
    }

private:
    void snapTinyStateToZero() noexcept
    {
        constexpr auto threshold = static_cast<SampleType> (1.0e-15);

        if (std::abs (z0) < threshold)
            z0 = SampleType {};

        if (std::abs (z1) < threshold)
            z1 = SampleType {};
    }

    SampleType sampleRate = static_cast<SampleType> (48000.0);
    SampleType a = SampleType {};
    SampleType d = static_cast<SampleType> (1.0);
    SampleType g1 = SampleType {};
    SampleType z0 = SampleType {};
    SampleType z1 = SampleType {};
    FilterMode mode = FilterMode::lowpass;
};

} // namespace chardsp
