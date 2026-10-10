// The constant-Q transform here follows CiCueTea's NsgfCqtSparse
// (https://github.com/jdsierral/CiCueTea), under its licence:
//
// MIT License
//
// Copyright (c) 2024-2026 Juan Sierra
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include "dsp/fft.h" // chardsp has "signalsmith-dsp/fft.h"; this example includes Signalsmith DSP as dsp/

namespace chardsp
{

// The bands of a constant-Q transform: referenceFrequency · 2^(k / bandsPerOctave)
// for every whole k from minFrequency to maxFrequency, analysed over blocks of
// blockSize samples (a power of two; 0 picks minimumBlockSize()).
struct ConstantQDesign
{
    double sampleRate = 48000.0;
    double bandsPerOctave = 24.0;
    double minFrequency = 55.0;
    double maxFrequency = 8000.0;
    double referenceFrequency = 440.0;
    int blockSize = 0;
};

inline bool operator== (const ConstantQDesign& a,
                        const ConstantQDesign& b) noexcept
{
    return a.sampleRate == b.sampleRate
        && a.bandsPerOctave == b.bandsPerOctave
        && a.minFrequency == b.minFrequency
        && a.maxFrequency == b.maxFrequency
        && a.referenceFrequency == b.referenceFrequency
        && a.blockSize == b.blockSize;
}

inline bool operator!= (const ConstantQDesign& a,
                        const ConstantQDesign& b) noexcept
{
    return ! (a == b);
}

// How a band's level over a stretch of time becomes one value: its magnitude
// at the stretch's middle, the largest magnitude in it, or the root mean
// square of the magnitudes in it.
enum class ConstantQReading
{
    centre,
    peak,
    mean
};

// A constant-Q transform: the sparse non-stationary Gabor CQT of CiCueTea
// (Juan Sierra, MIT), forward only. Each band weighs the block's spectrum by a
// Gaussian in log frequency, e^(-c · (log2 f - log2 fk)²) with
// c = ln 4 · bandsPerOctave², leaving out weights of 1e-6 or less; its
// coefficients are the inverse transform of that, a complex envelope over the
// block, which a full-scale sine on the band's centre holds near 1.
//
// The bands, weights and coefficients are those of CiCueTea's NsgfCqtSparse
// built with minFrequency / 2^(1 / bandsPerOctave) and
// maxFrequency · 2^(1 / bandsPerOctave) (Listener's way of using it), less its
// two edge bands, which are low- and high-pass rather than constant-Q.
//
// process() takes one real FFT of a block; everything after reads that
// spectrum. A coefficient at any time is a weighted sum over the band's bins,
// so one value per band (as Listener reads them, at the block's centre) costs
// far less than CiCueTea's inverse FFT per band. Like CiCueTea's, the
// transform treats the block as periodic: a whole file analysed as one block
// wants silence padded at both ends.
//
// prepare() allocates; the rest does not.
template <typename SampleType>
class ConstantQ
{
public:
    static_assert (std::is_same_v<SampleType, float>
                   || std::is_same_v<SampleType, double>,
                   "ConstantQ supports float or double");

    // Listener's block: the shortest power of two holding bandsPerOctave
    // periods of minFrequency, enough for the lowest band's atom.
    static int minimumBlockSize (double sampleRate, double bandsPerOctave,
                                 double minFrequency) noexcept
    {
        return nextPowerOfTwo (static_cast<int64_t> (
            std::ceil (sampleRate * bandsPerOctave / minFrequency)));
    }

