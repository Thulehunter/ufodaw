#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

// The wavetables of the synth's wavetable engine: a number of sets, each with `numFrames` single-cycle waves that change gradually from the first to the last
// (the "position" of the synth moves through them). Every wave is built from harmonics (sine and cosine terms) and made in `numMips` band-limited versions
// (128, 64, 32 ... 1 harmonics), so a high note reads a version without harmonics above the Nyquist frequency: no aliasing.
// The tables are made once, the first time they are asked for (Synth::prepare does it, so never on the audio thread). Everything is computed in code.
namespace Wavetable
{
    constexpr int numSets = 14, numFrames = 64, size = 2048, numMips = 8, maxHarmonics = 128;

    inline const char* setName (int set)
    {
        static const char* names[numSets] = { "Basic (sine, triangle, saw, square)", "Digital", "Vowels (a e i o u)", "Comb", "Organ drawbars", "Pulse width",
                                              "Wavefold", "Saturated", "Choir (ah oh ee oo)", "Rising harmonics", "Glass", "Hard sync", "FM", "Texture" };
        return names[set < 0 ? 0 : set >= numSets ? numSets - 1 : set];
    }

    // The short lower-case names the scripts use (ufodaw.set_synth { wtTable = "fold" })
    inline const char* setKey (int set)
    {
        static const char* keys[numSets] = { "basic", "digital", "vowels", "comb", "organ", "pulse", "fold", "saturated", "choir", "rising", "glass", "sync", "fm", "texture" };
        return keys[set < 0 ? 0 : set >= numSets ? numSets - 1 : set];
    }

    // Sets that are given as a curve (one cycle of the wave); the others are given as a spectrum
    inline bool isCurve (int set) { return set == 6 || set == 7 || set == 11 || set == 12; }

    // A repeatable pseudo-random number 0..1 for (seed, index)
    inline double hash01 (uint32_t seed, uint32_t i)
    {
        uint32_t x = seed * 2654435761u + i * 40503u + 0x9e3779b9u;
        x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; x *= 3266489917u; x ^= x >> 16;
        return (double) (x >> 8) / 16777216.0;
    }

    // One cycle of a curve set: `x` 0..1 is the place in the cycle, `t` 0..1 the position in the set.
    inline double curve (int set, double t, double x)
    {
        const double th = 6.283185307179586 * x;
        switch (set)
        {
            case 6:   return std::sin (1.5707963267948966 * (1.0 + 7.0 * t) * std::sin (th));                          // a sine folded over and over
            case 7:   return std::tanh ((1.0 + 14.0 * t) * (std::sin (th) + 0.5 * std::sin (2.0 * th + 0.6)));         // pushed into saturation
            case 11:  { const double r = 1.0 + 4.5 * t; return 2.0 * (x * r - std::floor (x * r)) - 1.0; }             // a saw with a sweeping hard sync
            default:  return std::sin (th + 7.0 * t * std::sin (2.0 * th));                                            // FM: modulator at twice the pitch
        }
    }

