#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>
#include "dsp/fft.h" // Signalsmith Audio's FFT (MIT)

namespace chardsp
{
// A short-time Fourier transform for a spectrogram, as Listener's: a
// Hann-windowed FFT of `size` samples every `hop`, each one a column of levels
// in dB per bin (bin k at k · sampleRate / size Hz), relative to a full-scale
// sine, which reads 0 dB.
//
// The audio thread only copies samples into a ring buffer. next() runs on
// another thread and returns each column that has arrived since its last call.
struct Stft
{
    static constexpr int size = 2048, hop = 512;

    Stft()
    {
        window.resize (size);
        double sum = 0;
        for (int i = 0; i < size; ++i)
            sum += window[i] = float (0.5 - 0.5 * std::cos (2 * M_PI * i / size));
        amplitudeScale = float (2 / sum); // a sine of amplitude A peaks at A·Σw/2 in the FFT
        transform.setSize (size);
        buffer.resize (size);
        bins.resize (size / 2);
    }

    // Audio thread: never blocks or allocates.
    void push (const float* left, const float* right, uint32_t frames) noexcept
    {
        uint32_t index = writeIndex.load (std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames; ++i, ++index)
            ring[index % ringSize].store (0.5f * (left[i] + right[i]), std::memory_order_relaxed);
        writeIndex.store (index, std::memory_order_release);
    }

    // Other thread: the next column into `decibels` (size / 2 values), if a
    // hop's worth of samples has arrived since the last. Columns the ring has
    // already overwritten are skipped.
    bool next (std::vector<float>& decibels)
    {
        const uint32_t end = writeIndex.load (std::memory_order_acquire);
        if (end < uint32_t (size))
            return false;
        if (end - analysed > uint32_t (ringSize - size)) // fell behind: start from the newest
            analysed = end - hop;
        if (end - analysed < uint32_t (hop))
            return false;
        analysed += hop;
        for (int i = 0; i < size; ++i)
            buffer[i] = ring[(analysed - size + i) % ringSize].load (std::memory_order_relaxed) * window[i];
        transform.fft (buffer, bins);
        decibels.resize (size / 2);
        for (int k = 0; k < size / 2; ++k)
            decibels[k] = 20 * std::log10 (std::max (std::abs (bins[k]) * amplitudeScale, 1e-10f));
        return true;
    }

private:
    static constexpr int ringSize = 65536;
    std::vector<float> window, buffer;
    std::vector<std::complex<float>> bins;
    signalsmith::fft::RealFFT<float> transform;
    float amplitudeScale = 1;
    uint32_t analysed = 0; // the sample the last column ended at
    std::atomic<uint32_t> writeIndex {0};
    std::atomic<float> ring[ringSize] {};
};
} // namespace chardsp