    // False, leaving no bands, for a design without a band, with a band at or
    // above Nyquist, or with a band the block can't resolve (fewer than four
    // bins, CiCueTea's limit).
    bool prepare (const ConstantQDesign& newDesign)
    {
        design = newDesign;
        clear();
        const auto& d = design;

        if (! (d.sampleRate > 0.0) || ! (d.bandsPerOctave > 0.0)
            || ! (d.minFrequency > 0.0)
            || ! (d.minFrequency <= d.maxFrequency)
            || ! (2.0 * d.maxFrequency < d.sampleRate)
            || ! (d.referenceFrequency > 0.0)
            || ! std::isfinite (d.sampleRate + d.bandsPerOctave
                                + d.maxFrequency + d.referenceFrequency))
            return false;

        if (design.blockSize == 0)
            design.blockSize = minimumBlockSize (d.sampleRate,
                                                 d.bandsPerOctave,
                                                 d.minFrequency);

        size = design.blockSize;

        if (size < 4 || size > (1 << 30) || (size & (size - 1)) != 0)
            return clear();

        // CiCueTea's arithmetic, in its order, so these are the same doubles.
        const double fraction = 1.0 / d.bandsPerOctave;
        const double c = std::log (4.0) / (fraction * fraction);
        const double rate = d.sampleRate;
        const double reach = std::sqrt (std::log (1.0 / threshold) / c)
                           + 1.0e-3; // octaves either side above threshold
        const auto lowest = static_cast<int64_t> (std::floor (
            d.bandsPerOctave * std::log2 (d.minFrequency
                                          / d.referenceFrequency))) - 1;
        const auto highest = static_cast<int64_t> (std::ceil (
            d.bandsPerOctave * std::log2 (d.maxFrequency
                                          / d.referenceFrequency))) + 1;

        for (auto k = lowest; k <= highest; ++k)
        {
            const double centre = d.referenceFrequency
                * std::exp (fraction * std::log (2.0) * static_cast<double> (k));

            if (centre < d.minFrequency * (1.0 - 1.0e-9)
                || centre > d.maxFrequency * (1.0 + 1.0e-9))
                continue;

            const int from = std::max (1, static_cast<int> (std::floor (
                centre * std::exp2 (-reach) * size / rate)));
            const int to = std::min (size / 2, static_cast<int> (std::ceil (
                centre * std::exp2 (reach) * size / rate)));
            Band band;
            band.offset = weights.size();

            for (int j = from; j <= to; ++j)
            {
                const double difference =
                    std::log2 (static_cast<double> (j) * rate
                               / static_cast<double> (size))
                    - std::log2 (centre);
                const double weight = std::exp (-c * (difference * difference));

                if (weight <= threshold)
                    continue;

                // The Gaussian falls away from its centre, so the bins above
                // the threshold are one run.
                if (band.count == 0)
                    band.first = j;

                weights.push_back (static_cast<SampleType> (
                    2.0 / size * weight));
                ++band.count;
            }

            if (band.count < 4)
                return clear();

            band.length = nextPowerOfTwo (band.count);
            // Readings over a stretch take coefficients a third of the atom's
            // standard deviation in time apart (or CiCueTea's, if closer): a
            // click's peak then reads within 0.15 dB, wherever it falls. Set
            // by time, not bins, so a shorter block has the same grid.
            // Near its centre the Gaussian is one of standard deviation
            // fk · ln 2 / sqrt (2c) Hz, so its atom spreads 1 / (2π) of the
            // reciprocal in time.
            band.spread = rate / (twoPi * centre * std::log (2.0)
                                  / std::sqrt (2.0 * c));
            band.readingLength = std::max (band.length, std::min (
                size, nextPowerOfTwo (static_cast<int64_t> (
                    std::ceil (3.0 * size / band.spread)))));
            bands.push_back (band);
            frequencies.push_back (centre);
        }

        if (bands.empty())
            return clear();

        int longest = 0;

        for (const auto& band : bands)
            longest = std::max (longest, band.readingLength);

        inverses.clear();

        for (int i = 0; i <= log2Of (longest); ++i) // by log2 of the size
            inverses.emplace_back (size_t { 1 } << std::max (i, 2));

        transform.setSize (static_cast<size_t> (size));
        spectrum.assign (static_cast<size_t> (size / 2 + 1), {});
        scratch.assign (static_cast<size_t> (longest), {});
        envelope.assign (static_cast<size_t> (longest), {});
        return true;
    }

