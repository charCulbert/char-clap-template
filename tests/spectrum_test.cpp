// The spectrum reads true levels: sines at their level (dB re full-scale sine),
// with little leakage, at low, mid and high frequencies; pink noise flat.
#include "../chardsp_Spectrum.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>

constexpr float sampleRate = 48000, minHz = 20, maxHz = 20000;

static int bandOf(float hz)
{
    const double ratio = std::pow(double(maxHz) / minHz, 1.0 / chardsp::Spectrum::bandCount);
    return int(std::log(hz / minHz) / std::log(ratio));
}

// Feeds `seconds` of a signal in 512-sample blocks, analyzing 60 times a second.
template <typename Signal>
static std::vector<double> run(Signal signal, float seconds)
{
    auto spectrum = std::make_unique<chardsp::Spectrum>();
    std::vector<double> bands;
    float block[512];
    int sinceUpdate = 0;
    for (int n = 0; n < int(seconds * sampleRate); n += 512)
    {
        for (int i = 0; i < 512; ++i) block[i] = signal(n + i);
        spectrum->push(block, block, 512);
        if ((sinceUpdate += 512) >= sampleRate / 60)
            sinceUpdate = 0, spectrum->analyze(sampleRate, minHz, maxHz, 60, bands);
    }
    return bands;
}

int main()
{
    const double pi = 3.14159265358979;
    for (float hz : {40.0f, 100.0f, 1000.0f, 5000.0f, 15000.0f})
    {
        const auto bands = run([&](int n) { return float(0.5 * std::sin(2 * pi * hz * n / sampleRate)); }, 2);
        const int b = bandOf(hz);
        const double peak = std::max({bands[b - 1], bands[b], bands[b + 1]});
        // An octave away, the sine's leakage is far below it.
        const double octaveUp = bands[std::min(b + 26, chardsp::Spectrum::bandCount - 1)];
        std::printf("%6.0f Hz sine at -6.02 dB: reads %6.2f dB, an octave up %7.1f dB\n", hz, peak, octaveUp);
        assert(std::abs(peak - -6.02) < 0.5);
        assert(hz * 2 > maxHz || octaveUp < peak - 60);
    }

    // Anywhere between bands: 60 frequencies from 30 Hz to 18 kHz, none off by
    // more than a fraction of a dB (no scalloping between band centres).
    double worst = 0;
    for (int i = 0; i < 60; ++i)
    {
        const double hz = 30 * std::pow(600.0, i / 59.0);
        const auto bands = run([&](int n) { return float(0.5 * std::sin(2 * pi * hz * n / sampleRate)); }, 0.6f);
        const int b = bandOf(float(hz));
        worst = std::max(worst, std::abs(std::max({bands[b - 1], bands[b], bands[b + 1]}) - -6.02));
    }
    std::printf("sweep 30 Hz to 18 kHz: worst error %.2f dB\n", worst);
    assert(worst < 0.3);

    // Pink noise (Paul Kellet's filter): the same power per band, so flat.
    std::mt19937 random(1);
    std::uniform_real_distribution<float> uniform(-1, 1);
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    const auto bands = run([&](int) {
        const float w = uniform(random);
        b0 = 0.99886f * b0 + w * 0.0555179f; b1 = 0.99332f * b1 + w * 0.0750759f;
        b2 = 0.96900f * b2 + w * 0.1538520f; b3 = 0.86650f * b3 + w * 0.3104856f;
        b4 = 0.55000f * b4 + w * 0.5329522f; b5 = -0.7616f * b5 - w * 0.0168980f;
        const float pink = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f) * 0.05f;
        b6 = w * 0.115926f;
        return pink;
    }, 4);
    // Average over third-octave groups, above the resolution limit.
    double lowest = 1e9, highest = -1e9;
    for (float hz = 500; hz < 16000; hz *= 1.26f)
    {
        double sum = 0;
        for (int b = bandOf(hz); b < bandOf(hz * 1.26f); ++b) sum += bands[b];
        const double mean = sum / (bandOf(hz * 1.26f) - bandOf(hz));
        lowest = std::min(lowest, mean), highest = std::max(highest, mean);
    }
    std::printf("pink noise, 500 Hz to 16 kHz: third-octave means within %.2f dB\n", highest - lowest);
    assert(highest - lowest < 2.0);
}
