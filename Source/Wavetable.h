#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

// The wavetables of the synth's wavetable engine: a few sets, each with `numFrames` single-cycle waves that change gradually from the first to the last
// (the "position" of the synth moves through them). Every wave is built from harmonics (sine and cosine terms) and made in `numMips` band-limited versions
// (128, 64, 32 ... 1 harmonics), so a high note reads a version without harmonics above the Nyquist frequency: no aliasing.
// The tables are made once, the first time they are asked for (Synth::prepare does it, so never on the audio thread).
namespace Wavetable
{
    constexpr int numSets = 6, numFrames = 16, size = 2048, numMips = 8, maxHarmonics = 128;

    inline const char* setName (int set)
    {
        static const char* names[numSets] = { "Basic (sine, triangle, saw, square)", "Digital", "Vowels (a e i o u)", "Comb", "Organ drawbars", "Pulse width" };
        return names[set < 0 ? 0 : set >= numSets ? numSets - 1 : set];
    }

    // The spectrum of a wave: harmonic h (1..maxHarmonics) = sin(h x) * a[h] + cos(h x) * b[h]; `t` 0..1 is the position in the set.
    inline void spectrum (int set, double t, double* a, double* b)
    {
        const double pi = 3.141592653589793;
        for (int h = 0; h <= maxHarmonics; ++h)
            a[h] = b[h] = 0.0;

        switch (set)
        {
            case 0:   // sine -> triangle -> saw -> square
            {
                const double x = t * 3.0;
                const int seg = x >= 3.0 ? 2 : (int) x;
                const double f = x - seg;
                auto shape = [] (int which, int h)
                {
                    const double pi2 = 3.141592653589793;
                    switch (which)
                    {
                        case 0:  return h == 1 ? 1.0 : 0.0;
                        case 1:  return (h & 1) ? ((((h - 1) / 2) & 1) ? -1.0 : 1.0) * 8.0 / (pi2 * pi2 * h * h) : 0.0;
                        case 2:  return ((h & 1) ? 1.0 : -1.0) * 2.0 / (pi2 * h);
                        default: return (h & 1) ? 4.0 / (pi2 * h) : 0.0;
                    }
                };
                for (int h = 1; h <= maxHarmonics; ++h)
                    a[h] = shape (seg, h) * (1.0 - f) + shape (seg + 1, h) * f;
                break;
            }
            case 1:   // digital: an uneven set of harmonics that shifts with the position
                for (int h = 1; h <= maxHarmonics; ++h)
                    a[h] = std::pow ((double) h, -0.7) * (0.2 + 0.8 * std::abs (std::sin ((double) h * (0.35 + 0.9 * t) + 4.0 * t)));
                break;
            case 2:   // vowels: two formants (as harmonic numbers of a 110 Hz voice) that move from a to e to i to o to u
            {
                static const double f1[5] = { 800, 400, 300, 450, 325 }, f2[5] = { 1200, 2000, 2500, 800, 700 };
                const double x = t * 4.0;
                const int seg = x >= 4.0 ? 3 : (int) x;
                const double f = x - seg;
                const double h1 = (f1[seg] * (1.0 - f) + f1[seg + 1] * f) / 110.0, h2 = (f2[seg] * (1.0 - f) + f2[seg + 1] * f) / 110.0;
                for (int h = 1; h <= maxHarmonics; ++h)
                {
                    const double e1 = std::exp (-std::pow ((h - h1) / 1.6, 2.0)), e2 = std::exp (-std::pow ((h - h2) / (1.5 + 0.1 * h2), 2.0));
                    a[h] = (1.0 / h) * (0.15 + 6.0 * e1 + 3.0 * e2);
                }
                break;
            }
            case 3:   // comb: the spacing of the strong harmonics changes
                for (int h = 1; h <= maxHarmonics; ++h)
                    a[h] = std::pow ((double) h, -0.5) * (0.5 + 0.5 * std::cos (2.0 * pi * h / (2.0 + 10.0 * t)));
                break;
            case 4:   // organ drawbars: harmonics 1 2 3 4 5 6 8 with levels that change
            {
                static const int bars[7] = { 1, 2, 3, 4, 5, 6, 8 };
                for (int i = 0; i < 7; ++i)
                    a[bars[i]] = 0.5 + 0.5 * std::sin (2.0 * pi * (t * (i + 1) * 0.37 + i * 0.21));
                a[1] = std::max (a[1], 0.4);
                break;
            }
            default:  // pulse width from 50 % to 5 %: the difference of two saws
            {
                const double phi = 2.0 * pi * (0.5 - 0.45 * t);
                for (int h = 1; h <= maxHarmonics; ++h)
                {
                    a[h] = (1.0 - std::cos (h * phi)) / h;
                    b[h] = std::sin (h * phi) / h;
                }
                break;
            }
        }
    }

    struct Bank
    {
        std::vector<float> data;   // [set][frame][mip][size + 1] (the last sample repeats the first: no wrap in the interpolation)

        const float* frame (int set, int frame, int mip) const
        {
            return data.data() + (size_t) (((set * numFrames + frame) * numMips + mip) * (size + 1));
        }

        Bank()
        {
            data.assign ((size_t) numSets * numFrames * numMips * (size + 1), 0.0f);
            std::vector<double> sine (size);
            for (int i = 0; i < size; ++i)
                sine[(size_t) i] = std::sin (6.283185307179586 * i / size);

            double a[maxHarmonics + 1], b[maxHarmonics + 1];
            std::vector<double> wave (size);
            for (int set = 0; set < numSets; ++set)
                for (int f = 0; f < numFrames; ++f)
                {
                    spectrum (set, (double) f / (numFrames - 1), a, b);
                    double scale = 0.0;   // (the loudness of a frame follows the full version: a high note is not louder than a low one)
                    for (int mip = 0; mip < numMips; ++mip)
                    {
                        const int top = maxHarmonics >> mip;
                        for (int i = 0; i < size; ++i)
                        {
                            double s = 0.0;
                            for (int h = 1; h <= top; ++h)
                                s += a[h] * sine[(size_t) ((h * i) & (size - 1))] + b[h] * sine[(size_t) ((h * i + size / 4) & (size - 1))];
                            wave[(size_t) i] = s;
                        }
                        if (mip == 0)
                        {
                            double peak = 1.0e-9;
                            for (double w : wave) peak = std::max (peak, std::abs (w));
                            scale = 1.0 / peak;
                        }
                        float* out = data.data() + (size_t) (((set * numFrames + f) * numMips + mip) * (size + 1));
                        for (int i = 0; i < size; ++i)
                            out[i] = (float) (wave[(size_t) i] * scale);
                        out[size] = out[0];
                    }
                }
        }
    };

    inline const Bank& bank()
    {
        static const Bank b;
        return b;
    }

    // The version of a wave for a note that advances `increment` of a cycle per sample (0.5 = Nyquist)
    inline int mipFor (double increment)
    {
        const double allowed = 0.45 / std::max (1.0e-6, increment);   // harmonics that stay below the Nyquist frequency
        int mip = 0;
        while (mip < numMips - 1 && (maxHarmonics >> mip) > allowed)
            ++mip;
        return mip;
    }

    // One sample: `phase` 0..1 in the wave, `frac` the way from frame a to frame b
    inline float read (const float* a, const float* b, float frac, double phase)
    {
        const double p = phase * size;
        const int i = (int) p;
        const float f = (float) (p - i);
        const float x = a[i] + f * (a[i + 1] - a[i]);
        const float y = b[i] + f * (b[i + 1] - b[i]);
        return x + frac * (y - x);
    }
}