    bool isValid() const noexcept { return ! bands.empty(); }
    const ConstantQDesign& getDesign() const noexcept { return design; }
    int getBlockSize() const noexcept { return size; }
    int getNumBands() const noexcept { return static_cast<int> (bands.size()); }

    // How far a band's atom spreads in time: the standard deviation of its
    // magnitude, in samples. A coefficient depends on the samples within
    // about six of these of its time.
    double getSpread (int bandIndex) const noexcept
    {
        return bands[static_cast<size_t> (bandIndex)].spread;
    }

    // Each band's centre in Hz, low to high.
    const std::vector<double>& getFrequencies() const noexcept
    {
        return frequencies;
    }

    // Takes in a block of getBlockSize() samples.
    void process (const SampleType* block) noexcept
    {
        if (! isValid())
            return;

        transform.fft (block, spectrum);
        // The FFT packs Nyquist into DC's imaginary part.
        spectrum[static_cast<size_t> (size / 2)] = spectrum[0].imag();
        spectrum[0].imag (SampleType {});
    }

    // The band's coefficient `time` samples into the block (any real time;
    // the block repeats).
    std::complex<SampleType> getCoefficient (int bandIndex,
                                             double time) const noexcept
    {
        const auto& band = bands[static_cast<size_t> (bandIndex)];
        const double turns = std::fmod (time, static_cast<double> (size))
                           / size;
        const auto start = std::polar (1.0, twoPi * std::fmod (
            band.first * turns, 1.0));
        const auto step = std::polar (1.0, twoPi * turns);
        return static_cast<std::complex<SampleType>> (
            sum (band, start, step));
    }

    // The band's magnitude at the block's centre: the value Listener reads.
    SampleType getCentreMagnitude (int bandIndex) const noexcept
    {
        // At the centre, bin j turns by (-1)^j.
        const auto& band = bands[static_cast<size_t> (bandIndex)];
        const SampleType* w = weights.data() + band.offset;
        const std::complex<SampleType>* x = spectrum.data() + band.first;
        double evenReal = 0.0, evenImag = 0.0, oddReal = 0.0, oddImag = 0.0;
        int m = 0;

        for (; m + 1 < band.count; m += 2)
        {
            evenReal += static_cast<double> (w[m]) * x[m].real();
            evenImag += static_cast<double> (w[m]) * x[m].imag();
            oddReal += static_cast<double> (w[m + 1]) * x[m + 1].real();
            oddImag += static_cast<double> (w[m + 1]) * x[m + 1].imag();
        }

        if (m < band.count)
        {
            evenReal += static_cast<double> (w[m]) * x[m].real();
            evenImag += static_cast<double> (w[m]) * x[m].imag();
        }

        return static_cast<SampleType> (std::hypot (evenReal - oddReal,
                                                    evenImag - oddImag));
    }

    // CiCueTea's coefficients for a band: getNumCoefficients() of them, the
    // nth at n · getBlockSize() / getNumCoefficients() samples into the block.
    int getNumCoefficients (int bandIndex) const noexcept
    {
        return bands[static_cast<size_t> (bandIndex)].length;
    }

    void getCoefficients (int bandIndex,
                          std::complex<SampleType>* coefficients) noexcept
    {
        const auto& band = bands[static_cast<size_t> (bandIndex)];
        inverse (band, band.length);

        for (int n = 0; n < band.length; ++n)
        {
            // CiCueTea's phase: time measured from the block's start.
            const auto turn = static_cast<double> (
                (static_cast<int64_t> (band.first) * n) % band.length)
                / band.length;
            coefficients[n] = envelope[static_cast<size_t> (n)]
                * static_cast<std::complex<SampleType>> (
                    std::polar (1.0, twoPi * turn));
        }
    }

