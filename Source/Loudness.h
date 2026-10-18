#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <vector>

// Loudness as streaming services and broadcasters measure it (ITU-R BS.1770-4 / EBU R128): the sound goes through a "K" filter (a shelf that
// lifts the highs a little, and a high-pass), the energy is measured over windows of 400 ms (momentary) or 3 s (short-term), and the whole
// piece is judged with gates that ignore silence (integrated). The unit is LUFS: -14 LUFS is about what streaming services aim for.
namespace Loudness
{
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() noexcept { z1 = z2 = 0.0; }
    };

    // The two filters of the K-weighting (the coefficients of BS.1770 for any sample rate)
    struct KFilter
    {
        Biquad shelf, highPass;

        void prepare (double rate)
        {
            {
                const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
                const double k = std::tan (juce::MathConstants<double>::pi * f0 / rate);
                const double vh = std::pow (10.0, g / 20.0), vb = std::pow (vh, 0.4996667741545416);
                const double a0 = 1.0 + k / q + k * k;
                shelf.b0 = (vh + vb * k / q + k * k) / a0;
                shelf.b1 = 2.0 * (k * k - vh) / a0;
                shelf.b2 = (vh - vb * k / q + k * k) / a0;
                shelf.a1 = 2.0 * (k * k - 1.0) / a0;
                shelf.a2 = (1.0 - k / q + k * k) / a0;
            }
            {
                const double f0 = 38.13547087602444, q = 0.5003270373238773;
                const double k = std::tan (juce::MathConstants<double>::pi * f0 / rate);
                const double a0 = 1.0 + k / q + k * k;
                highPass.b0 = 1.0;
                highPass.b1 = -2.0;
                highPass.b2 = 1.0;
                highPass.a1 = 2.0 * (k * k - 1.0) / a0;
                highPass.a2 = (1.0 - k / q + k * k) / a0;
            }
            reset();
        }

        void reset() { shelf.reset(); highPass.reset(); }
        double process (double x) noexcept { return highPass.process (shelf.process (x)); }
    };

    inline double lufs (double meanSquare) { return meanSquare > 1.0e-12 ? -0.691 + 10.0 * std::log10 (meanSquare) : -120.0; }

    // The integrated loudness of a list of 400 ms block energies (mean squares): the blocks above -70 LUFS, then those within 10 LU of their average.
    inline double integratedOf (const std::vector<double>& blocks)
    {
        double sum = 0.0;
        int n = 0;
        for (double b : blocks)
            if (lufs (b) > -70.0) { sum += b; ++n; }
        if (n == 0)
            return -120.0;
        const double relativeGate = lufs (sum / n) - 10.0;
        sum = 0.0;
        n = 0;
        for (double b : blocks)
            if (lufs (b) > -70.0 && lufs (b) > relativeGate) { sum += b; ++n; }
        return n > 0 ? lufs (sum / n) : -120.0;
    }

    // The integrated loudness of a piece of audio (a reference track): 400 ms blocks every 100 ms
    inline double integratedOfAudio (const juce::AudioBuffer<float>& audio, double rate)
    {
        const int total = audio.getNumSamples();
        const int hop = juce::jmax (1, (int) std::lround (rate * 0.1));
        if (total < hop * 4 || audio.getNumChannels() < 1)
            return -120.0;
        KFilter fl, fr;
        fl.prepare (rate);
        fr.prepare (rate);
        const float* l = audio.getReadPointer (0);
        const float* r = audio.getNumChannels() > 1 ? audio.getReadPointer (1) : l;
        std::vector<double> hops;
        for (int start = 0; start + hop <= total; start += hop)
        {
            double sum = 0.0;
            for (int i = 0; i < hop; ++i)
            {
                const double a = fl.process (l[start + i]), b = fr.process (r[start + i]);
                sum += a * a + b * b;
            }
            hops.push_back (sum / hop);
        }
        std::vector<double> blocks;
        for (size_t j = 0; j + 4 <= hops.size(); ++j)
            blocks.push_back ((hops[j] + hops[j + 1] + hops[j + 2] + hops[j + 3]) / 4.0);
        return integratedOf (blocks);
    }
}

// The loudness meter of the master: the audio thread feeds it, the window reads it.
class LoudnessMeter
{
public:
    static constexpr int ringSize = 36000;   // 100 ms blocks: an hour

    void prepare (double rate)
    {
        sampleRate = rate;
        hop = juce::jmax (1, (int) std::lround (rate * 0.1));
        left.prepare (rate);
        right.prepare (rate);
        count = 0;
        sum = 0.0;
        restart();
    }

