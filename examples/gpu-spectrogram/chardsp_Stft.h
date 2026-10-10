#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>
#include "dsp/fft.h" // Signalsmith Audio's FFT (MIT)

namespace chardsp
{
// A short-time Fourier transform for a spectrogram, as Listener's: a windowed
// FFT of `size` samples every `hop`, each one a column of levels in dB per bin
// (bin k at k · sampleRate / size Hz), relative to a full-scale sine, which
// reads 0 dB whatever the window. Its channel is the left, the right or their
// mix; time smoothing mixes each bin with its last value (in dB).
//
// The audio thread only copies both channels into a ring, which holds several
// of the largest FFT. configure() and next() run on another thread (the
// analysis), which sets the FFT up and reads the ring.
struct Stft
{
    enum class Window { rectangular, hann, hamming, blackman };
    enum class Channel { mix, left, right };
    static constexpr int minSize = 256, maxSize = 16384;
    struct Settings
    {
        int size = 2048, hop = 512; // samples: a power of two from minSize to maxSize, and 1 to size
        Window window = Window::hann;
        Channel channel = Channel::mix;
        float smoothing = 0; // 0 (off) to 0.9: how much of each bin's last value stays
        bool operator== (const Settings&) const = default;
    };

    Stft() { configure (Settings {}); }

    // Analysis thread: new settings. A new size or window makes a new FFT
    // and window; anything else changed starts afresh from the newest samples.
    void configure (Settings s)
    {
        s.size = std::clamp (s.size, minSize, maxSize);
        s.hop = std::clamp (s.hop, 1, s.size);
        s.smoothing = std::clamp (s.smoothing, 0.0f, 0.9f);
        if (s == settings && ! window.empty())
            return;
        if (s.size != settings.size || s.window != settings.window || window.empty())
        {
            const int n = s.size;
            window.resize (n);
            double sum = 0;
            for (int i = 0; i < n; ++i) // symmetric, as Listener's
            {
                const double phase = 2 * M_PI * i / (n - 1);
                switch (s.window)
                {
                    case Window::rectangular: window[i] = 1; break;
                    case Window::hann: window[i] = float (0.5 - 0.5 * std::cos (phase)); break;
                    case Window::hamming: window[i] = float (0.54 - 0.46 * std::cos (phase)); break;
                    case Window::blackman: window[i] = float (0.42 - 0.5 * std::cos (phase) + 0.08 * std::cos (2 * phase)); break;
                }
                sum += window[i];
            }
            amplitudeScale = float (2 / sum); // a sine of amplitude A peaks at A·Σw/2 in the FFT
            transform.setSize (n);
            buffer.resize (n);
            bins.resize (n / 2);
        }
        settings = s;
        smoothed.clear();
        analysed = writeIndex.load (std::memory_order_acquire);
    }

    const Settings& current() const { return settings; }

    // Audio thread: never blocks or allocates.
    void push (const float* left, const float* right, uint32_t frames) noexcept
    {
        uint32_t index = writeIndex.load (std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames; ++i, ++index)
        {
            leftRing[index % ringSize].store (left[i], std::memory_order_relaxed);
            rightRing[index % ringSize].store (right[i], std::memory_order_relaxed);
        }
        writeIndex.store (index, std::memory_order_release);
    }

    // Analysis thread: the next column into `decibels` (size / 2 values), if a
    // hop's worth of samples has arrived since the last, and in `advanced` the
    // samples since the last column. With more than `keep` columns waiting,
    // or any the ring has already overwritten, it skips to the newest few, so
    // a short hop with a large FFT can't take more time than it has.
    bool next (std::vector<float>& decibels, uint32_t& advanced, uint32_t keep = 8)
    {
        const int size = settings.size;
        const uint32_t hop = uint32_t (settings.hop);
        const uint32_t end = writeIndex.load (std::memory_order_acquire);
        if (end < uint32_t (size))
            return false;
        uint32_t before = analysed;
        if (end - analysed > uint32_t (ringSize - size)) // fell behind the ring: start again from the newest
            analysed = end - hop, before = analysed - hop;
        const uint32_t waiting = (end - analysed) / hop;
        if (waiting == 0)
            return false;
        if (waiting > keep)
            analysed += (waiting - keep) * hop;
        analysed += hop;
        advanced = analysed - before;
        for (int i = 0; i < size; ++i)
        {
            const uint32_t at = (analysed - size + i) % ringSize;
            const float left = leftRing[at].load (std::memory_order_relaxed), right = rightRing[at].load (std::memory_order_relaxed);
            const float sample = settings.channel == Channel::left ? left : settings.channel == Channel::right ? right : 0.5f * (left + right);
            buffer[i] = sample * window[i];
        }
        transform.fft (buffer, bins);
        decibels.resize (size / 2);
        for (int k = 1; k < size / 2; ++k)
            decibels[k] = std::max (-160.0f, 20 * std::log10 (std::abs (bins[k]) * amplitudeScale + 1e-12f));
        decibels[0] = decibels[1]; // as Listener's: DC shows as its neighbour
        if (settings.smoothing > 0)
        {
            if (smoothed.size() != decibels.size())
                smoothed = decibels;
            for (size_t k = 0; k < decibels.size(); ++k)
                decibels[k] = smoothed[k] = settings.smoothing * smoothed[k] + (1 - settings.smoothing) * decibels[k];
        }
        return true;
    }

private:
    static constexpr int ringSize = 65536; // four of the largest FFT: enough for the analysis to fall a while behind
    static_assert (ringSize >= 2 * maxSize);
    Settings settings { 0, 0 };
    std::vector<float> window, buffer, smoothed;
    std::vector<std::complex<float>> bins;
    signalsmith::fft::RealFFT<float> transform;
    float amplitudeScale = 1;
    uint32_t analysed = 0; // the sample the last column ended at
    std::atomic<uint32_t> writeIndex { 0 };
    std::atomic<float> leftRing[ringSize] {}, rightRing[ringSize] {};
};
} // namespace chardsp