    // A band's level from `from` to `to` samples into the block, read from
    // its coefficients there (at least the one nearest the middle).
    SampleType getMagnitude (int bandIndex, ConstantQReading reading,
                             double from, double to) noexcept
    {
        const double middle = 0.5 * (from + to);

        if (reading == ConstantQReading::centre)
            return middle == 0.5 * size
                ? getCentreMagnitude (bandIndex)
                : std::abs (getCoefficient (bandIndex, middle));

        const auto& band = bands[static_cast<size_t> (bandIndex)];
        const int length = band.readingLength;
        const double spacing = static_cast<double> (size) / length;
        auto first = static_cast<int> (std::ceil (
            std::max (from, 0.0) / spacing));
        auto last = static_cast<int> (std::ceil (
            std::min (to, static_cast<double> (size)) / spacing)) - 1;

        if (first > last)
            first = last = static_cast<int> (std::lround (middle / spacing))
                         % length;

        const int count = last - first + 1;
        double peak = 0.0, power = 0.0;

        // Each coefficient alone costs a sum over the band's bins; all of
        // them, an inverse FFT. Whichever is less work.
        if (static_cast<int64_t> (count) * band.count
            <= static_cast<int64_t> (length) * log2Of (length))
        {
            const auto step = std::polar (1.0, twoPi / length);
            auto rotation = std::polar (1.0, twoPi * first / length);

            for (int n = first; n <= last; ++n, rotation *= step)
            {
                const auto start = std::polar (1.0, twoPi * static_cast<double> (
                    (static_cast<int64_t> (band.first) * n) % length)
                    / length);
                const double magnitude = std::abs (sum (band, start, rotation));
                peak = std::max (peak, magnitude);
                power += magnitude * magnitude;
            }
        }
        else
        {
            inverse (band, length);

            for (int n = first; n <= last; ++n)
            {
                const double magnitude = std::abs (
                    std::complex<double> (envelope[static_cast<size_t> (n)]));
                peak = std::max (peak, magnitude);
                power += magnitude * magnitude;
            }
        }

        return static_cast<SampleType> (reading == ConstantQReading::peak
            ? peak : std::sqrt (power / count));
    }

    // getMagnitude() for every band, into getNumBands() values.
    void getMagnitudes (ConstantQReading reading, double from, double to,
                        SampleType* magnitudes) noexcept
    {
        for (int b = 0; b < getNumBands(); ++b)
            magnitudes[b] = getMagnitude (b, reading, from, to);
    }

    // One band over a run of columns `spacing` samples apart, the first
    // centred `firstTime` samples into the block: one inverse FFT, then each
    // column read from the coefficients within half a spacing of its time
    // (centre: the one nearest). For analysing a long block at any time
    // resolution.
    void getColumns (int bandIndex, ConstantQReading reading,
                     double firstTime, double spacing, int numColumns,
                     SampleType* magnitudes) noexcept
    {
        const auto& band = bands[static_cast<size_t> (bandIndex)];
        const int length = band.readingLength;
        inverse (band, length);
        const double step = static_cast<double> (size) / length;
        const auto at = [&] (int64_t n) {
            n %= length;
            return std::abs (std::complex<double> (envelope[static_cast<size_t> (
                n < 0 ? n + length : n)]));
        };

        for (int c = 0; c < numColumns; ++c)
        {
            const double time = firstTime + c * spacing;
            auto first = static_cast<int64_t> (std::ceil ((time - 0.5 * spacing)
                                                          / step));
            auto last = static_cast<int64_t> (std::ceil ((time + 0.5 * spacing)
                                                         / step)) - 1;

            if (reading == ConstantQReading::centre || first > last)
                first = last = std::llround (time / step);

            double peak = 0.0, power = 0.0;

            for (auto n = first; n <= last; ++n)
            {
                const double magnitude = at (n);
                peak = std::max (peak, magnitude);
                power += magnitude * magnitude;
            }

            magnitudes[c] = static_cast<SampleType> (
                reading == ConstantQReading::mean
                    ? std::sqrt (power / static_cast<double> (last - first + 1))
                    : peak);
        }
    }

private:
    static constexpr double threshold = 1.0e-6; // CiCueTea's
    static constexpr double twoPi = 6.283185307179586476925286766559;

