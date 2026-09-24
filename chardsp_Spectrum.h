#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace chardsp
{
// A spectrum for display. The audio thread only copies samples into a ring
// buffer; analyze() runs on another thread, takes the latest fftSize samples,
// and reduces the FFT to log-spaced bands in dB that fall back slowly.
struct Spectrum
{
    static constexpr int fftSize = 2048;
    static constexpr int bandCount = 256;

    // Audio thread: never blocks or allocates.
    void push (const float* left, const float* right, uint32_t frames) noexcept
    {
        uint32_t index = writeIndex.load (std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames; ++i, ++index)
            ring[index % ringSize].store (0.5f * (left[i] + right[i]), std::memory_order_relaxed);
        writeIndex.store (index, std::memory_order_release);
    }

    // Main thread: bands in dB from minHz to maxHz. Returns false until the
    // first fftSize samples have arrived.
    bool analyze (float sampleRate, float minHz, float maxHz, std::vector<double>& bands)
    {
        const uint32_t end = writeIndex.load (std::memory_order_acquire);
        if (end < fftSize)
            return false;
        buffer.resize (fftSize);
        for (int i = 0; i < fftSize; ++i)
        {
            const float hann = 0.5f - 0.5f * std::cos (2.0f * pi * i / (fftSize - 1));
            buffer[i] = ring[(end - fftSize + i) % ringSize].load (std::memory_order_relaxed) * hann;
        }
        fft (buffer);

        // Magnitude of a frequency, interpolated between FFT bins; a Hann window
        // halves the amplitude, so a full-scale sine reads about 0 dB.
        auto magnitude = [&] (float hz) {
            const float bin = std::clamp (hz / sampleRate * fftSize, 0.0f, fftSize / 2.0f - 1.001f);
            const int k = int (bin);
            const float t = bin - k;
            return (std::abs (buffer[k]) * (1 - t) + std::abs (buffer[k + 1]) * t) * 4.0f / fftSize;
        };
        smoothed.resize (bandCount, -120.0f);
        bands.resize (bandCount);
        const float ratio = std::pow (maxHz / minHz, 1.0f / bandCount);
        for (int b = 0; b < bandCount; ++b)
        {
            // The loudest bin within the band, or the interpolated value where
            // a band is narrower than a bin (low frequencies).
            const float lo = minHz * std::pow (ratio, float (b)), hi = lo * ratio;
            float peak = magnitude (std::sqrt (lo * hi));
            for (int k = int (std::ceil (lo / sampleRate * fftSize)); k <= int (hi / sampleRate * fftSize) && k < fftSize / 2; ++k)
                peak = std::max (peak, std::abs (buffer[k]) * 4.0f / fftSize);
            const float db = std::max (20.0f * std::log10 (peak + 1e-9f), -120.0f);
            smoothed[b] = std::max (db, smoothed[b] - 1.5f); // rise at once, fall 1.5 dB per update
            bands[b] = smoothed[b];
        }
        return true;
    }

private:
    static constexpr int ringSize = 4096;
    static constexpr float pi = 3.14159265358979f;

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
            const auto step = std::polar (1.0f, -2.0f * pi / length);
            for (size_t i = 0; i < n; i += length)
            {
                std::complex<float> w = 1;
                for (size_t k = 0; k < length / 2; ++k, w *= step)
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
    std::vector<std::complex<float>> buffer; // main thread only
    std::vector<float> smoothed;             // main thread only
};
} // namespace chardsp