    // Audio thread
    void process (const float* l, const float* r, int n) noexcept
    {
        float p = peak.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            const double a = left.process (l[i]), b = right.process (r != nullptr ? r[i] : l[i]);
            sum += a * a + b * b;
            p = std::max (p, std::max (std::abs (l[i]), std::abs (r != nullptr ? r[i] : l[i])));
            if (++count >= hop)
            {
                const auto w = written.load (std::memory_order_relaxed);
                ring[(size_t) (w % ringSize)].store (sum / hop, std::memory_order_relaxed);
                written.store (w + 1, std::memory_order_release);
                count = 0;
                sum = 0.0;
            }
        }
        peak.store (p, std::memory_order_relaxed);
    }

    // Message thread
    double momentary() const { return window (4); }
    double shortTerm() const { return window (30); }
    float samplePeak() const { return peak.load(); }

    double integrated() const
    {
        const auto end = written.load (std::memory_order_acquire);
        const auto first = juce::jmax (startBlock.load(), end - ringSize);
        std::vector<double> blocks;
        for (auto j = first; j + 4 <= end; ++j)
        {
            double s = 0.0;
            for (int k = 0; k < 4; ++k)
                s += ring[(size_t) ((j + k) % ringSize)].load (std::memory_order_relaxed);
            blocks.push_back (s / 4.0);
        }
        return Loudness::integratedOf (blocks);
    }

    // Starts the integrated loudness and the peak again (from now on)
    void restart()
    {
        startBlock = written.load();
        peak = 0.0f;
    }

    bool hasSignal() const { return written.load() > startBlock.load(); }

private:
    double window (int hops) const
    {
        const auto end = written.load (std::memory_order_acquire);
        if (end < hops)
            return -120.0;
        double s = 0.0;
        for (int k = 1; k <= hops; ++k)
            s += ring[(size_t) ((end - k) % ringSize)].load (std::memory_order_relaxed);
        return Loudness::lufs (s / hops);
    }

    double sampleRate = 44100.0;
    int hop = 4410, count = 0;
    double sum = 0.0;
    Loudness::KFilter left, right;
    std::array<std::atomic<double>, ringSize> ring;
    std::atomic<long long> written { 0 }, startBlock { 0 };
    std::atomic<float> peak { 0.0f };
};

//==============================================================================
// A short history of the master's sound for the spectrum display: the audio thread writes, the window reads the newest samples.
class SpectrumTap
{
public:
    static constexpr int size = 16384;

    void write (const float* l, const float* r, int n) noexcept
    {
        auto w = written.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
            data[(size_t) ((w + i) % size)].store (0.5f * (l[i] + (r != nullptr ? r[i] : l[i])), std::memory_order_relaxed);
        written.store (w + n, std::memory_order_release);
    }

    bool readLatest (float* dest, int count) const
    {
        const auto end = written.load (std::memory_order_acquire);
        if (end < count)
            return false;
        for (int i = 0; i < count; ++i)
            dest[i] = data[(size_t) ((end - count + i) % size)].load (std::memory_order_relaxed);
        return true;
    }

private:
    std::array<std::atomic<float>, size> data;
    std::atomic<long long> written { 0 };
};

// The magnitude spectrum (dB, 0 = full scale sine) of `n` samples (a power of 2), with a Hann window: n / 2 bins up to the sample rate / 2.
inline void spectrumDb (const float* samples, int n, std::vector<float>& outDb)
{
    std::vector<std::complex<float>> x ((size_t) n);
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = samples[i] * (0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (n - 1)));

    for (int i = 1, j = 0; i < n; ++i)   // bit reversal
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (x[(size_t) i], x[(size_t) j]);
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        const float angle = -2.0f * juce::MathConstants<float>::pi / (float) len;
        const std::complex<float> step (std::cos (angle), std::sin (angle));
        for (int i = 0; i < n; i += len)
        {
            std::complex<float> w (1.0f, 0.0f);
            for (int k = 0; k < len / 2; ++k)
            {
                const auto u = x[(size_t) (i + k)], v = x[(size_t) (i + k + len / 2)] * w;
                x[(size_t) (i + k)] = u + v;
                x[(size_t) (i + k + len / 2)] = u - v;
                w *= step;
            }
        }
    }
    outDb.assign ((size_t) n / 2, -120.0f);
    const float scale = 4.0f / (float) n;   // a full-scale sine reads 0 dB (Hann window: the amplitude is halved)
    for (int i = 0; i < n / 2; ++i)
        outDb[(size_t) i] = 20.0f * std::log10 (juce::jmax (1.0e-7f, std::abs (x[(size_t) i]) * scale));
}