    struct Band
    {
        int first = 0;  // bin
        int count = 0;  // bins
        int length = 0; // coefficients: count rounded up to a power of two
        int readingLength = 0; // coefficients for readings
        double spread = 0.0; // samples
        size_t offset = 0; // into weights
    };

    static int nextPowerOfTwo (int64_t value) noexcept
    {
        int64_t power = 4;

        while (power < value && power < (int64_t { 1 } << 30))
            power *= 2;

        return static_cast<int> (power);
    }

    static int log2Of (int power) noexcept
    {
        int bits = 0;

        while ((1 << bits) < power)
            ++bits;

        return bits;
    }

    bool clear()
    {
        bands.clear();
        frequencies.clear();
        weights.clear();
        size = 0;
        return false;
    }

    // Σ w_m · X_(first + m) · start · step^m, in double.
    std::complex<double> sum (const Band& band, std::complex<double> start,
                              std::complex<double> step) const noexcept
    {
        // Spelled out: std::complex's multiply checks for infinities.
        const SampleType* w = weights.data() + band.offset;
        const std::complex<SampleType>* x = spectrum.data() + band.first;
        const double stepReal = step.real(), stepImag = step.imag();
        double turnReal = start.real(), turnImag = start.imag();
        double totalReal = 0.0, totalImag = 0.0;

        for (int m = 0; m < band.count; ++m)
        {
            const double real = w[m] * static_cast<double> (x[m].real());
            const double imag = w[m] * static_cast<double> (x[m].imag());
            totalReal += real * turnReal - imag * turnImag;
            totalImag += real * turnImag + imag * turnReal;
            const double nextReal = turnReal * stepReal - turnImag * stepImag;
            turnImag = turnReal * stepImag + turnImag * stepReal;
            turnReal = nextReal;
        }

        return { totalReal, totalImag };
    }

    // The band's envelope, without CiCueTea's phase, into `envelope`.
    void inverse (const Band& band, int length) noexcept
    {
        const SampleType* w = weights.data() + band.offset;
        const std::complex<SampleType>* x = spectrum.data() + band.first;
        std::fill (scratch.begin(), scratch.begin() + length,
                   std::complex<SampleType> {});

        for (int m = 0; m < band.count; ++m)
            scratch[static_cast<size_t> (m)] = w[m] * x[m];

        inverses[static_cast<size_t> (log2Of (length))].ifft (
            scratch.data(), envelope.data());
    }

    ConstantQDesign design;
    int size = 0;
    std::vector<Band> bands;
    std::vector<double> frequencies;
    std::vector<SampleType> weights;
    std::vector<std::complex<SampleType>> spectrum, scratch, envelope;
    signalsmith::fft::RealFFT<SampleType> transform;
    std::vector<signalsmith::fft::FFT<SampleType>> inverses;
};

enum class ConstantQChannel
{
    mix,
    left,
    right
};

// A ConstantQ over a live stereo signal, as Listener's spectrogram: a column
// of levels in dB per band (-160 at least) every `hop` samples, from the newest
// block, each band read over the hop centred on the block's middle.
//
// ConstantQReading::centre reads only the middle, as Listener does, and
// matches it: one coefficient per band per hop. A high band's coefficients
// change far faster than that, so a click between two middles goes unseen and
// close tones in one band alias to a slow flicker. peak and mean read every
// coefficient in the hop instead. mean, the default, is the band's power over
// the hop: steady tones read as in centre, noise reads the same in every band
// whatever the hop, and a click reads its energy spread over the hop. peak
// shows a click at its own level, but reads noise higher in the bands with
// more coefficients per hop (the high ones).
//
// For peak and mean, the bands' coefficients over the hop only depend on the
// samples within six of their atom's spreads, so bands are grouped by how many
// samples that is, rounded up to a power of two, and each group analyses only
// those: the same bands, sampled on the coarser bins of a shorter block.
//
// prepare() allocates the ring, holding two of the largest block, while
// nothing pushes. push() runs on the audio thread and only copies both
// channels into the ring. configure() and next() run on another thread (the
// analysis), which sets the transform up and reads the ring; configure()
// allocates, next() does not.
template <typename SampleType>
class ConstantQStream
{
public:
    static_assert (std::is_same_v<SampleType, float>
                   || std::is_same_v<SampleType, double>,
                   "ConstantQStream supports float or double");

