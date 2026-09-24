#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace chardsp
{
// A spectrum for display, as the power in log-spaced bands (like a
// real-time analyser): a sine reads its level in dB relative to a full-scale
// sine wherever it falls, and pink noise reads flat down to where a band is
// narrower than the FFT's 12 Hz resolution (about 450 Hz); below that, noise
// reads a little high.
//
// The audio thread only copies samples into a ring buffer. analyze() runs on
// another thread: it windows the latest fftSize samples, runs an FFT, and
// integrates the power spectrum across each band, so bands narrower than an
// FFT bin (low frequencies) and wider ones (high) are both measured, not
// sampled. Results are smoothed over time in the power domain.
struct Spectrum
{
    static constexpr int fftSize = 16384; // 2.9 Hz bins at 48 kHz
    static constexpr int bandCount = 256; // about 1/26 octave from 20 Hz to 20 kHz

    // Audio thread: never blocks or allocates.
    void push (const float* left, const float* right, uint32_t frames) noexcept
    {
        uint32_t index = writeIndex.load (std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames; ++i, ++index)
            ring[index % ringSize].store (0.5f * (left[i] + right[i]), std::memory_order_relaxed);
        writeIndex.store (index, std::memory_order_release);
    }

    // Other thread: bands in dB from minHz to maxHz, updated about updatesPerSecond
    // times a second. Returns false until the first fftSize samples have arrived.
    bool analyze (float sampleRate, float minHz, float maxHz, float updatesPerSecond, std::vector<double>& bands)
    {
        const uint32_t end = writeIndex.load (std::memory_order_acquire);
        if (end < fftSize || sampleRate <= 0)
            return false;
        prepare();
        for (int i = 0; i < fftSize; ++i)
            buffer[i] = ring[(end - fftSize + i) % ringSize].load (std::memory_order_relaxed) * window[i];
        fft (buffer);

        // Power per bin, scaled so that summing the bins of a sine of
        // amplitude A gives A²/2 (the Hann window's energy is divided out).
        for (int k = 0; k <= fftSize / 2; ++k)
            power[k] = std::norm (buffer[k]) * (k == 0 || k == fftSize / 2 ? 1.0 : 2.0) / (fftSize * windowEnergy);
        // Running sums of the power spectrum, treated as piecewise linear
        // between bin centres, so any band's power is an exact integral.
        cumulative[0] = 0;
        for (int k = 0; k < fftSize / 2; ++k)
            cumulative[k + 1] = cumulative[k] + 0.5 * (power[k] + power[k + 1]);

        const double binHz = sampleRate / fftSize;
        const double ratio = std::pow (double (maxHz) / minHz, 1.0 / bandCount);
        // One-pole smoothing: follows rises within about 20 ms, falls over about 300 ms.
        const double rise = 1 - std::exp (-1 / (0.02 * updatesPerSecond));
        const double fall = 1 - std::exp (-1 / (0.3 * updatesPerSecond));
        smoothed.resize (bandCount, 0.0);
        bands.resize (bandCount);
        for (int b = 0; b < bandCount; ++b)
        {
            // The band's total power (right for noise), or the power of the
            // strongest main lobe (4 bins) within it, whichever is more: a sine
            // spreads over 4 bins and may straddle a band edge, and this still
            // counts all of it, wherever the band is narrower or wider.
            const double lo = minHz * std::pow (ratio, b) / binHz, hi = lo * ratio;
            double p = integral (hi) - integral (lo);
            for (double c = lo; c <= std::max (hi, lo + 0.01); c += 0.5)
                p = std::max (p, integral (c + 2) - integral (c - 2));
            double& s = smoothed[b];
            s += (p > s ? rise : fall) * (p - s);
            // 0 dB is a full-scale sine (power 1/2).
            bands[b] = std::max (10 * std::log10 (s / 0.5 + 1e-15), -140.0);
        }
        return true;
    }

private:
    static constexpr int ringSize = 32768;
    static constexpr double pi = 3.14159265358979323846;

    void prepare()
    {
        if (!window.empty())
            return;
        window.resize (fftSize);
        windowEnergy = 0;
        for (int i = 0; i < fftSize; ++i)
        {
            window[i] = float (0.5 - 0.5 * std::cos (2 * pi * i / fftSize));
            windowEnergy += double (window[i]) * window[i];
        }
        buffer.resize (fftSize);
        power.resize (fftSize / 2 + 1);
        cumulative.resize (fftSize / 2 + 1);
    }

    // The integral of the power spectrum from bin 0 to a fractional bin; a
    // bin's power counts as spread over one bin's width around its centre.
    double integral (double bin) const
    {
        bin = std::clamp (bin, 0.0, double (fftSize / 2));
        const int k = std::min (int (bin), fftSize / 2 - 1);
        const double t = bin - k;
        // Within bin k..k+1 the spectrum is linear from power[k] to power[k+1].
        return cumulative[k] + t * power[k] + 0.5 * t * t * (power[k + 1] - power[k]);
    }

    // In-place radix-2 FFT.
    static void fft (std::vector<std::complex<float>>& x)
    {
        const size_t n = x.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap (x[i], x[j]);
        }
        for (size_t length = 2; length <= n; length <<= 1)
        {
            const double angle = -2 * pi / length;
            for (size_t k = 0; k < length / 2; ++k)
            {
                // Twiddles computed directly (not by repeated multiplication), so
                // rounding error does not build up across a large FFT.
                const std::complex<float> w (float (std::cos (angle * k)), float (std::sin (angle * k)));
                for (size_t i = 0; i < n; i += length)
                {
                    const auto a = x[i + k], b = x[i + k + length / 2] * w;
                    x[i + k] = a + b;
                    x[i + k + length / 2] = a - b;
                }
            }
        }
    }

    std::atomic<float> ring[ringSize] {};
    std::atomic<uint32_t> writeIndex { 0 };
    // The rest is used only by analyze().
    std::vector<float> window;
    double windowEnergy = 1;
    std::vector<std::complex<float>> buffer;
    std::vector<double> power, cumulative, smoothed;
};
} // namespace chardsp
