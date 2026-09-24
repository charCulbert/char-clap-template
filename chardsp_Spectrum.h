#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace chardsp
{
// A spectrum for display, after the method of Signalsmith Audio's analyser
// (github.com/Signalsmith-Audio/basics, analyser.h, MIT):
//
// - A short windowed FFT (windowSize samples, zero-padded to fftSize) every
//   `hop` samples.
// - Analysis bands 8 Hz apart at low frequencies, 1/24 octave in the middle,
//   and one per window bin (23 Hz) above, so no frequency falls between bands.
// - For each band, the FFT's complex value at the band's centre goes through
//   a two-pole complex resonator tuned to that frequency, twice as wide as the
//   band spacing. That smooths each band over time and makes it a smooth,
//   overlapping band-pass: narrow bands respond slower, wide ones faster.
// - Levels are in dB relative to a full-scale sine, which reads 0 dB.
//
// The audio thread only copies samples into a ring buffer. analyze() runs on
// another thread and catches up on every hop since its last call.
struct Spectrum
{
    static constexpr int windowSize = 2048; // 43 ms at 48 kHz
    static constexpr int fftSize = 4096;    // zero-padded: finer bins to interpolate between
    static constexpr int hop = 240;         // 5 ms at 48 kHz

    // Audio thread: never blocks or allocates.
    void push (const float* left, const float* right, uint32_t frames) noexcept
    {
        uint32_t index = writeIndex.load (std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames; ++i, ++index)
            ring[index % ringSize].store (0.5f * (left[i] + right[i]), std::memory_order_relaxed);
        writeIndex.store (index, std::memory_order_release);
    }

    // Other thread: `bandCount` display bands in dB from minHz to maxHz, evenly
    // spaced on Signalsmith's display scale hz / (1500 + hz) (see scale()),
    // each the loudest analysis band within it. Returns false until the first
    // FFT's worth of samples has arrived.
    bool analyze (float sampleRate, float minHz, float maxHz, int bandCount, std::vector<double>& display)
    {
        const uint32_t end = writeIndex.load (std::memory_order_acquire);
        if (end < windowSize || sampleRate <= 0)
            return false;
        if (sampleRate != preparedRate)
            prepare (sampleRate);

        // Every hop boundary since the last call. After a long gap (the page
        // was closed), only the last 100 ms: resonators forget older input.
        // (Sample positions are unsigned and may wrap, so compare distances.)
        uint32_t next = analysed + hop;
        if (analysed == 0 || end - analysed > uint32_t (20 * hop))
        {
            const uint32_t from = end > uint32_t (windowSize + 20 * hop) ? end - 20 * hop : uint32_t (windowSize);
            next = (from + hop - 1) / hop * hop; // on the hop grid, so each band's phase steps evenly
        }
        for (; end - next < 0x80000000u; next += hop) // while next <= end
            step (next);
        analysed = next - hop;

        // Reduce to the display's bands.
        display.assign (bandCount, -140.0);
        for (size_t b = 0; b < hz.size(); ++b)
        {
            const int d = int (std::floor ((scale (hz[b]) - scale (minHz)) / (scale (maxHz) - scale (minHz)) * bandCount));
            if (d < 0 || d >= bandCount)
                continue;
            const double amplitude = std::abs (state2[b]) * amplitudeScale;
            display[d] = std::max (display[d], 20 * std::log10 (amplitude + 1e-7));
        }
        // Display bands narrower than the analysis spacing hold no band centre;
        // a smooth curve (Catmull-Rom) through the ones that do fills them in.
        known.clear();
        for (int d = 0; d < bandCount; ++d)
            if (display[d] > -140.0)
                known.push_back (d);
        for (size_t i = 0; i + 1 < known.size(); ++i)
        {
            const int d0 = known[i], d1 = known[i + 1];
            const int dPrev = i > 0 ? known[i - 1] : d0, dNext = i + 2 < known.size() ? known[i + 2] : d1;
            const double y0 = display[d0], y1 = display[d1];
            // Tangents from the neighbouring points, per display band.
            const double t0 = (y1 - display[dPrev]) / std::max (1, d1 - dPrev) * (d1 - d0);
            const double t1 = (display[dNext] - y0) / std::max (1, dNext - d0) * (d1 - d0);
            for (int g = d0 + 1; g < d1; ++g)
            {
                const double t = double (g - d0) / (d1 - d0), t2 = t * t, t3 = t2 * t;
                display[g] = (2 * t3 - 3 * t2 + 1) * y0 + (t3 - 2 * t2 + t) * t0 + (-2 * t3 + 3 * t2) * y1 + (t3 - t2) * t1;
            }
        }
        return true;
    }

    // The display's frequency scale: close to linear in the lows and to
    // logarithmic above, roughly like hearing.
    static double scale (double hz) { return hz / (1500 + hz); }

private:
    static constexpr int ringSize = 16384;
    static constexpr double pi = 3.14159265358979323846;

    void prepare (float sampleRate)
    {
        preparedRate = sampleRate;
        window.resize (windowSize);
        double windowSum = 0;
        for (int i = 0; i < windowSize; ++i)
            windowSum += window[i] = float (0.5 - 0.5 * std::cos (2 * pi * i / windowSize));
        amplitudeScale = 2 / windowSum; // a sine of amplitude A peaks at A·Σw/2 in the FFT
        buffer.resize (fftSize);

        // Bands: 1/24 octave, but at least 8 Hz apart (like Bark bands, so the
        // lows stay quick) and at most one window bin apart (so none is missed).
        const double windowBinHz = double (sampleRate) / windowSize, step = std::pow (2.0, 1.0 / 24);
        hz.clear();
        widthHz.clear();
        for (double f = 20; f < sampleRate / 2;)
        {
            const double width = std::clamp (f * (step - 1), 8.0, windowBinHz);
            hz.push_back (f);
            widthHz.push_back (width);
            f += width;
        }
        // Resonators run once per hop: a pole at each band's frequency (the
        // FFT phase advances by that much per hop), damped by twice the band's
        // width, so neighbouring bands overlap and a sine between two centres
        // still reads nearly its full level.
        const double hopRate = double (sampleRate) / hop;
        twist.resize (hz.size());
        gain.resize (hz.size());
        for (size_t b = 0; b < hz.size(); ++b)
        {
            twist[b] = std::exp (std::complex<double> (-2 * pi * 2 * widthHz[b] / hopRate, 2 * pi * hz[b] / hopRate));
            gain[b] = 1 - std::abs (twist[b]);
        }
        state1.assign (hz.size(), 0.0);
        state2.assign (hz.size(), 0.0);
        analysed = 0;
    }

    // One analysis frame ending at sample `end`.
    void step (uint32_t end)
    {
        // Rotated so the window's centre is sample 0 ("zero phase"): then a
        // sine's neighbouring bins agree in phase and interpolate cleanly.
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        for (int i = 0; i < windowSize; ++i)
            buffer[(i - windowSize / 2 + fftSize) % fftSize] = ring[(end - windowSize + i) % ringSize].load (std::memory_order_relaxed) * window[i];
        fft (buffer);
        const double binHz = preparedRate / fftSize;
        for (size_t b = 0; b < hz.size(); ++b)
        {
            // The FFT's value at the band's centre, interpolated between bins.
            const double bin = std::min (hz[b] / binHz, fftSize / 2 - 1.001);
            const int k = int (bin);
            const std::complex<double> input = std::complex<double> (buffer[k]) * (k + 1 - bin)
                                             + std::complex<double> (buffer[k + 1]) * (bin - k);
            state1[b] = state1[b] * twist[b] + input * gain[b];
            state2[b] = state2[b] * twist[b] + state1[b] * gain[b];
        }
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
    float preparedRate = 0;
    uint32_t analysed = 0; // the last hop boundary analysed
    std::vector<float> window;
    double amplitudeScale = 1;
    std::vector<std::complex<float>> buffer;
    std::vector<double> hz, widthHz, gain;
    std::vector<std::complex<double>> twist, state1, state2;
    std::vector<int> known;
};
} // namespace chardsp