    void prepare (int maxBlockSize)
    {
        int capacity = 8;

        while (capacity < 2 * maxBlockSize && capacity < (1 << 30))
            capacity *= 2;

        ringSize = static_cast<uint32_t> (capacity);
        leftRing.reset (new std::atomic<SampleType>[ringSize]());
        rightRing.reset (new std::atomic<SampleType>[ringSize]());
        writeIndex.store (0, std::memory_order_release);
        analysed = 0;
    }

    // False, analysing nothing, when the design isn't valid or its block is
    // over half the ring. Anything changed starts afresh from the newest
    // samples.
    bool configure (const ConstantQDesign& newDesign, int newHop,
                    ConstantQReading newReading = ConstantQReading::mean,
                    ConstantQChannel newChannel = ConstantQChannel::mix)
    {
        newHop = std::max (1, newHop);
        const bool rebuild = newDesign != design || ! transform.isValid();

        if (rebuild)
        {
            design = newDesign;
            transform.prepare (design);
            block.assign (static_cast<size_t> (transform.getBlockSize()),
                          SampleType {});
            magnitudes.assign (static_cast<size_t> (transform.getNumBands()),
                               SampleType {});
        }

        if (rebuild || newHop != hop)
        {
            hop = newHop;
            group();
        }

        reading = newReading;
        channel = newChannel;
        analysed = writeIndex.load (std::memory_order_acquire);
        const auto blockSize = static_cast<uint32_t> (transform.getBlockSize());
        ready = transform.isValid() && blockSize * 2 <= ringSize
             && static_cast<uint32_t> (hop) <= ringSize - blockSize;
        return ready;
    }

    // The bands, with the whole block.
    const ConstantQ<SampleType>& getTransform() const noexcept
    {
        return transform;
    }

    int getHop() const noexcept { return hop; }

    // Audio thread: never blocks or allocates.
    void push (const SampleType* left, const SampleType* right,
               uint32_t frames) noexcept
    {
        if (ringSize == 0)
            return;

        uint32_t index = writeIndex.load (std::memory_order_relaxed);

        for (uint32_t i = 0; i < frames; ++i, ++index)
        {
            leftRing[index % ringSize].store (left[i],
                                              std::memory_order_relaxed);
            rightRing[index % ringSize].store (right[i],
                                               std::memory_order_relaxed);
        }

        writeIndex.store (index, std::memory_order_release);
    }