    // Random spectra that the position morphs through (glass and texture)
    inline void randomSpectrum (uint32_t seed, double slope, double sparse, double t, double* a, double* b)
    {
        const double x = t * 3.0;
        const int seg = x >= 3.0 ? 2 : (int) x;
        const double f = x - seg;
        for (int h = 1; h <= maxHarmonics; ++h)
        {
            double sa = 0.0, sb = 0.0;
            for (int k = 0; k < 2; ++k)
            {
                const uint32_t key = (uint32_t) (seg + k);
                const double amp = std::pow (hash01 (seed + key * 7919u, (uint32_t) h), sparse) * std::pow ((double) h, -slope);
                const double ph = hash01 (seed + key * 104729u + 5u, (uint32_t) h) * 6.283185307179586;
                const double w = k == 0 ? 1.0 - f : f;
                sa += w * amp * std::cos (ph);
                sb += w * amp * std::sin (ph);
            }
            a[h] = sa;
            b[h] = sb;
        }
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
            case 5:   // pulse width from 50 % to 5 %: the difference of two saws
            {
                const double phi = 2.0 * pi * (0.5 - 0.45 * t);
                for (int h = 1; h <= maxHarmonics; ++h)
                {
                    a[h] = (1.0 - std::cos (h * phi)) / h;
                    b[h] = std::sin (h * phi) / h;
                }
                break;
            }
            case 8:   // choir: a, o, ee, oo, a with wider formants than the vowels set (and a third one for the "air")
            {
                static const double f1[5] = { 730, 450, 270, 300, 730 }, f2[5] = { 1090, 800, 2300, 870, 1090 };
                const double x = t * 4.0;
                const int seg = x >= 4.0 ? 3 : (int) x;
                const double f = x - seg;
                const double h1 = (f1[seg] * (1.0 - f) + f1[seg + 1] * f) / 98.0, h2 = (f2[seg] * (1.0 - f) + f2[seg + 1] * f) / 98.0;
                for (int h = 1; h <= maxHarmonics; ++h)
                {
                    const double e1 = std::exp (-std::pow ((h - h1) / (1.2 + 0.25 * h1), 2.0)), e2 = std::exp (-std::pow ((h - h2) / (1.5 + 0.12 * h2), 2.0));
                    const double e3 = std::exp (-std::pow ((h - 2600.0 / 98.0) / 4.0, 2.0));
                    a[h] = std::pow ((double) h, -0.8) * (0.1 + 5.0 * e1 + 3.0 * e2 + 1.0 * e3);
                }
                break;
            }
            case 9:   // rising harmonics: a saw that gets more and more overtones
            {
                const double count = 1.0 + 127.0 * t * t;
                for (int h = 1; h <= maxHarmonics; ++h)
                    a[h] = std::clamp (count - h + 1.0, 0.0, 1.0) / h;
                break;
            }
            case 10:  randomSpectrum (1234u, 0.9, 2.0, t, a, b); break;   // glass: a few strong overtones that drift
            case 13:  randomSpectrum (777u, 0.55, 4.0, t, a, b); break;   // texture: sparse, noisy overtones
            default:  break;   // (the curve sets are turned into a spectrum in Bank)
        }
    }

    // A small FFT (power of two size). `inverse` uses e^(+i...); nothing is scaled.
    inline void fft (std::vector<std::complex<double>>& x, bool inverse)
    {
        const size_t n = x.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap (x[i], x[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const double ang = (inverse ? 6.283185307179586 : -6.283185307179586) / (double) len;
            const std::complex<double> wl (std::cos (ang), std::sin (ang));
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<double> w (1.0, 0.0);
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const auto u = x[i + k], v = x[i + k + len / 2] * w;
                    x[i + k] = u + v;
                    x[i + k + len / 2] = u - v;
                    w *= wl;
                }
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
            double a[maxHarmonics + 1], b[maxHarmonics + 1];
            std::vector<std::complex<double>> z (size);
            for (int set = 0; set < numSets; ++set)
                for (int f = 0; f < numFrames; ++f)
                {
                    const double t = (double) f / (numFrames - 1);
                    if (isCurve (set))
                    {
                        // the curve to its harmonics (z[h] = b - i a, see below)
                        for (int i = 0; i < size; ++i)
                            z[(size_t) i] = curve (set, t, (double) i / size);
                        fft (z, false);
                        for (int h = 0; h <= maxHarmonics; ++h)
                        {
                            const auto c = z[(size_t) h] * (2.0 / size);
                            b[h] = c.real();
                            a[h] = -c.imag();
                        }
                        a[0] = b[0] = 0.0;
                    }
                    else
                        spectrum (set, t, a, b);

                    double scale = 1.0;   // (the loudness of a frame follows the full version: a high note is not louder than a low one)
                    for (int mip = 0; mip < numMips; ++mip)
                    {
                        const int top = maxHarmonics >> mip;
                        std::fill (z.begin(), z.end(), std::complex<double> (0.0, 0.0));
                        for (int h = 1; h <= top; ++h)
                            z[(size_t) h] = std::complex<double> (b[h], -a[h]);   // Re (z e^(i h x)) = b cos (h x) + a sin (h x)
                        fft (z, true);
                        if (mip == 0)
                        {
                            double peak = 1.0e-9;
                            for (int i = 0; i < size; ++i) peak = std::max (peak, std::abs (z[(size_t) i].real()));
                            scale = 1.0 / peak;
                        }
                        float* out = data.data() + (size_t) (((set * numFrames + f) * numMips + mip) * (size + 1));
                        for (int i = 0; i < size; ++i)
                            out[i] = (float) (z[(size_t) i].real() * scale);
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