    // The next column into `decibels` (a value per band), if a hop's worth of
    // samples has arrived since the last, and in `advanced` the samples since
    // the last column. With more than `keep` columns waiting, or any the ring
    // has already overwritten, it skips to the newest few.
    bool next (SampleType* decibels, uint32_t& advanced,
               uint32_t keep = 8) noexcept
    {
        if (! ready)
            return false;

        const auto size = static_cast<uint32_t> (transform.getBlockSize());
        const auto step = static_cast<uint32_t> (hop);
        const uint32_t end = writeIndex.load (std::memory_order_acquire);

        if (end < size)
            return false;

        uint32_t before = analysed;

        if (end - analysed > ringSize - size) // fell behind the ring
            analysed = end - step, before = analysed - step;

        const uint32_t waiting = (end - analysed) / step;

        if (waiting == 0)
            return false;

        if (waiting > keep)
            analysed += (waiting - keep) * step;

        analysed += step;
        advanced = analysed - before;

        for (uint32_t i = 0; i < size; ++i)
        {
            const uint32_t at = (analysed - size + i) % ringSize;
            const auto left = leftRing[at].load (std::memory_order_relaxed);
            const auto right = rightRing[at].load (std::memory_order_relaxed);
            block[i] = channel == ConstantQChannel::left ? left
                : channel == ConstantQChannel::right ? right
                : static_cast<SampleType> (0.5) * (left + right);
        }

        if (reading == ConstantQReading::centre)
        {
            transform.process (block.data());

            for (int b = 0; b < transform.getNumBands(); ++b)
                magnitudes[static_cast<size_t> (b)] =
                    transform.getCentreMagnitude (b);
        }
        else
        {
            const double middle = 0.5 * size;
            bool whole = false;

            for (auto& g : groups)
            {
                if (g.transform.isValid())
                {
                    const auto length = static_cast<uint32_t> (
                        g.transform.getBlockSize());
                    g.transform.process (block.data() + (size - length) / 2);
                    g.transform.getMagnitudes (
                        reading, 0.5 * length - 0.5 * hop,
                        0.5 * length + 0.5 * hop, magnitudes.data() + g.first);
                    continue;
                }

                if (! whole)
                    transform.process (block.data()), whole = true;

                for (int b = g.first; b < g.end; ++b)
                    magnitudes[static_cast<size_t> (b)] = transform.getMagnitude (
                        b, reading, middle - 0.5 * hop, middle + 0.5 * hop);
            }
        }

        for (size_t b = 0; b < magnitudes.size(); ++b)
            decibels[b] = static_cast<SampleType> (std::max (-160.0,
                20.0 * std::log10 (std::max (
                    1.0e-8, static_cast<double> (magnitudes[b])))));

        return true;
    }

private:
    struct Group
    {
        int first = 0, end = 0; // bands
        int length = 0; // samples
        ConstantQ<SampleType> transform; // when not valid, the whole block's
    };

    // Each band's coefficients across the hop need the samples within six of
    // its atom's spreads either side.
    void group()
    {
        groups.clear();
        const int size = transform.getBlockSize();
        int previous = -1;

        for (int b = 0; b < transform.getNumBands(); ++b)
        {
            int length = 4;

            while (length < hop + 12.0 * transform.getSpread (b) && length < size)
                length *= 2;

            if (length != previous)
            {
                groups.emplace_back();
                groups.back().first = b;
                groups.back().length = previous = length;
            }

            groups.back().end = b + 1;
        }

        const auto& frequencies = transform.getFrequencies();

        for (auto& g : groups)
        {
            auto part = design;
            part.minFrequency = frequencies[static_cast<size_t> (g.first)];
            part.maxFrequency = frequencies[static_cast<size_t> (g.end - 1)];
            part.blockSize = g.length;

            // Too long, or bands it can't resolve: the whole block's.
            if (part.blockSize >= size || ! g.transform.prepare (part)
                || g.transform.getNumBands() != g.end - g.first)
                g.transform = ConstantQ<SampleType>();
        }
    }

    ConstantQ<SampleType> transform;
    std::vector<Group> groups;
    ConstantQDesign design;
    int hop = 0;
    ConstantQReading reading = ConstantQReading::mean;
    ConstantQChannel channel = ConstantQChannel::mix;
    bool ready = false;
    std::vector<SampleType> block, magnitudes;
    uint32_t ringSize = 0;
    uint32_t analysed = 0; // the sample the last column ended at
    std::unique_ptr<std::atomic<SampleType>[]> leftRing, rightRing;
    std::atomic<uint32_t> writeIndex { 0 };
};

} // namespace chardsp
