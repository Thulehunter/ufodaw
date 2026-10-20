#include "StarterPack.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace
{
    using Sig = std::vector<float>;
    constexpr double rate = 44100.0;
    constexpr double twoPi = 6.283185307179586;
    constexpr int packVersion = 6;

    Sig make (double seconds) { return Sig ((size_t) (seconds * rate), 0.0f); }
    size_t n (const Sig& s) { return s.size(); }

    // RBJ biquad filters (double precision) for shaping the noise and the metallic sounds
    struct Biq
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        static Biq make (int kind, double f, double q)   // 0 low-pass, 1 high-pass, 2 band-pass
        {
            Biq b;
            const double w = twoPi * juce::jmin (f, rate * 0.45) / rate, cs = std::cos (w), al = std::sin (w) / (2.0 * q);
            double a0 = 1.0 + al;
            if (kind == 0)      { b.b0 = (1 - cs) / 2; b.b1 = 1 - cs; b.b2 = (1 - cs) / 2; }
            else if (kind == 1) { b.b0 = (1 + cs) / 2; b.b1 = -(1 + cs); b.b2 = (1 + cs) / 2; }
            else                { b.b0 = al; b.b1 = 0; b.b2 = -al; }
            b.a1 = -2 * cs;
            b.a2 = 1 - al;
            b.b0 /= a0; b.b1 /= a0; b.b2 /= a0; b.a1 /= a0; b.a2 /= a0;
            return b;
        }
        float process (float x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return (float) y;
        }
    };

    Sig filtered (Sig s, int kind, double f, double q = 0.707, int passes = 1)
    {
        for (int p = 0; p < passes; ++p)
        {
            auto b = Biq::make (kind, f, q);
            for (auto& v : s) v = b.process (v);
        }
        return s;
    }

    void finish (Sig& s, float peak = 0.9f)   // a short fade-out (no click at the end), then the level
    {
        const size_t fade = juce::jmin (n (s), (size_t) (0.004 * rate));
        for (size_t i = 0; i < fade; ++i)
            s[n (s) - 1 - i] *= (float) i / (float) fade;
        float m = 0.0f;
        for (auto v : s) m = juce::jmax (m, std::abs (v));
        if (m > 1.0e-6f)
            for (auto& v : s) v *= peak / m;
    }

    Sig noise (double seconds, int seed)
    {
        juce::Random rng (seed);
        auto s = make (seconds);
        for (auto& v : s) v = rng.nextFloat() * 2.0f - 1.0f;
        return s;
    }

    void mix (Sig& dst, const Sig& src, size_t at, float gain)
    {
        for (size_t i = 0; i < src.size() && at + i < dst.size(); ++i)
            dst[at + i] += src[i] * gain;
    }

    //==========================================================================
    // Space. The drums are made mono; a few get a small room around them or a wide image (two channels), which is a large part of what makes a drum
    // sound recorded instead of synthesised.
    struct Stereo { Sig l, r; };

    // A small room: early reflections and a diffuse tail (four damped combs and two all-passes per side). `mix` is the level of the room, `decay` its
    // length in seconds, `size` scales the delays, `width` how different the two sides are.
    Stereo roomify (const Sig& dry, double mix, double decay, double size = 1.0, double width = 1.0)
    {
        const size_t len = dry.size() + (size_t) (decay * 1.3 * rate);
        auto padded = [&] { Sig s (len, 0.0f); std::copy (dry.begin(), dry.end(), s.begin()); return s; };
        const auto in = padded();

        auto early = [&] (const double* taps)
        {
            static const float gains[4] = { 0.5f, 0.38f, 0.28f, 0.2f };
            Sig out (len, 0.0f);
            for (int k = 0; k < 4; ++k)
            {
                const size_t d = (size_t) (taps[k] * size * rate);
                for (size_t i = 0; i + d < len; ++i)
                    out[i + d] += in[i] * gains[k];
            }
            return filtered (out, 0, 5500.0);
        };
        auto tail = [&] (const int* delays)
        {
            Sig out (len, 0.0f);
            for (int c = 0; c < 4; ++c)
            {
                const size_t d = (size_t) (delays[c] * size);
                const float g = (float) std::exp (-6.9 * (double) d / (decay * rate));
                std::vector<float> buf (d, 0.0f);
                size_t p = 0;
                float lp = 0.0f;
                for (size_t i = 0; i < len; ++i)
                {
                    const float y = buf[p];
                    lp += 0.35f * (y - lp);   // (the highs die first)
                    buf[p] = in[i] + g * lp;
                    out[i] += y * 0.25f;
                    p = p + 1 == d ? 0 : p + 1;
                }
            }
            for (int a = 0; a < 2; ++a)   // diffusion
            {
                const size_t d = (size_t) ((a == 0 ? 225 : 341) * size);
                std::vector<float> buf (d, 0.0f);
                size_t p = 0;
                for (size_t i = 0; i < len; ++i)
                {
                    const float x = out[i], bufOut = buf[p];
                    const float y = -0.5f * x + bufOut;
                    buf[p] = x + 0.5f * y;
                    out[i] = y;
                    p = p + 1 == d ? 0 : p + 1;
                }
            }
            return out;
        };

        static const double tapsL[4] = { 0.007, 0.013, 0.023, 0.037 }, tapsR[4] = { 0.009, 0.017, 0.029, 0.041 };
        static const int combsL[4] = { 1117, 1277, 1423, 1583 }, combsR[4] = { 1171, 1307, 1481, 1613 };
        const auto eL = early (tapsL), eR = early (tapsR);
        const auto tL = tail (combsL), tR = tail (combsR);
        Stereo out { Sig (len, 0.0f), Sig (len, 0.0f) };
        for (size_t i = 0; i < len; ++i)
        {
            const float roomL = eL[i] + tL[i] * 1.4f, roomR = eR[i] + tR[i] * 1.4f;
            out.l[i] = in[i] + (float) mix * (roomL * (float) (0.5 + 0.5 * width) + roomR * (float) (0.5 - 0.5 * width));
            out.r[i] = in[i] + (float) mix * (roomR * (float) (0.5 + 0.5 * width) + roomL * (float) (0.5 - 0.5 * width));
        }
        return out;
    }

    // A wide image from a mono sound: the right side is a hair later and phase-shifted (Haas), the left stays.
    Stereo widen (const Sig& dry, double amount)
    {
        const size_t d = (size_t) (0.0007 * rate);
        Stereo out { dry, Sig (dry.size(), 0.0f) };
        float x1 = 0.0f, y1 = 0.0f;
        for (size_t i = 0; i < dry.size(); ++i)
        {
            const float x = i >= d ? dry[i - d] : 0.0f;
            const float a = 0.55f;   // a first-order all-pass
            const float y = -a * x + x1 + a * y1;
            x1 = x; y1 = y;
            out.r[i] = (float) ((1.0 - amount) * dry[i] + amount * y);
        }
        return out;
    }

    void finishStereo (Stereo& s, float peak = 0.9f)
    {
        const size_t fade = juce::jmin (s.l.size(), (size_t) (0.004 * rate));
        for (size_t i = 0; i < fade; ++i)
        {
            const float f = (float) i / (float) fade;
            s.l[s.l.size() - 1 - i] *= f;
            s.r[s.r.size() - 1 - i] *= f;
        }
        float m = 0.0f;
        for (size_t i = 0; i < s.l.size(); ++i) m = juce::jmax (m, std::abs (s.l[i]), std::abs (s.r[i]));
        if (m > 1.0e-6f)
            for (size_t i = 0; i < s.l.size(); ++i) { s.l[i] *= peak / m; s.r[i] *= peak / m; }
    }

    //==========================================================================
    // drums
    // `soft` 0..1: a gentler hit (less click and drive, a slower pitch drop); `seed` and `detune` make a variation of the same drum for round-robin
    Sig kick (double f0, double f1, double sweepRate, double decay, double len, double drive, double click, double soft = 0.0, int seed = 5, double detune = 1.0)
    {
        f0 *= detune;
        f1 *= detune;
        const double hit = 1.0 - 0.55 * soft;
        auto s = make (len);
        double phase = 0.0, lp = 0.0;
        juce::Random rng (seed);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            phase += twoPi * (f1 + (f0 - f1) * std::exp (-t * sweepRate * (1.0 - 0.3 * soft))) / rate;
            // a punch that dies fast and a tail that carries the weight
            const double env = 0.72 * std::exp (-t * decay * 2.4) + 0.28 * std::exp (-t * decay * 0.8);
            double v = std::tanh (drive * hit * std::sin (phase)) * env;

            // the beater: a short burst of noise with the lows taken out, and a high ping
            const double nz = rng.nextFloat() * 2.0 - 1.0;
            lp += 0.3 * (nz - lp);
            v += click * hit * 1.3 * (nz - lp) * std::exp (-t * 750.0);
            v += click * hit * 0.45 * std::sin (twoPi * 1900.0 * t) * std::exp (-t * 950.0);
            s[i] = (float) v;
        }
        s = filtered (s, 1, 22.0, 0.7);   // (no rumble below the kick)
        finish (s);
        return s;
    }

    Sig snare (double bodyHz, double bodyDecay, double noiseDecay, double len, double noiseHp, double noiseMix, int crushBits = 0, double soft = 0.0, int seed = 11, double detune = 1.0)
    {
        bodyHz *= detune;
        auto s = make (len);
        auto nz = filtered (filtered (noise (len, seed), 1, noiseHp, 0.7), 0, 10000.0);   // the snare wires: noise without the lows and the very highs
        auto click = noise (0.01, seed + 7);
        double phase = 0.0, phase2 = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            const double pitch = bodyHz * (1.0 + 0.5 * std::exp (-t * 60.0));
            phase += twoPi * pitch / rate;
            phase2 += twoPi * pitch * 1.58 / rate;   // (a second mode of the shell)
            const double body = (std::sin (phase) * std::exp (-t * bodyDecay) * 0.8 + std::sin (phase2) * std::exp (-t * bodyDecay * 1.6) * 0.28) * (1.0 + 0.2 * soft);
            const double wires = nz[i] * (0.75 * std::exp (-t * noiseDecay) + 0.25 * std::exp (-t * noiseDecay * 0.35)) * noiseMix * (1.0 - 0.5 * soft);
            const double stick = i < click.size() ? click[i] * std::exp (-t * 1800.0) * 0.55 * (1.0 - soft) : 0.0;
            s[i] = (float) (body + wires + stick);
        }
        if (crushBits > 0)
        {
            const float steps = (float) (1 << crushBits);
            for (auto& v : s) v = std::round (v * steps) / steps;
            s = filtered (s, 0, 5000.0);
        }
        finish (s);
        return s;
    }

    // Several people clapping not quite together: bursts at slightly uneven distances, then the tail of the last one.
    Sig clap (int bursts, double spacing, double tailDecay, double len, double bpHz, double soft = 0.0, int seed = 21)
    {
        if (soft > 0.0)
            bursts = juce::jmax (2, bursts - 1);
        auto nz = filtered (noise (len, seed), 2, bpHz, 1.1, 2);
        auto snap = filtered (noise (len, seed + 1), 1, 2800.0, 0.7);
        auto s = make (len);
        juce::Random rng (seed + 1000);
        std::vector<double> times, levels, rates;
        double at = 0.0;
        for (int b = 0; b < bursts - 1; ++b)
        {
            times.push_back (at);
            levels.push_back (0.7 + 0.3 * rng.nextDouble());
            rates.push_back (360.0 + 140.0 * rng.nextDouble());
            at += spacing * (0.75 + 0.5 * rng.nextDouble());
        }
        const double lastAt = at;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            double e = t >= lastAt ? std::exp (-(t - lastAt) * tailDecay) : 0.0;
            for (size_t b = 0; b < times.size(); ++b)
            {
                const double d = t - times[b];
                if (d >= 0.0 && d < (b + 1 < times.size() ? times[b + 1] - times[b] : lastAt - times[b]))
                    e = juce::jmax (e, std::exp (-d * rates[b]) * levels[b]);
            }
            s[i] = (nz[i] + snap[i] * (0.22f * (1.0f - (float) soft))) * (float) e;
        }
        finish (s);
        return s;
    }

    // Hats, cymbals and tambourines: six square oscillators at the classic inharmonic 808 frequencies plus noise, filtered. Long sounds decay in two stages
    // (a quick drop, then the shimmer); every sound starts with a tiny tick. `soft` rounds it off; `detune` and `seed` make a round-robin variation.
    Sig metallic (double seconds, double decay, double hp, double bp, int seed, double attack = 0.0, double soft = 0.0, double detune = 1.0)
    {
        static const double freqs[6] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };   // the classic 808 hat oscillators
        auto s = make (seconds);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            double v = 0.0;
            for (double f : freqs) v += std::sin (twoPi * f * 7.0 * detune * t) > 0.0 ? 1.0 : -1.0;
            s[i] = (float) (v / 6.0);
        }
        auto nz = noise (seconds, seed);
        for (size_t i = 0; i < n (s); ++i) s[i] = s[i] * 0.7f + nz[i] * 0.3f;
        s = filtered (filtered (s, 2, bp, 1.2), 1, hp, 0.7, 2);
        if (soft > 0.0)
            s = filtered (s, 0, 9000.0 - 3500.0 * soft);
        const bool longSound = seconds > 0.3;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            const double env = longSound ? 0.55 * std::exp (-t * decay * 2.2) + 0.45 * std::exp (-t * decay * 0.75) : std::exp (-t * decay);
            const double tick = 0.5 * (1.0 - soft) * std::exp (-t * 1600.0);
            s[i] *= (float) ((env + tick) * (attack > 0.0 ? juce::jmin (1.0, t / attack) : 1.0) * (1.0 - 0.3 * soft));
        }
        finish (s);
        return s;
    }

    Sig tom (double f0, double f1, double decay, double len, double soft = 0.0, int seed = 3, double detune = 1.0)
    {
        f0 *= detune;
        f1 *= detune;
        auto s = make (len);
        double phase = 0.0;
        juce::Random rng (seed);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            const double f = f1 + (f0 - f1) * std::exp (-t * 25.0);
            phase += twoPi * f / rate;
            const double body = std::sin (phase) + 0.3 * std::sin (phase * 1.62) * std::exp (-t * decay * 1.8);   // (the head's second mode)
            const double stick = 0.35 * (1.0 - soft) * (rng.nextFloat() * 2.0 - 1.0) * std::exp (-t * 400.0);
            s[i] = (float) ((body * std::exp (-t * decay)) + stick);
        }
        finish (s);
        return s;
    }

    Sig shaker (double len, double attack, double decay)
    {
        auto s = filtered (noise (len, 31), 2, 6500.0, 0.9, 2);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            s[i] *= (float) (juce::jmin (1.0, t / attack) * std::exp (-t * decay));
        }
        finish (s);
        return s;
    }

    Sig tone (double f0, double f1, double decay, double len, double harm = 0.0)   // a short pitched hit (cowbell, block, conga)
    {
        auto s = make (len);
        double phase = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            phase += twoPi * (f1 + (f0 - f1) * std::exp (-t * 40.0)) / rate;
            s[i] = (float) ((std::sin (phase) + harm * (std::sin (phase * 1.48) > 0.0 ? 1.0 : -1.0)) * std::exp (-t * decay));
        }
        finish (s);
        return s;
    }

    //==========================================================================
    // More realistic drums
    struct Mode { double ratio, amp, decayMul; };

    // A drum head: the modes of a membrane ring at their own pitches (not whole-number multiples) and decay at their own rates; the pitch starts a
    // little high and settles (the head is stretched hardest at the strike).
    Sig membrane (double f, double len, double decay, double drop, std::initializer_list<Mode> modes, double seed = 0.0)
    {
        auto s = make (len);
        std::vector<double> phase (modes.size(), seed);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            const double bend = 1.0 + drop * std::exp (-t * 45.0);
            double v = 0.0;
            size_t k = 0;
            for (auto& m : modes)
            {
                phase[k] += twoPi * f * m.ratio * bend / rate;
                v += std::sin (phase[k]) * m.amp * std::exp (-t * decay * m.decayMul);
                ++k;
            }
            s[i] = (float) v;
        }
        return s;
    }

    Sig kickAcoustic (double soft = 0.0, int seed = 8)
    {
        auto s = membrane (68.0, 0.7, 7.5, 0.35, { { 1.0, 1.0, 1.0 }, { 1.59, 0.35, 1.5 }, { 2.14, 0.18, 2.2 } });
        auto thump = filtered (noise (0.7, seed), 0, 220.0, 0.7);
        auto beater = filtered (noise (0.7, seed + 1), 2, 3200.0, 0.9);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            s[i] += (float) (thump[i] * std::exp (-t * 60.0) * 1.6 + beater[i] * std::exp (-t * 260.0) * 0.9 * (1.0 - 0.6 * soft));
        }
        s = filtered (s, 1, 28.0, 0.7);
        finish (s);
        return s;
    }

    Sig kickElectro (double f0, double f1, double sweep, double decay, double len, double click, double soft = 0.0, int seed = 5)
    {
        auto s = kick (f0, f1, sweep, decay, len, 2.4, click, soft, seed);
        return s;
    }

    Sig snareAcoustic (double soft = 0.0, int seed = 31, double detune = 1.0)
    {
        const double f = 185.0 * detune;
        auto body = membrane (f, 0.5, 20.0, 0.3, { { 1.0, 1.0, 1.0 }, { 1.58, 0.5, 1.2 }, { 2.14, 0.32, 1.6 }, { 2.65, 0.18, 2.0 } });
        auto wires = filtered (filtered (noise (0.5, seed), 1, 2200.0, 0.7), 0, 11000.0);
        auto stick = noise (0.01, seed + 5);
        auto s = make (0.5);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            const double buzz = 0.82 + 0.18 * std::sin (twoPi * 120.0 * t);   // the wires rattle against the head
            const double w = wires[i] * (0.7 * std::exp (-t * 15.0) + 0.3 * std::exp (-t * 5.5)) * buzz * (1.0 - 0.5 * soft);
            const double st = i < stick.size() ? stick[i] * std::exp (-t * 2000.0) * 0.5 * (1.0 - soft) : 0.0;
            s[i] = (float) (body[i] * 0.55 + w * 0.9 + st);
        }
        finish (s);
        return s;
    }

    Sig snareElectro (double soft = 0.0, int seed = 41)
    {
        auto s = make (0.32);
        auto nz = filtered (filtered (noise (0.32, seed), 1, 1600.0, 0.7), 0, 9000.0);
        double p1 = 0.0, p2 = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            const double bend = 1.0 + 0.35 * std::exp (-t * 70.0);
            p1 += twoPi * 180.0 * bend / rate;
            p2 += twoPi * 330.0 * bend / rate;
            const double tones = (std::sin (p1) + 0.55 * std::sin (p2)) * std::exp (-t * 26.0);
            s[i] = (float) (tones * 0.75 + nz[i] * std::exp (-t * 15.0) * (0.9 - 0.4 * soft));
        }
        finish (s);
        return s;
    }

    Sig snareBrush()
    {
        auto nz = filtered (filtered (noise (0.4, 55), 2, 3800.0, 0.5), 1, 1500.0, 0.7);
        auto s = make (0.4);
        double phase = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            phase += twoPi * (150.0 + 40.0 * std::exp (-t * 40.0)) / rate;
            s[i] = (float) (nz[i] * juce::jmin (1.0, t / 0.012) * std::exp (-t * 8.5) + std::sin (phase) * std::exp (-t * 30.0) * 0.35);
        }
        finish (s);
        return s;
    }

    // Cymbals: a dozen inharmonic partials plus a wash of noise, a quick drop and a long shimmer. `bell` adds the clear ping of a ride.
    Sig cymbal (double seconds, double decay, double lo, double hi, int seed, double bell = 0.0)
    {
        juce::Random rng (seed);
        auto s = make (seconds);
        struct P { double f, a, d, ph; };
        std::vector<P> parts;
        for (int k = 0; k < 14; ++k)
            parts.push_back ({ lo * std::pow (hi / lo, rng.nextDouble()), 0.4 + 0.6 * rng.nextDouble(), 0.6 + 1.0 * rng.nextDouble(), rng.nextDouble() * twoPi });
        auto wash = filtered (filtered (noise (seconds, seed + 1), 2, std::sqrt (lo * hi), 0.45), 1, lo, 0.7);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            double v = 0.0;
            for (auto& p : parts)
                v += p.a * std::sin (twoPi * p.f * t + p.ph + 2.0 * std::sin (twoPi * p.f * 0.37 * t)) * std::exp (-t * decay * p.d);   // (a little frequency wobble: the metal flexes)
            const double env = 0.55 * std::exp (-t * decay * 2.0) + 0.45 * std::exp (-t * decay * 0.7);
            double b = 0.0;
            if (bell > 0.0)
                b = bell * (std::sin (twoPi * 1180.0 * t) + 0.6 * std::sin (twoPi * 2570.0 * t) + 0.4 * std::sin (twoPi * 4130.0 * t)) * std::exp (-t * 4.5);
            s[i] = (float) ((v / 14.0 * 3.0 + wash[i] * 1.2) * env * juce::jmin (1.0, t / 0.002) + b);
        }
        finish (s);
        return s;
    }

    Sig triangle()
    {
        auto s = make (1.8);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            s[i] = (float) ((std::sin (twoPi * 4280.0 * t) + 0.7 * std::sin (twoPi * 6830.0 * t) + 0.5 * std::sin (twoPi * 9560.0 * t)) * std::exp (-t * 2.6) * juce::jmin (1.0, t / 0.0008));
        }
        finish (s);
        return s;
    }

    Sig snap()
    {
        auto nz = filtered (noise (0.1, 61), 2, 2400.0, 1.6, 2);
        auto s = make (0.1);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            s[i] = (float) (nz[i] * std::exp (-t * 85.0) + std::sin (twoPi * (1300.0 - 500.0 * t * 6.0) * t) * std::exp (-t * 120.0) * 0.5);
        }
        finish (s);
        return s;
    }

    Sig cabasa()
    {
        auto nz = filtered (noise (0.22, 62), 1, 6500.0, 0.8, 2);
        auto s = make (0.22);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            s[i] = nz[i] * (float) (juce::jmin (1.0, t / 0.004) * std::exp (-t * 24.0) * (0.75 + 0.25 * std::sin (twoPi * 55.0 * t)));
        }
        finish (s);
        return s;
    }

    //==========================================================================
    // tonal
    void saw (Sig& dst, size_t at, double freq, double seconds, double cutoffStart, double cutoffEnd, double attack, double release, float gain, double detune = 0.0, int voices = 1)
    {
        const size_t len = (size_t) (seconds * rate);
        for (int v = 0; v < voices; ++v)
        {
            const double f = freq * (1.0 + detune * (voices == 1 ? 0.0 : (double) v / (voices - 1) - 0.5));
            double phase = 0.0, lp = 0.0;
            for (size_t i = 0; i < len && at + i < dst.size(); ++i)
            {
                const double t = (double) i / rate;
                phase += f / rate;
                if (phase >= 1.0) phase -= 1.0;
                const double cutoff = cutoffEnd + (cutoffStart - cutoffEnd) * std::exp (-t * 6.0);
                const double a = 1.0 - std::exp (-twoPi * cutoff / rate);
                lp += a * ((phase * 2.0 - 1.0) - lp);
                const double env = juce::jmin (1.0, t / juce::jmax (attack, 0.0005)) * (t > seconds - release ? juce::jmax (0.0, (seconds - t) / release) : 1.0);
                dst[at + i] += (float) (lp * env * gain / voices);
            }
        }
    }

    double midiHz (int note) { return 440.0 * std::pow (2.0, (note - 69) / 12.0); }

    Sig bass808 (double freq, double len, double decay)
    {
        auto s = make (len);
        double phase = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            phase += twoPi * (freq * (1.0 + 0.6 * std::exp (-t * 30.0))) / rate;
            s[i] = (float) (std::tanh (1.6 * std::sin (phase)) * std::exp (-t * decay));
        }
        finish (s);
        return s;
    }

    Sig chordStab (std::initializer_list<int> notes, double len)
    {
        auto s = make (len);
        for (int note : notes)
            saw (s, 0, midiHz (note), len, 5000.0, 700.0, 0.002, 0.3, 0.5f, 0.012, 3);
        finish (s);
        return s;
    }

    //==========================================================================
    // loops: steps of a bar in sixteenths
    Sig drumLoop (int bpm, const char* kickP, const char* snareP, const char* hatP, const char* openP, const Sig& k, const Sig& sn, const Sig& h, const Sig& o, double swing, int bars = 2)
    {
        const double step = 60.0 / bpm / 4.0;
        auto out = make (step * 16 * bars + 0.5);
        for (int b = 0; b < bars; ++b)
            for (int i = 0; i < 16; ++i)
            {
                const double t = (b * 16 + i) * step + ((i % 2) ? swing * step : 0.0);
                const size_t at = (size_t) (t * rate);
                if (kickP[i] != '.')  mix (out, k, at, kickP[i] == 'X' ? 1.0f : 0.7f);
                if (snareP[i] != '.') mix (out, sn, at, snareP[i] == 'X' ? 0.9f : 0.5f);
                if (hatP[i] != '.')   mix (out, h, at, hatP[i] == 'X' ? 0.55f : 0.3f);
                if (openP[i] != '.')  mix (out, o, at, 0.45f);
            }
        out.resize ((size_t) (step * 16 * bars * rate));
        finish (out, 0.9f);
        return out;
    }

    Sig bassLoop (int bpm, std::initializer_list<std::pair<int, int>> notesAndSteps, double cutoff)   // (pitch, length in sixteenths) one after the other
    {
        const double step = 60.0 / bpm / 4.0;
        double steps = 0;
        for (auto& p : notesAndSteps) steps += p.second;
        auto out = make (steps * step);
        double t = 0;
        for (auto& p : notesAndSteps)
        {
            if (p.first > 0)
                saw (out, (size_t) (t * rate), midiHz (p.first), p.second * step * 0.95, cutoff * 2.0, cutoff * 0.5, 0.003, 0.05, 0.8f, 0.01, 2);
            t += p.second * step;
        }
        finish (out, 0.9f);
        return out;
    }

    Sig riser (double len, bool up)
    {
        auto nz = noise (len, 41);
        auto s = make (len);
        double lp = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate, x = up ? t / len : 1.0 - t / len;
            const double cutoff = 300.0 + 9000.0 * x * x;
            lp += (1.0 - std::exp (-twoPi * cutoff / rate)) * (nz[i] - lp);
            s[i] = (float) (lp * (0.15 + 0.85 * x * x));
        }
        finish (s);
        return s;
    }

    Sig impact()
    {
        auto s = make (3.0);
        auto nz = filtered (noise (3.0, 51), 0, 1800.0);
        double phase = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            phase += twoPi * (38.0 + 60.0 * std::exp (-t * 12.0)) / rate;
            s[i] = (float) (std::sin (phase) * std::exp (-t * 1.6) + nz[i] * std::exp (-t * 6.0) * 0.5);
        }
        finish (s);
        return s;
    }

    Sig vinyl()
    {
        auto s = make (4.0);
        juce::Random rng (61);
        double lp = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            lp += 0.2 * ((rng.nextFloat() * 2.0 - 1.0) - lp);
            double v = lp * 0.04;
            if (rng.nextInt (6000) == 0)
                v += (rng.nextFloat() * 2.0 - 1.0) * 0.8;   // a crackle
            s[i] = (float) v;
        }
        s = filtered (s, 1, 600.0);
        finish (s, 0.6f);
        return s;
    }

    //==========================================================================
    // more sounds (pack version 4): a few more building blocks

    Sig reversed (Sig s)
    {
        std::reverse (s.begin(), s.end());
        finish (s);
        return s;
    }

    Sig squareLead (double freq, double len, double cutoff, double attack = 0.01)
    {
        auto s = make (len);
        double phase = 0.0, lp = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            phase += freq / rate;
            if (phase >= 1.0) phase -= 1.0;
            lp += (1.0 - std::exp (-twoPi * cutoff / rate)) * ((phase < 0.5 ? 1.0 : -1.0) - lp);
            const double env = juce::jmin (1.0, t / attack) * juce::jmin (1.0, (len - t) / 0.15);
            s[i] = (float) (lp * env * 0.7);
        }
        finish (s, 0.4f);   // (a square wave is loud: it is kept in line with the other sounds)
        return s;
    }

    Sig bell (double freq, double len)   // inharmonic partials that ring and fade at different speeds
    {
        auto s = make (len);
        const double ratio[] = { 1.0, 2.76, 5.4, 8.93 }, amp[] = { 1.0, 0.55, 0.3, 0.15 }, fade[] = { 1.4, 2.6, 4.2, 6.5 };
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            double v = 0.0;
            for (int k = 0; k < 4; ++k)
                v += std::sin (twoPi * freq * ratio[k] * t) * amp[k] * std::exp (-t * fade[k] * 2.0 / len * 2.5);
            s[i] = (float) (v * juce::jmin (1.0, t / 0.002));
        }
        finish (s);
        return s;
    }

    Sig epiano (double freq, double len)   // a soft electric piano: two-operator FM, the bite fades quickly
    {
        auto s = make (len);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            const double mod = std::sin (twoPi * freq * 14.0 * t) * std::exp (-t * 9.0) * 1.3 + std::sin (twoPi * freq * t) * 0.25;
            const double v = std::sin (twoPi * freq * t + mod) * std::exp (-t * 2.4) + 0.2 * std::sin (twoPi * freq * 2.0 * t) * std::exp (-t * 5.0);
            s[i] = (float) (v * juce::jmin (1.0, t / 0.003) * juce::jmin (1.0, (len - t) / 0.1));
        }
        finish (s);
        return s;
    }

    Sig marimba (double freq, double len)
    {
        auto s = make (len);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            const double v = std::sin (twoPi * freq * t) * std::exp (-t * 5.5) + 0.45 * std::sin (twoPi * freq * 3.93 * t) * std::exp (-t * 26.0)
                             + 0.05 * std::sin (twoPi * freq * 9.9 * t) * std::exp (-t * 60.0);
            s[i] = (float) (v * juce::jmin (1.0, t / 0.001));
        }
        finish (s);
        return s;
    }

    Sig subBass (double freq, double len, double decay)   // a pure sine with a short pitch drop, no distortion
    {
        auto s = make (len);
        double phase = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            phase += twoPi * freq * (1.0 + 0.25 * std::exp (-t * 40.0)) / rate;
            s[i] = (float) (std::sin (phase) * std::exp (-t * decay) * juce::jmin (1.0, t / 0.004));
        }
        finish (s);
        return s;
    }

    Sig bass808Drive (double freq, double len, double decay, double drive)
    {
        auto s = make (len);
        double phase = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            phase += twoPi * (freq * (1.0 + 0.6 * std::exp (-t * 30.0))) / rate;
            s[i] = (float) (std::tanh (drive * std::sin (phase)) * std::exp (-t * decay));
        }
        finish (s);
        return s;
    }

    Sig reese (double freq, double len)   // detuned saws that beat against each other
    {
        auto s = make (len);
        saw (s, 0, freq, len, 700.0, 350.0, 0.01, 0.15, 0.8f, 0.045, 3);
        finish (s);
        return s;
    }

    Sig laser (double len, bool down)
    {
        auto s = make (len);
        double phase = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate, x = t / len;
            const double f = down ? 3500.0 * std::exp (-x * 4.0) + 80.0 : 120.0 + 3400.0 * x * x;
            phase += twoPi * f / rate;
            s[i] = (float) (std::sin (phase) * (down ? std::exp (-x * 3.0) : std::sin (x * 3.14159)) * 0.8);
        }
        finish (s, 0.5f);
        return s;
    }

    Sig whoosh (double len)   // noise through a band that rises and falls
    {
        auto nz = noise (len, 71);
        auto s = make (len);
        Biq f = Biq::make (2, 1000.0, 1.2);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double x = (double) i / rate / len;
            const double centre = 400.0 + 4200.0 * std::sin (x * 3.14159);
            if ((i & 31) == 0)
            {
                const auto z1 = f.z1, z2 = f.z2;
                f = Biq::make (2, centre, 1.2);
                f.z1 = z1;
                f.z2 = z2;
            }
            s[i] = (float) (f.process (nz[i]) * std::sin (x * 3.14159) * 3.0);
        }
        finish (s);
        return s;
    }

    Sig noiseBurst (double len, double decay, double hp)
    {
        auto s = filtered (noise (len, 81), 1, hp);
        for (size_t i = 0; i < n (s); ++i)
            s[i] *= (float) std::exp (-(double) i / rate * decay);
        finish (s);
        return s;
    }

    Sig chordLoop (int bpm, std::initializer_list<std::initializer_list<int>> chords, int stepsPerChord, double cutoff, double attack, double gate = 0.95)
    {
        const double step = 60.0 / bpm / 4.0, each = step * stepsPerChord;
        auto out = make (each * (double) chords.size());
        size_t index = 0;
        for (auto& chord : chords)
        {
            for (int note : chord)
                saw (out, (size_t) (each * (double) index * rate), midiHz (note), each * gate, cutoff * 1.6, cutoff * 0.7, attack, 0.12, 0.4f, 0.012, 3);
            ++index;
        }
        finish (out, 0.9f);
        return out;
    }

    //==========================================================================
    // even more sounds (pack version 5): plucked strings, organs, voices, vintage drum machines, world percussion, textures

    Sig pluckString (double freq, double len, double damp, double bright, int seed)   // Karplus-Strong: a burst of noise ringing in a short delay line
    {
        auto s = make (len);
        const int N = juce::jmax (2, (int) (rate / freq + 0.5));
        std::vector<float> line ((size_t) N);
        juce::Random rng (seed);
        float smooth = 0.0f;
        for (auto& v : line)
        {
            smooth += (float) bright * ((rng.nextFloat() * 2.0f - 1.0f) - smooth);
            v = smooth;
        }
        size_t idx = 0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const float cur = line[idx], next = line[(idx + 1) % (size_t) N];
            line[idx] = (float) damp * 0.5f * (cur + next);
            s[i] = cur;
            idx = (idx + 1) % (size_t) N;
        }
        finish (s);
        return s;
    }

    Sig organ (double freq, double len, std::initializer_list<double> drawbars)   // harmonics 1, 2, 3, 4, 6, 8 with these levels
    {
        auto s = make (len);
        const double harmonic[] = { 1.0, 2.0, 3.0, 4.0, 6.0, 8.0 };
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            double v = 0.0;
            int k = 0;
            for (double level : drawbars)
                v += level * std::sin (twoPi * freq * harmonic[k++] * t);
            s[i] = (float) (v * juce::jmin (1.0, t / 0.006) * juce::jmin (1.0, (len - t) / 0.08));
        }
        finish (s, 0.5f);
        return s;
    }

    Sig vox (double freq, double len, double f1, double f2, double f3, double attack = 0.12)   // a voice: a saw wave through three formant bands
    {
        auto src = make (len);
        saw (src, 0, freq, len, 6000.0, 6000.0, attack, 0.2, 0.9f, 0.01, 3);
        auto s = make (len);
        Biq b1 = Biq::make (2, f1, 8.0), b2 = Biq::make (2, f2, 10.0), b3 = Biq::make (2, f3, 12.0);
        for (size_t i = 0; i < n (s); ++i)
            s[i] = b1.process (src[i]) * 1.0f + b2.process (src[i]) * 0.6f + b3.process (src[i]) * 0.3f;
        finish (s);
        return s;
    }

    Sig clav (double freq, double len)
    {
        auto s = make (len);
        double phase = 0.0, lp = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            phase += freq / rate;
            if (phase >= 1.0) phase -= 1.0;
            const double cutoff = 600.0 + 7000.0 * std::exp (-t * 14.0);
            lp += (1.0 - std::exp (-twoPi * cutoff / rate)) * ((phase < 0.22 ? 1.0 : -1.0) - lp);
            s[i] = (float) (lp * std::exp (-t * 6.0) * juce::jmin (1.0, t / 0.001));
        }
        finish (s);
        return s;
    }

    Sig kalimba (double freq, double len)
    {
        auto s = make (len);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            const double v = std::sin (twoPi * freq * t) * std::exp (-t * 3.2) + 0.35 * std::sin (twoPi * freq * 5.4 * t) * std::exp (-t * 22.0);
            s[i] = (float) (v * juce::jmin (1.0, t / 0.001));
        }
        finish (s);
        return s;
    }

    Sig fmBass (double freq, double len, double index)
    {
        auto s = make (len);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            const double mod = std::sin (twoPi * freq * 2.0 * t) * index * std::exp (-t * 5.0);
            s[i] = (float) (std::sin (twoPi * freq * t + mod) * std::exp (-t * 1.6) * juce::jmin (1.0, t / 0.002));
        }
        finish (s);
        return s;
    }

    Sig wobble (double freq, double len, double lfoHz, double depth)
    {
        auto s = make (len);
        double phase = 0.0, lp = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            phase += freq / rate;
            if (phase >= 1.0) phase -= 1.0;
            const double cutoff = 250.0 + depth * (0.5 + 0.5 * std::sin (twoPi * lfoHz * t));
            lp += (1.0 - std::exp (-twoPi * cutoff / rate)) * ((phase * 2.0 - 1.0) - lp);
            s[i] = (float) (lp * juce::jmin (1.0, t / 0.01) * juce::jmin (1.0, (len - t) / 0.1));
        }
        finish (s, 0.6f);
        return s;
    }

    Sig brassStab (std::initializer_list<int> notes, double len)
    {
        auto s = make (len);
        for (int note : notes)
        {
            saw (s, 0, midiHz (note), len, 4200.0, 900.0, 0.02, 0.15, 0.4f, 0.006, 3);
            saw (s, 0, midiHz (note) * 2.0, len, 2500.0, 700.0, 0.03, 0.15, 0.15f, 0.004, 2);
        }
        finish (s);
        return s;
    }

    Sig stringsPad (std::initializer_list<int> notes, double len)
    {
        auto s = make (len);
        for (int note : notes)
            saw (s, 0, midiHz (note), len, 3200.0, 2400.0, 0.5, 0.8, 0.35f, 0.02, 6);
        finish (s);
        return s;
    }

    Sig hat808 (double len, double decay, double hp)   // six square waves, as in the old drum machines
    {
        auto s = make (len);
        const double f[] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            double v = 0.0;
            for (double fk : f)
                v += std::fmod (t * fk, 1.0) < 0.5 ? 1.0 : -1.0;
            s[i] = (float) (v * std::exp (-t * decay) * juce::jmin (1.0, t / 0.0005));
        }
        s = filtered (s, 1, hp, 0.9, 2);
        finish (s);
        return s;
    }

    Sig cowbell808()
    {
        auto s = make (0.6);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            const double v = (std::fmod (t * 540.0, 1.0) < 0.5 ? 1.0 : -1.0) + (std::fmod (t * 800.0, 1.0) < 0.5 ? 1.0 : -1.0);
            s[i] = (float) (v * (0.6 * std::exp (-t * 60.0) + 0.4 * std::exp (-t * 9.0)));
        }
        s = filtered (s, 2, 700.0, 1.0, 1);
        finish (s);
        return s;
    }

    Sig castanet()
    {
        auto s = filtered (noise (0.12, 91), 2, 2600.0, 3.0);
        for (size_t i = 0; i < n (s); ++i)
            s[i] *= (float) (std::exp (-(double) i / rate * 70.0) * (1.0 + 0.7 * std::sin ((double) i / rate * 3000.0)));
        finish (s);
        return s;
    }

    Sig guiro()
    {
        auto s = make (0.35);
        auto nz = filtered (noise (0.35, 92), 2, 3800.0, 2.0);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            const double scrape = 0.5 + 0.5 * std::sin (twoPi * 55.0 * t);   // the ridges
            s[i] = (float) (nz[i] * scrape * std::exp (-t * 7.0) * juce::jmin (1.0, t / 0.01));
        }
        finish (s);
        return s;
    }

    Sig glitch (double len)   // short bursts of noise and tone, cut up
    {
        auto s = make (len);
        juce::Random rng (93);
        size_t at = 0;
        while (at < n (s))
        {
            const size_t piece = (size_t) (rate * (0.01 + 0.05 * rng.nextDouble()));
            const bool tonal = rng.nextBool();
            const double f = 200.0 + rng.nextDouble() * 3000.0;
            for (size_t i = 0; i < piece && at + i < n (s); ++i)
            {
                const double t = (double) i / rate;
                s[at + i] = tonal ? (float) std::sin (twoPi * f * t) : (rng.nextFloat() * 2.0f - 1.0f);
                s[at + i] *= (float) (1.0 - (double) i / (double) piece);
            }
            at += piece + (size_t) (rate * 0.02 * rng.nextDouble());
        }
        finish (s);
        return s;
    }

    Sig tapeStop (double len)   // a chord slowing down until it stops
    {
        auto s = make (len);
        double p1 = 0.0, p2 = 0.0, p3 = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate, speed = std::pow (juce::jmax (0.0, 1.0 - t / len), 2.0);
            p1 += twoPi * 220.0 * speed / rate;
            p2 += twoPi * 277.2 * speed / rate;
            p3 += twoPi * 329.6 * speed / rate;
            s[i] = (float) ((std::sin (p1) + std::sin (p2) + std::sin (p3)) * 0.3 * speed);
        }
        finish (s);
        return s;
    }

    Sig windNoise (double len)
    {
        auto nz = noise (len, 94);
        auto s = make (len);
        double lp = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            const double cutoff = 300.0 + 700.0 * (0.5 + 0.5 * std::sin (twoPi * 0.31 * t)) + 400.0 * (0.5 + 0.5 * std::sin (twoPi * 0.77 * t + 1.0));
            lp += (1.0 - std::exp (-twoPi * cutoff / rate)) * (nz[i] - lp);
            s[i] = (float) (lp * (0.6 + 0.4 * std::sin (twoPi * 0.19 * t)));
        }
        finish (s, 0.5f);
        return s;
    }

    Sig rainNoise (double len)
    {
        auto s = filtered (noise (len, 95), 1, 3500.0);
        juce::Random rng (96);
        for (size_t i = 0; i < n (s); ++i)
            if (rng.nextInt (900) == 0)
                for (size_t k = 0; k < 90 && i + k < n (s); ++k)
                    s[i + k] += (rng.nextFloat() * 2.0f - 1.0f) * 0.5f * (float) std::exp (-(double) k / 22.0);
        finish (s, 0.5f);
        return s;
    }

    Sig tapeHiss (double len)
    {
        auto s = filtered (noise (len, 97), 1, 2500.0);
        finish (s, 0.25f);
        return s;
    }

    Sig drone (double freq, double len)
    {
        auto s = make (len);
        saw (s, 0, freq, len, 700.0, 700.0, 2.0, 2.0, 0.4f, 0.03, 5);
        saw (s, 0, freq * 1.5, len, 600.0, 600.0, 2.5, 2.0, 0.25f, 0.03, 4);
        saw (s, 0, freq * 0.5, len, 400.0, 400.0, 1.5, 2.0, 0.3f, 0.02, 3);
        finish (s, 0.6f);
        return s;
    }

    Sig sweepNoise (double len, bool up)
    {
        auto nz = noise (len, 98);
        auto s = make (len);
        Biq f = Biq::make (2, 500.0, 2.0);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double x = (double) i / rate / len;
            const double centre = up ? 300.0 * std::pow (30.0, x) : 9000.0 * std::pow (0.04, x);
            if ((i & 31) == 0)
            {
                const auto z1 = f.z1, z2 = f.z2;
                f = Biq::make (2, centre, 2.0);
                f.z1 = z1;
                f.z2 = z2;
            }
            s[i] = (float) (f.process (nz[i]) * 4.0 * juce::jmin (1.0, x * 20.0) * juce::jmin (1.0, (1.0 - x) * 10.0));
        }
        finish (s);
        return s;
    }

    // an arpeggio: the notes one after the other on sixteenths, each a short pluck
    Sig arpLoop (int bpm, std::vector<int> notes, double cutoff, int bars = 2, double gate = 1.6)
    {
        const double step = 60.0 / bpm / 4.0;
        const size_t total = (size_t) (step * 16.0 * bars * rate);
        auto out = make (step * 16.0 * bars + 1.0);
        for (int i = 0; i < 16 * bars; ++i)
            saw (out, (size_t) ((double) i * step * rate), midiHz (notes[(size_t) i % notes.size()]), step * gate, cutoff, cutoff * 0.25, 0.002, 0.08, 0.6f, 0.008, 2);
        out.resize (total);
        finish (out, 0.9f);
        return out;
    }

    // a melody: (pitch, length in sixteenths) one after the other, pitch 0 is a rest
    Sig melodyLoop (int bpm, std::initializer_list<std::pair<int, int>> notesAndSteps, double cutoff)
    {
        const double step = 60.0 / bpm / 4.0;
        double steps = 0;
        for (auto& p : notesAndSteps) steps += p.second;
        auto out = make (steps * step + 0.5);
        double t = 0;
        for (auto& p : notesAndSteps)
        {
            if (p.first > 0)
                saw (out, (size_t) (t * rate), midiHz (p.first), p.second * step * 0.9, cutoff, cutoff * 0.3, 0.004, 0.08, 0.6f, 0.01, 3);
            t += p.second * step;
        }
        out.resize ((size_t) (steps * step * rate));
        finish (out, 0.9f);
        return out;
    }

    //==========================================================================
    // pack version 6: families of sounds made in loops (every key, several tempos)

    const char* const noteNames[12] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };

    Sig bassLoopV (int bpm, const std::vector<std::pair<int, int>>& notesAndSteps, double cutoff)
    {
        const double step = 60.0 / bpm / 4.0;
        double steps = 0;
        for (auto& p : notesAndSteps) steps += p.second;
        auto out = make (steps * step);
        double t = 0;
        for (auto& p : notesAndSteps)
        {
            if (p.first > 0)
                saw (out, (size_t) (t * rate), midiHz (p.first), p.second * step * 0.95, cutoff * 2.0, cutoff * 0.5, 0.003, 0.05, 0.8f, 0.01, 2);
            t += p.second * step;
        }
        finish (out, 0.9f);
        return out;
    }

    Sig chordLoopV (int bpm, const std::vector<std::vector<int>>& chords, int stepsPerChord, double cutoff, double attack)
    {
        const double step = 60.0 / bpm / 4.0, each = step * stepsPerChord;
        auto out = make (each * (double) chords.size());
        for (size_t index = 0; index < chords.size(); ++index)
            for (int note : chords[index])
                saw (out, (size_t) (each * (double) index * rate), midiHz (note), each * 0.95, cutoff * 1.6, cutoff * 0.7, attack, 0.12, 0.4f, 0.012, 3);
        finish (out, 0.9f);
        return out;
    }

    Sig chordShot (int root, int kind, double len)   // 0 minor 7, 1 major 7, 2 dominant 7
    {
        static const int shapes[3][4] = { { 0, 3, 7, 10 }, { 0, 4, 7, 11 }, { 0, 4, 7, 10 } };
        auto s = make (len);
        for (int k = 0; k < 4; ++k)
            saw (s, 0, midiHz (root + shapes[kind][k]), len, 5000.0, 700.0, 0.002, 0.3, 0.5f, 0.012, 3);
        finish (s);
        return s;
    }

    Sig superSaw (double freq, double len)
    {
        auto s = make (len);
        saw (s, 0, freq, len, 7000.0, 4000.0, 0.01, 0.3, 0.8f, 0.045, 7);
        finish (s);
        return s;
    }

    Sig sineLead (double freq, double len)
    {
        auto s = make (len);
        double phase = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            phase += twoPi * freq * (1.0 + 0.004 * std::sin (twoPi * 5.5 * t) * juce::jmin (1.0, t / 0.4)) / rate;
            s[i] = (float) ((std::sin (phase) + 0.15 * std::sin (2.0 * phase)) * juce::jmin (1.0, t / 0.01) * juce::jmin (1.0, (len - t) / 0.2));
        }
        finish (s, 0.45f);
        return s;
    }

    Sig subDrop (double len)
    {
        auto s = make (len);
        double phase = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            phase += twoPi * (28.0 + 110.0 * std::exp (-t * 3.0)) / rate;
            s[i] = (float) (std::sin (phase) * std::exp (-t * 1.1) * juce::jmin (1.0, t / 0.003));
        }
        finish (s);
        return s;
    }

    Sig snareRoll (double len)   // snares that speed up and get louder
    {
        auto sn = snare (200, 20, 15, 0.2, 1500, 1.0);
        auto s = make (len);
        double t = 0.0, gap = 0.22;
        while (t < len - 0.1)
        {
            mix (s, sn, (size_t) (t * rate), (float) (0.25 + 0.75 * t / len));
            t += gap;
            gap = juce::jmax (0.03, gap * 0.9);
        }
        finish (s);
        return s;
    }

    Sig siren (double len, double lo, double hi, double rateHz)
    {
        auto s = make (len);
        double phase = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            const double f = lo + (hi - lo) * (0.5 + 0.5 * std::sin (twoPi * rateHz * t));
            phase += twoPi * f / rate;
            const double sq = std::sin (phase) > 0 ? 1.0 : -1.0;
            s[i] = (float) (0.6 * std::sin (phase) + 0.2 * sq) * (float) (juce::jmin (1.0, t / 0.02) * juce::jmin (1.0, (len - t) / 0.1));
        }
        finish (s, 0.5f);
        return s;
    }

    Sig colouredNoise (double len, double lowpassHz, int seed)   // pink-ish (a low cutoff) or white-ish (a high one) noise bed
    {
        auto s = filtered (noise (len, seed), 0, lowpassHz);
        finish (s, 0.5f);
        return s;
    }

    Sig fillBar (int bpm, const char* snareP, const char* tomP, const Sig& sn, const Sig& tm)
    {
        const double step = 60.0 / bpm / 4.0;
        auto out = make (step * 16 + 0.5);
        for (int i = 0; i < 16; ++i)
        {
            const size_t at = (size_t) ((double) i * step * rate);
            if (snareP[i] != '.')  mix (out, sn, at, snareP[i] == 'X' ? 0.9f : 0.5f);
            if (tomP[i] != '.')    mix (out, tm, at, tomP[i] == 'X' ? 0.9f : 0.6f);
        }
        out.resize ((size_t) (step * 16 * rate));
        finish (out, 0.9f);
        return out;
    }

    struct Groove { const char* kick; const char* snare; const char* hat; const char* open; double swing; };

    //==========================================================================
    struct Entry { const char* category; juce::String name; std::function<Sig()> make; double room = 0.0, roomDecay = 0.3, wide = 0.0; };   // (room / wide: a stereo sound)

    const std::vector<Entry>& catalog()
    {
        static const std::vector<Entry> list = []
        {
            std::vector<Entry> e;
            auto add = [&] (const char* c, const juce::String& name, std::function<Sig()> f, double room = 0.0, double roomDecay = 0.3, double wide = 0.0) { e.push_back ({ c, name, std::move (f), room, roomDecay, wide }); };

            add ("Kicks", "Kick Punch",  [] { return kick (150, 48, 28, 7, 0.5, 1.5, 0.15); });
            add ("Kicks", "Kick Deep",   [] { return kick (110, 38, 18, 4.5, 0.9, 1.2, 0.05); });
            add ("Kicks", "Kick 808",    [] { return kick (90, 45, 12, 2.2, 1.6, 1.4, 0.03); });
            add ("Kicks", "Kick Soft",   [] { return kick (120, 55, 22, 9, 0.4, 0.8, 0.02); });
            add ("Kicks", "Kick Click",  [] { return kick (220, 60, 60, 10, 0.35, 1.3, 0.4); });

            add ("Snares", "Snare Tight", [] { return snare (200, 28, 20, 0.3, 1800, 0.9); });
            add ("Snares", "Snare Fat",   [] { return snare (160, 14, 10, 0.45, 1200, 1.0); });
            add ("Snares", "Snare Snap",  [] { return snare (240, 40, 26, 0.22, 2500, 1.1); });
            add ("Snares", "Snare Lofi",  [] { return snare (180, 18, 13, 0.4, 900, 0.9, 5); });
            add ("Snares", "Rim Shot",    [] { return tone (1800, 1500, 90, 0.12, 0.4); });

            add ("Claps", "Clap Tight", [] { return clap (4, 0.012, 20, 0.3, 1300); });
            add ("Claps", "Clap Wide",  [] { return clap (5, 0.016, 9, 0.6, 1100); });

            add ("Hats", "Hat Closed 1", [] { return metallic (0.12, 55, 7000, 10000, 71); });
            add ("Hats", "Hat Closed 2", [] { return metallic (0.09, 80, 8000, 11000, 72); });
            add ("Hats", "Hat Closed 3", [] { return metallic (0.15, 40, 5500, 8000, 73); });
            add ("Hats", "Hat Open 1",   [] { return metallic (0.6, 7, 6500, 9000, 74); });
            add ("Hats", "Hat Open 2",   [] { return metallic (0.9, 5, 5500, 8000, 75); });

            add ("Toms", "Tom Low",  [] { return tom (130, 70, 8, 0.7); });
            add ("Toms", "Tom Mid",  [] { return tom (180, 110, 9, 0.6); });
            add ("Toms", "Tom High", [] { return tom (250, 160, 10, 0.5); });

            add ("Cymbals", "Crash", [] { return cymbal (2.6, 1.5, 2200, 12000, 81); }, 0.0, 0.3, 0.7);
            add ("Cymbals", "Ride",  [] { return cymbal (1.6, 2.6, 2800, 9500, 82, 0.4); }, 0.0, 0.3, 0.5);

            add ("Percussion", "Shaker",     [] { return shaker (0.18, 0.012, 22); });
            add ("Percussion", "Tambourine", [] { return metallic (0.35, 14, 6000, 9000, 91, 0.004); });
            add ("Percussion", "Cowbell",    [] { return tone (560, 540, 12, 0.5, 0.9); });
            add ("Percussion", "Block",      [] { return tone (900, 850, 60, 0.12, 0.0); });
            add ("Percussion", "Conga",      [] { return tone (340, 220, 14, 0.4, 0.0); });

            // ---- more drums: electronic and acoustic
            add ("Kicks", "Kick 909",      [] { return kickElectro (170, 52, 38, 5.5, 0.55, 0.25); });
            add ("Kicks", "Kick Boom",     [] { return kick (70, 36, 10, 1.8, 1.8, 1.1, 0.02); });
            add ("Kicks", "Kick Acoustic", [] { return kickAcoustic(); }, 0.22, 0.28);
            add ("Snares", "Snare 909",    [] { return snareElectro(); });
            add ("Snares", "Snare Room",   [] { return snareAcoustic(); }, 0.3, 0.36);
            add ("Snares", "Snare Brush",  [] { return snareBrush(); });
            add ("Snares", "Rim Click",    [] { return tone (2100, 1850, 110, 0.1, 0.5); });
            add ("Claps", "Clap Room",     [] { return clap (5, 0.014, 10, 0.5, 1200); }, 0.35, 0.4);
            add ("Hats", "Hat Closed 909", [] { return metallic (0.1, 60, 8500, 11500, 76, 0.0, 0.0, 1.25); });
            add ("Hats", "Hat Open 909",   [] { return metallic (0.55, 8, 7000, 10500, 77, 0.0, 0.0, 1.25); });
            add ("Hats", "Hat Pedal",      [] { return metallic (0.06, 90, 6500, 9500, 78, 0.0, 0.3); });
            add ("Toms", "Tom Floor",      [] { return membrane (92.0, 0.9, 5.2, 0.3, { { 1.0, 1.0, 1.0 }, { 1.59, 0.3, 1.5 }, { 2.14, 0.15, 2.0 } }); }, 0.25, 0.35);
            add ("Toms", "Tom Rack",       [] { return membrane (150.0, 0.7, 7.0, 0.3, { { 1.0, 1.0, 1.0 }, { 1.59, 0.3, 1.5 }, { 2.14, 0.15, 2.0 } }); }, 0.25, 0.3);
            add ("Percussion", "Clave",    [] { return tone (2500, 2350, 75, 0.1, 0.0); });
            add ("Percussion", "Snap",     [] { return snap(); });
            add ("Percussion", "Triangle", [] { return triangle(); }, 0.0, 0.3, 0.6);
            add ("Percussion", "Cabasa",   [] { return cabasa(); });

            // variations of the new drums
            add ("Variations", "Kick 909 soft",      [] { return kickElectro (170, 52, 38, 5.5, 0.55, 0.25, 0.85, 6); });
            add ("Variations", "Kick Acoustic soft", [] { return kickAcoustic (0.8, 9); }, 0.22, 0.28);
            add ("Variations", "Kick Acoustic b",    [] { return kickAcoustic (0.0, 10); }, 0.22, 0.28);
            add ("Variations", "Snare 909 soft",     [] { return snareElectro (0.8, 42); });
            add ("Variations", "Snare 909 b",        [] { return snareElectro (0.0, 43); });
            add ("Variations", "Snare 909 c",        [] { return snareElectro (0.0, 44); });
            add ("Variations", "Snare Room soft",    [] { return snareAcoustic (0.8, 32); }, 0.3, 0.36);
            add ("Variations", "Snare Room b",       [] { return snareAcoustic (0.0, 33, 1.02); }, 0.3, 0.36);
            add ("Variations", "Snare Room c",       [] { return snareAcoustic (0.0, 34, 0.98); }, 0.3, 0.36);
            add ("Variations", "Clap Room b",        [] { return clap (5, 0.014, 10, 0.5, 1200, 0.0, 24); }, 0.35, 0.4);
            add ("Variations", "Clap Room c",        [] { return clap (5, 0.014, 10, 0.5, 1200, 0.0, 25); }, 0.35, 0.4);
            add ("Variations", "Hat Closed 909 b",   [] { return metallic (0.1, 60, 8500, 11500, 176, 0.0, 0.0, 1.28); });
            add ("Variations", "Hat Closed 909 c",   [] { return metallic (0.1, 60, 8500, 11500, 276, 0.0, 0.0, 1.22); });
            add ("Variations", "Hat Closed 909 soft", [] { return metallic (0.09, 65, 8500, 11500, 376, 0.0, 0.8, 1.25); });
            add ("Variations", "Tom Floor b",        [] { return membrane (94.0, 0.9, 5.2, 0.3, { { 1.0, 1.0, 1.0 }, { 1.59, 0.3, 1.5 }, { 2.14, 0.15, 2.0 } }, 0.7); }, 0.25, 0.35);
            add ("Variations", "Tom Rack b",         [] { return membrane (153.0, 0.7, 7.0, 0.3, { { 1.0, 1.0, 1.0 }, { 1.59, 0.3, 1.5 }, { 2.14, 0.15, 2.0 } }, 0.7); }, 0.25, 0.3);

            add ("Bass", "808 Sub A1",  [] { return bass808 (55.0, 2.0, 1.5); });
            add ("Bass", "808 Sub E1",  [] { return bass808 (41.2, 2.0, 1.4); });
            add ("Bass", "Pluck Bass A1", [] { auto s = make (0.8); saw (s, 0, 55.0, 0.8, 2500, 250, 0.002, 0.2, 0.9f, 0.01, 2); finish (s); return s; });
            add ("Bass", "Fat Bass A1",   [] { auto s = make (1.2); saw (s, 0, 55.0, 1.2, 1800, 400, 0.004, 0.3, 0.9f, 0.02, 3); finish (s); return s; });

            add ("Synth", "Stab Cm7",   [] { return chordStab ({ 48, 51, 55, 58 }, 0.9); });
            add ("Synth", "Stab Fmaj7", [] { return chordStab ({ 53, 57, 60, 64 }, 0.9); });
            add ("Synth", "Stab Am9",   [] { return chordStab ({ 57, 60, 64, 67, 71 }, 1.0); });
            add ("Synth", "Pluck C4",   [] { auto s = make (0.7); saw (s, 0, midiHz (60), 0.7, 6000, 500, 0.001, 0.2, 0.9f, 0.006, 2); finish (s); return s; });
            add ("Synth", "Pad Swell Am", [] { auto s = make (4.0); for (int note : { 57, 60, 64, 69 }) saw (s, 0, midiHz (note), 4.0, 1200, 1200, 1.2, 1.2, 0.5f, 0.015, 4); finish (s); return s; });

            add ("FX", "Riser 4s",     [] { return riser (4.0, true); });
            add ("FX", "Downlifter 3s", [] { return riser (3.0, false); });
            add ("FX", "Impact",       [] { return impact(); });
            add ("FX", "Vinyl Crackle", [] { return vinyl(); });

            // loops (made from the sounds above)
            // Variations of the drums the kits use: a soft version (for light hits) and round-robin versions (so a repeated hit does not sound the same):
            // the drum rack puts them on the pad as velocity layers. Named "<drum> soft", "<drum> b", "<drum> c".
            add ("Variations", "Kick Punch soft", [] { return kick (150, 48, 28, 7, 0.5, 1.5, 0.15, 0.85, 6); });
            add ("Variations", "Kick Deep soft",  [] { return kick (110, 38, 18, 4.5, 0.9, 1.2, 0.05, 0.85, 6); });
            add ("Variations", "Kick 808 soft",   [] { return kick (90, 45, 12, 2.2, 1.6, 1.4, 0.03, 0.85, 6); });
            add ("Variations", "Kick Soft soft",  [] { return kick (120, 55, 22, 9, 0.4, 0.8, 0.02, 0.85, 6); });
            add ("Variations", "Kick Click soft", [] { return kick (220, 60, 60, 10, 0.35, 1.3, 0.4, 0.85, 6); });
            add ("Variations", "Snare Tight soft", [] { return snare (200, 28, 20, 0.3, 1800, 0.9, 0, 0.8, 14); });
            add ("Variations", "Snare Tight b",    [] { return snare (200, 28, 20, 0.3, 1800, 0.9, 0, 0.0, 12, 1.02); });
            add ("Variations", "Snare Tight c",    [] { return snare (200, 28, 20, 0.3, 1800, 0.9, 0, 0.0, 13, 0.98); });
            add ("Variations", "Snare Snap soft",  [] { return snare (240, 40, 26, 0.22, 2500, 1.1, 0, 0.8, 14); });
            add ("Variations", "Snare Snap b",     [] { return snare (240, 40, 26, 0.22, 2500, 1.1, 0, 0.0, 12, 1.02); });
            add ("Variations", "Snare Snap c",     [] { return snare (240, 40, 26, 0.22, 2500, 1.1, 0, 0.0, 13, 0.98); });
            add ("Variations", "Snare Lofi soft",  [] { return snare (180, 18, 13, 0.4, 900, 0.9, 5, 0.8, 14); });
            add ("Variations", "Snare Lofi b",     [] { return snare (180, 18, 13, 0.4, 900, 0.9, 5, 0.0, 12, 1.02); });
            add ("Variations", "Snare Fat soft",   [] { return snare (160, 14, 10, 0.45, 1200, 1.0, 0, 0.8, 14); });
            add ("Variations", "Snare Fat b",      [] { return snare (160, 14, 10, 0.45, 1200, 1.0, 0, 0.0, 12, 1.02); });
            add ("Variations", "Clap Tight b",  [] { return clap (4, 0.012, 20, 0.3, 1300, 0.0, 22); });
            add ("Variations", "Clap Tight c",  [] { return clap (4, 0.012, 20, 0.3, 1300, 0.0, 23); });
            add ("Variations", "Clap Wide b",   [] { return clap (5, 0.016, 9, 0.6, 1100, 0.0, 22); });
            add ("Variations", "Clap Wide c",   [] { return clap (5, 0.016, 9, 0.6, 1100, 0.0, 23); });
            add ("Variations", "Hat Closed 1 b", [] { return metallic (0.12, 55, 7000, 10000, 171, 0.0, 0.0, 1.03); });
            add ("Variations", "Hat Closed 1 c", [] { return metallic (0.12, 55, 7000, 10000, 271, 0.0, 0.0, 0.97); });
            add ("Variations", "Hat Closed 1 soft", [] { return metallic (0.1, 60, 7000, 10000, 371, 0.0, 0.8); });
            add ("Variations", "Hat Closed 2 b", [] { return metallic (0.09, 80, 8000, 11000, 172, 0.0, 0.0, 1.03); });
            add ("Variations", "Hat Closed 2 c", [] { return metallic (0.09, 80, 8000, 11000, 272, 0.0, 0.0, 0.97); });
            add ("Variations", "Hat Closed 2 soft", [] { return metallic (0.08, 85, 8000, 11000, 372, 0.0, 0.8); });
            add ("Variations", "Hat Closed 3 b", [] { return metallic (0.15, 40, 5500, 8000, 173, 0.0, 0.0, 1.03); });
            add ("Variations", "Hat Closed 3 c", [] { return metallic (0.15, 40, 5500, 8000, 273, 0.0, 0.0, 0.97); });
            add ("Variations", "Hat Open 1 b", [] { return metallic (0.6, 7, 6500, 9000, 174, 0.0, 0.0, 1.03); });
            add ("Variations", "Hat Open 2 b", [] { return metallic (0.9, 5, 5500, 8000, 175, 0.0, 0.0, 1.03); });
            add ("Variations", "Tom Low b",  [] { return tom (130, 70, 8, 0.7, 0.0, 4, 1.02); });
            add ("Variations", "Tom Mid b",  [] { return tom (180, 110, 9, 0.6, 0.0, 4, 1.02); });
            add ("Variations", "Tom High b", [] { return tom (250, 160, 10, 0.5, 0.0, 4, 1.02); });

            add ("Loops", "Boom Bap 90 BPM",   [] { return drumLoop (90, "X.......X.X.....", "....X.......X..X", "x.x.x.x.x.x.x.x.", "................", kick (130, 50, 25, 6, 0.5, 1.4, 0.1), snare (190, 16, 12, 0.35, 1400, 1.0), metallic (0.1, 60, 7000, 10000, 71), metallic (0.5, 8, 6500, 9000, 74), 0.12); });
            add ("Loops", "House 124 BPM",     [] { return drumLoop (124, "X...X...X...X...", "....X.......X...", "..x...x...x...x.", "..............x.", kick (110, 42, 20, 4.5, 0.6, 1.3, 0.05), clap (4, 0.012, 16, 0.4, 1300), metallic (0.08, 70, 8000, 11000, 72), metallic (0.5, 8, 6500, 9000, 74), 0.0); });
            add ("Loops", "Trap 140 BPM",      [] { return drumLoop (140, "X.....x...X.....", "........X.......", "x.x.x.x.xxx.x.x.", "..............x.", kick (90, 45, 12, 2.2, 1.0, 1.4, 0.03), snare (240, 30, 20, 0.3, 2200, 1.0), metallic (0.07, 80, 8500, 11000, 72), metallic (0.4, 9, 6500, 9000, 74), 0.0); });
            add ("Loops", "Lo-fi 80 BPM",      [] { return drumLoop (80, "X.......x.X.....", "....X.......X...", "x...x...x...x...", "................", kick (120, 55, 22, 9, 0.4, 0.9, 0.02), snare (180, 18, 13, 0.4, 900, 0.9, 5), metallic (0.14, 40, 5500, 8000, 73), metallic (0.5, 8, 6500, 9000, 74), 0.18); });
            add ("Loops", "Bass Loop Am 90 BPM", [] { return bassLoop (90, { { 45, 3 }, { 0, 1 }, { 45, 2 }, { 48, 2 }, { 50, 3 }, { 0, 1 }, { 52, 2 }, { 47, 2 } }, 900); });
            add ("Loops", "Bass Loop Em 124 BPM", [] { return bassLoop (124, { { 40, 2 }, { 40, 2 }, { 0, 2 }, { 47, 2 }, { 40, 2 }, { 43, 2 }, { 0, 2 }, { 38, 2 } }, 1400); });

            // ---- pack version 4: a bigger library
            // kicks
            add ("Kicks", "Kick Trap",    [] { return kick (85, 40, 10, 1.6, 1.4, 1.5, 0.05); });
            add ("Kicks", "Kick Sub",     [] { return kick (70, 35, 8, 1.3, 1.5, 1.0, 0.0); });
            add ("Kicks", "Kick House",   [] { return kick (125, 45, 28, 5.5, 0.5, 1.2, 0.12); });
            add ("Kicks", "Kick Techno",  [] { return kick (140, 48, 40, 6.5, 0.45, 1.8, 0.2); });
            add ("Kicks", "Kick Thump",   [] { return kick (100, 52, 16, 3.0, 0.55, 1.1, 0.05); });
            add ("Kicks", "Kick Lofi",    [] { return kick (115, 50, 18, 8.0, 0.45, 0.9, 0.02, 0.0, 5, 1.0); });
            add ("Kicks", "Kick Hard",    [] { return kick (190, 55, 55, 8.0, 0.4, 2.4, 0.35); });
            add ("Kicks", "Kick Long 808", [] { return kick (80, 40, 8, 1.2, 2.2, 1.3, 0.02); });
            // snares
            add ("Snares", "Snare Trap",    [] { return snare (250, 32, 22, 0.28, 2800, 1.15); });
            add ("Snares", "Snare Crack",   [] { return snare (280, 35, 24, 0.25, 3200, 1.2); });
            add ("Snares", "Snare Rock",    [] { return snare (170, 18, 14, 0.38, 1400, 1.05); });
            add ("Snares", "Snare Piccolo", [] { return snare (320, 45, 30, 0.2, 3500, 1.0); });
            add ("Snares", "Snare Dust",    [] { return snare (190, 20, 15, 0.35, 1000, 0.95, 4); });
            add ("Snares", "Snare Crunch",  [] { return snare (210, 26, 18, 0.3, 1600, 1.0, 6); });
            // claps
            add ("Claps", "Clap Snappy", [] { return clap (3, 0.01, 26, 0.25, 1600); });
            add ("Claps", "Clap Big",    [] { return clap (6, 0.018, 7, 0.7, 1000); }, 0.4, 0.45);
            add ("Claps", "Clap Lofi",   [] { return clap (4, 0.014, 14, 0.35, 900); });
            add ("Claps", "Clap Stack",  [] { return clap (5, 0.012, 12, 0.45, 1400); });
            // hats and cymbals
            add ("Hats", "Hat Closed 4",   [] { return metallic (0.07, 95, 9000, 12000, 79); });
            add ("Hats", "Hat Closed Tick", [] { return metallic (0.05, 130, 9500, 12500, 80); });
            add ("Hats", "Hat Open 3",     [] { return metallic (0.4, 10, 7500, 10500, 83); });
            add ("Hats", "Hat Open Long",  [] { return metallic (1.2, 4, 6000, 9000, 84); });
            add ("Cymbals", "Crash 2",    [] { return cymbal (3.2, 1.2, 1800, 11000, 85); }, 0.0, 0.3, 0.7);
            add ("Cymbals", "Ride Bell",  [] { return cymbal (1.8, 2.2, 3200, 9000, 86, 0.9); }, 0.0, 0.3, 0.5);
            add ("Cymbals", "Splash",     [] { return cymbal (1.0, 3.5, 3000, 12000, 87); }, 0.0, 0.3, 0.6);
            add ("Cymbals", "China",      [] { return cymbal (2.0, 2.0, 1500, 10000, 88); }, 0.0, 0.3, 0.6);
            // toms and percussion
            add ("Toms", "Tom Deep",   [] { return tom (100, 55, 7, 0.8); });
            add ("Toms", "Tom Electro", [] { return tom (200, 90, 16, 0.5); });
            add ("Percussion", "Bongo High",  [] { return tone (480, 380, 18, 0.3, 0.0); });
            add ("Percussion", "Bongo Low",   [] { return tone (320, 250, 15, 0.35, 0.0); });
            add ("Percussion", "Conga Low",   [] { return tone (240, 170, 12, 0.45, 0.0); });
            add ("Percussion", "Woodblock",   [] { return tone (1300, 1200, 50, 0.15, 0.2); });
            add ("Percussion", "Maracas",     [] { return shaker (0.1, 0.006, 35); });
            add ("Percussion", "Rim Soft",    [] { return tone (1600, 1400, 100, 0.1, 0.3); });
            add ("Percussion", "Tick",        [] { return tone (3000, 2800, 150, 0.05, 0.0); });
            add ("Percussion", "Perc Metal",  [] { return tone (1100, 1050, 18, 0.5, 1.2); });
            add ("Percussion", "Shaker Long", [] { return shaker (0.3, 0.03, 12); });
            // bass
            add ("Bass", "808 Sub G1",     [] { return bass808 (49.0, 2.2, 1.4); });
            add ("Bass", "808 Sub D1",     [] { return bass808 (36.7, 2.4, 1.3); });
            add ("Bass", "808 Sub C1",     [] { return bass808 (32.7, 2.6, 1.2); });
            add ("Bass", "808 Sub F1",     [] { return bass808 (43.65, 2.2, 1.4); });
            add ("Bass", "808 Short A1",   [] { return bass808 (55.0, 0.6, 5.0); });
            add ("Bass", "808 Dirty A1",   [] { return bass808Drive (55.0, 1.8, 1.6, 4.5); });
            add ("Bass", "Sub Sine A1",    [] { return subBass (55.0, 1.6, 1.8); });
            add ("Bass", "Sub Sine E1",    [] { return subBass (41.2, 1.8, 1.6); });
            add ("Bass", "Reese A1",       [] { return reese (55.0, 2.0); });
            add ("Bass", "Reese D1",       [] { return reese (36.7, 2.0); });
            add ("Bass", "Pluck Bass E1",  [] { auto s = make (0.8); saw (s, 0, 41.2, 0.8, 2500, 250, 0.002, 0.2, 0.9f, 0.01, 2); finish (s); return s; });
            add ("Bass", "Pluck Bass D2",  [] { auto s = make (0.7); saw (s, 0, 73.4, 0.7, 3000, 300, 0.002, 0.2, 0.9f, 0.01, 2); finish (s); return s; });
            add ("Bass", "Fat Bass E1",    [] { auto s = make (1.2); saw (s, 0, 41.2, 1.2, 1800, 400, 0.004, 0.3, 0.9f, 0.02, 3); finish (s); return s; });
            // synth: stabs, plucks, pads, leads, keys, bells
            add ("Synth", "Stab Dm7",     [] { return chordStab ({ 50, 53, 57, 60 }, 0.9); });
            add ("Synth", "Stab Gm7",     [] { return chordStab ({ 55, 58, 62, 65 }, 0.9); });
            add ("Synth", "Stab Cmaj9",   [] { return chordStab ({ 48, 52, 55, 59, 62 }, 1.0); });
            add ("Synth", "Stab Em7",     [] { return chordStab ({ 52, 55, 59, 62 }, 0.9); });
            add ("Synth", "Pluck E4",     [] { auto s = make (0.7); saw (s, 0, midiHz (64), 0.7, 6000, 500, 0.001, 0.2, 0.9f, 0.006, 2); finish (s); return s; });
            add ("Synth", "Pluck G3",     [] { auto s = make (0.7); saw (s, 0, midiHz (55), 0.7, 5000, 450, 0.001, 0.2, 0.9f, 0.006, 2); finish (s); return s; });
            add ("Synth", "Pad Swell Cmaj7", [] { auto s = make (4.0); for (int note : { 48, 52, 55, 59 }) saw (s, 0, midiHz (note), 4.0, 1200, 1200, 1.2, 1.2, 0.5f, 0.015, 4); finish (s); return s; });
            add ("Synth", "Pad Swell Fm9",   [] { auto s = make (4.0); for (int note : { 53, 56, 60, 63, 67 }) saw (s, 0, midiHz (note), 4.0, 1100, 1100, 1.3, 1.2, 0.45f, 0.015, 4); finish (s); return s; });
            add ("Synth", "Pad Swell Em7",   [] { auto s = make (4.0); for (int note : { 52, 55, 59, 62 }) saw (s, 0, midiHz (note), 4.0, 1200, 1200, 1.2, 1.2, 0.5f, 0.015, 4); finish (s); return s; });
            add ("Synth", "Pad Warm Dm",     [] { auto s = make (5.0); for (int note : { 50, 57, 62, 65 }) saw (s, 0, midiHz (note), 5.0, 800, 800, 1.6, 1.5, 0.5f, 0.02, 4); finish (s); return s; });
            add ("Synth", "Lead Saw A4",     [] { auto s = make (1.2); saw (s, 0, midiHz (69), 1.2, 4500, 2500, 0.01, 0.25, 0.8f, 0.012, 3); finish (s); return s; });
            add ("Synth", "Lead Saw E4",     [] { auto s = make (1.2); saw (s, 0, midiHz (64), 1.2, 4500, 2500, 0.01, 0.25, 0.8f, 0.012, 3); finish (s); return s; });
            add ("Synth", "Lead Square C4",  [] { return squareLead (midiHz (60), 1.2, 2800); });
            add ("Synth", "Lead Square G4",  [] { return squareLead (midiHz (67), 1.2, 2800); });
            add ("Synth", "Bell C5",         [] { return bell (midiHz (72), 3.0); }, 0.0, 0.3, 0.5);
            add ("Synth", "Bell G5",         [] { return bell (midiHz (79), 3.0); }, 0.0, 0.3, 0.5);
            add ("Synth", "E-Piano C3",      [] { return epiano (midiHz (48), 2.5); }, 0.0, 0.3, 0.4);
            add ("Synth", "E-Piano E3",      [] { return epiano (midiHz (52), 2.5); }, 0.0, 0.3, 0.4);
            add ("Synth", "E-Piano G3",      [] { return epiano (midiHz (55), 2.5); }, 0.0, 0.3, 0.4);
            add ("Synth", "E-Piano C4",      [] { return epiano (midiHz (60), 2.5); }, 0.0, 0.3, 0.4);
            add ("Synth", "Marimba C4",      [] { return marimba (midiHz (60), 1.5); });
            add ("Synth", "Marimba G4",      [] { return marimba (midiHz (67), 1.5); });
            // effects
            add ("FX", "Riser 8s",       [] { return riser (8.0, true); });
            add ("FX", "Riser 2s",       [] { return riser (2.0, true); });
            add ("FX", "Downlifter 6s",  [] { return riser (6.0, false); });
            add ("FX", "Impact Big",     [] { auto s = impact(); for (size_t i = 0; i < n (s); ++i) s[i] *= (float) (1.0 + 0.6 * std::exp (-(double) i / rate * 3.0)); finish (s); return s; });
            add ("FX", "Impact Short",   [] { return kick (110, 38, 14, 5.0, 0.5, 1.6, 0.1); });
            add ("FX", "Whoosh",         [] { return whoosh (1.6); });
            add ("FX", "Whoosh Long",    [] { return whoosh (3.5); });
            add ("FX", "Laser Up",       [] { return laser (0.8, false); });
            add ("FX", "Laser Down",     [] { return laser (0.8, true); });
            add ("FX", "Reverse Crash",  [] { return reversed (cymbal (3.0, 1.2, 1800, 11000, 85)); }, 0.0, 0.3, 0.7);
            add ("FX", "Reverse Snare",  [] { return reversed (snare (200, 18, 12, 0.5, 1400, 1.0)); });
            add ("FX", "Noise Burst",    [] { return noiseBurst (0.6, 6.0, 800.0); });
            add ("FX", "Noise Hit",      [] { return noiseBurst (0.25, 14.0, 2500.0); });

            // loops at more tempos: drums, bass and chords
            add ("Loops", "Boom Bap 85 BPM",    [] { return drumLoop (85, "X.....x.X..x....", "....X.......X...", "x.x.x.x.x.x.x.x.", "..............x.", kick (100, 52, 16, 3, 0.55, 1.1, 0.05), snare (190, 20, 15, 0.35, 1000, 0.95, 4), metallic (0.1, 60, 7000, 10000, 71), metallic (0.5, 8, 6500, 9000, 74), 0.1); });
            add ("Loops", "Hip-hop 95 BPM",     [] { return drumLoop (95, "X.......X.X..X..", "....X.......X..X", "x.xxx.xxx.xxx.xx", "................", kick (130, 50, 25, 6, 0.5, 1.4, 0.1), snare (250, 32, 22, 0.28, 2800, 1.15), metallic (0.08, 80, 8000, 11000, 79), metallic (0.5, 8, 6500, 9000, 74), 0.08); });
            add ("Loops", "House 128 BPM",      [] { return drumLoop (128, "X...X...X...X...", "....X.......X...", ".x.x.x.x.x.x.x.x", "..x...x...x...x.", kick (125, 45, 28, 5.5, 0.5, 1.2, 0.12), clap (4, 0.012, 16, 0.4, 1300), metallic (0.05, 130, 9500, 12500, 80), metallic (0.4, 10, 7500, 10500, 83), 0.0); });
            add ("Loops", "Techno 130 BPM",     [] { return drumLoop (130, "X...X...X...X...", "............X...", "..x...x...x...x.", ".x.x.x.x.x.x.x.x", kick (140, 48, 40, 6.5, 0.45, 1.8, 0.2), clap (3, 0.01, 26, 0.25, 1600), metallic (0.05, 130, 9500, 12500, 80), metallic (0.4, 10, 7500, 10500, 83), 0.0); });
            add ("Loops", "Trap 150 BPM",       [] { return drumLoop (150, "X.....x...X.....", "........X.......", "x.x.xxxxx.x.x.xx", "..............x.", kick (85, 40, 10, 1.6, 1.4, 1.5, 0.05), snare (250, 32, 22, 0.28, 2800, 1.15), metallic (0.07, 95, 9000, 12000, 79), metallic (0.4, 10, 7500, 10500, 83), 0.0); });
            add ("Loops", "Drum and Bass 174 BPM", [] { return drumLoop (174, "X.........x.....", "....X.......X...", "x.x.x.x.x.x.x.x.", "..............x.", kick (120, 48, 30, 5, 0.5, 1.4, 0.2), snare (280, 35, 24, 0.25, 3200, 1.2), metallic (0.06, 110, 9000, 12000, 80), metallic (0.35, 12, 7500, 10500, 83), 0.0); });
            add ("Loops", "Lo-fi 70 BPM",       [] { return drumLoop (70, "X.......x.X.....", "....X.......X...", "x...x...x...x...", "................", kick (115, 50, 18, 8, 0.45, 0.9, 0.02), snare (180, 18, 13, 0.4, 900, 0.9, 5), metallic (0.14, 40, 5500, 8000, 73), metallic (0.5, 8, 6500, 9000, 74), 0.2); });
            add ("Loops", "Breakbeat 135 BPM",  [] { return drumLoop (135, "X.....x...x.X...", "....X..x....X.x.", "x.x.x.x.x.x.x.x.", "................", kick (130, 50, 25, 6, 0.5, 1.4, 0.1), snare (190, 16, 12, 0.35, 1400, 1.0), metallic (0.1, 60, 7000, 10000, 71), metallic (0.5, 8, 6500, 9000, 74), 0.05); });
            add ("Loops", "Bass Loop Gm 130 BPM", [] { return bassLoop (130, { { 43, 3 }, { 0, 1 }, { 43, 2 }, { 46, 2 }, { 48, 3 }, { 0, 1 }, { 46, 2 }, { 41, 2 } }, 1200); });
            add ("Loops", "Bass Loop Cm 140 BPM", [] { return bassLoop (140, { { 36, 2 }, { 36, 2 }, { 0, 2 }, { 39, 2 }, { 36, 2 }, { 43, 2 }, { 0, 2 }, { 34, 2 } }, 1000); });
            add ("Loops", "Bass Loop Dm 174 BPM", [] { return bassLoop (174, { { 38, 4 }, { 0, 2 }, { 38, 2 }, { 41, 4 }, { 0, 2 }, { 36, 2 } }, 1400); });
            add ("Loops", "Bass Loop Am 110 BPM", [] { return bassLoop (110, { { 45, 2 }, { 0, 2 }, { 45, 2 }, { 48, 2 }, { 50, 2 }, { 0, 2 }, { 52, 2 }, { 45, 2 } }, 800); });
            add ("Loops", "Chords Am F C G 90 BPM",  [] { return chordLoop (90, { { 57, 60, 64 }, { 53, 57, 60 }, { 48, 52, 55 }, { 55, 59, 62 } }, 16, 1800, 0.01); });
            add ("Loops", "Chords Cm Ab Eb Bb 80 BPM", [] { return chordLoop (80, { { 48, 51, 55, 58 }, { 44, 48, 51, 55 }, { 51, 55, 58, 62 }, { 46, 50, 53, 57 } }, 16, 1400, 0.03); });
            add ("Loops", "Chords Dm7 G7 Cmaj7 100 BPM", [] { return chordLoop (100, { { 50, 53, 57, 60 }, { 55, 59, 62, 65 }, { 48, 52, 55, 59 }, { 48, 52, 55, 59 } }, 16, 1600, 0.02); });
            add ("Loops", "Chords Em C G D 120 BPM",  [] { return chordLoop (120, { { 52, 55, 59 }, { 48, 52, 55 }, { 55, 59, 62 }, { 50, 54, 57 } }, 16, 2200, 0.005, 0.8); });

            // soft and round-robin versions of the new drums that the new kits use
            add ("Variations", "Kick Trap soft",   [] { return kick (85, 40, 10, 1.6, 1.4, 1.5, 0.05, 0.85, 6); });
            add ("Variations", "Kick Techno soft", [] { return kick (140, 48, 40, 6.5, 0.45, 1.8, 0.2, 0.85, 6); });
            add ("Variations", "Kick Thump soft",  [] { return kick (100, 52, 16, 3.0, 0.55, 1.1, 0.05, 0.85, 6); });
            add ("Variations", "Snare Trap soft",  [] { return snare (250, 32, 22, 0.28, 2800, 1.15, 0, 0.8, 14); });
            add ("Variations", "Snare Trap b",     [] { return snare (250, 32, 22, 0.28, 2800, 1.15, 0, 0.0, 12, 1.02); });
            add ("Variations", "Snare Crack soft", [] { return snare (280, 35, 24, 0.25, 3200, 1.2, 0, 0.8, 14); });
            add ("Variations", "Snare Crack b",    [] { return snare (280, 35, 24, 0.25, 3200, 1.2, 0, 0.0, 12, 1.02); });
            add ("Variations", "Snare Rock soft",  [] { return snare (170, 18, 14, 0.38, 1400, 1.05, 0, 0.8, 14); });
            add ("Variations", "Snare Rock b",     [] { return snare (170, 18, 14, 0.38, 1400, 1.05, 0, 0.0, 12, 1.02); });
            add ("Variations", "Snare Dust soft",  [] { return snare (190, 20, 15, 0.35, 1000, 0.95, 4, 0.8, 14); });
            add ("Variations", "Snare Dust b",     [] { return snare (190, 20, 15, 0.35, 1000, 0.95, 4, 0.0, 12, 1.02); });
            add ("Variations", "Hat Closed 4 b",   [] { return metallic (0.07, 95, 9000, 12000, 179, 0.0, 0.0, 1.03); });
            add ("Variations", "Hat Closed 4 soft", [] { return metallic (0.06, 100, 9000, 12000, 379, 0.0, 0.8); });
            add ("Variations", "Hat Closed Tick b", [] { return metallic (0.05, 130, 9500, 12500, 180, 0.0, 0.0, 1.03); });

            // ---- pack version 5: even more
            // vintage drum machines
            add ("Kicks", "Kick 707",     [] { return kick (160, 50, 35, 6.0, 0.45, 1.3, 0.2); });
            add ("Kicks", "Kick 606",     [] { return kick (130, 55, 30, 7.0, 0.4, 1.0, 0.1); });
            add ("Kicks", "Kick Disco",   [] { return kick (120, 48, 26, 5.0, 0.5, 1.2, 0.15); });
            add ("Kicks", "Kick Funk",    [] { return kickAcoustic (0.0, 12); }, 0.2, 0.25);
            add ("Snares", "Snare 707",   [] { return snare (230, 35, 25, 0.25, 2400, 1.0); });
            add ("Snares", "Snare 606",   [] { return snare (260, 40, 28, 0.22, 2800, 0.9, 6); });
            add ("Snares", "Snare Disco", [] { return snare (200, 22, 16, 0.32, 1500, 1.0); }, 0.25, 0.3);
            add ("Snares", "Snare Funk",  [] { return snareAcoustic (0.0, 36, 1.05); }, 0.25, 0.3);
            add ("Claps", "Clap 808",     [] { return clap (3, 0.01, 20, 0.3, 1100); });
            add ("Claps", "Clap Disco",   [] { return clap (5, 0.013, 11, 0.5, 1300); }, 0.3, 0.35);
            add ("Hats", "Hat 808 Closed", [] { return hat808 (0.1, 40, 6000); });
            add ("Hats", "Hat 808 Open",   [] { return hat808 (0.7, 6, 5500); });
            add ("Hats", "Hat 808 Pedal",  [] { return hat808 (0.05, 80, 7000); });
            add ("Toms", "Tom 808 Low",   [] { return tom (110, 70, 6, 0.8); });
            add ("Toms", "Tom 808 Mid",   [] { return tom (160, 100, 7, 0.7); });
            add ("Toms", "Tom 808 High",  [] { return tom (230, 150, 8, 0.6); });
            add ("Percussion", "Cowbell 808", [] { return cowbell808(); });
            add ("Percussion", "Conga 808",   [] { return tone (300, 220, 10, 0.4, 0.0); });
            // world percussion
            add ("Percussion", "Timbale High", [] { return tone (700, 600, 22, 0.4, 0.6); });
            add ("Percussion", "Timbale Low",  [] { return tone (450, 380, 18, 0.45, 0.6); });
            add ("Percussion", "Agogo High",   [] { return tone (1100, 1090, 14, 0.5, 0.8); });
            add ("Percussion", "Agogo Low",    [] { return tone (800, 790, 14, 0.5, 0.8); });
            add ("Percussion", "Guiro",        [] { return guiro(); });
            add ("Percussion", "Castanet",     [] { return castanet(); });
            add ("Percussion", "Tabla High",   [] { return membrane (330.0, 0.5, 9.0, 0.4, { { 1.0, 1.0, 1.0 }, { 1.5, 0.4, 1.2 }, { 2.0, 0.2, 1.6 } }); }, 0.12, 0.2);
            add ("Percussion", "Tabla Low",    [] { return membrane (170.0, 0.7, 6.0, 0.3, { { 1.0, 1.0, 1.0 }, { 1.5, 0.4, 1.2 }, { 2.0, 0.2, 1.6 } }); }, 0.12, 0.2);
            add ("Percussion", "Djembe Bass",  [] { return membrane (120.0, 0.8, 6.0, 0.5, { { 1.0, 1.0, 1.0 }, { 1.59, 0.3, 1.5 }, { 2.14, 0.15, 2.0 } }); }, 0.12, 0.2);
            add ("Percussion", "Djembe Slap",  [] { return tone (520, 330, 22, 0.25, 0.9); });
            add ("Percussion", "Frame Drum",   [] { return membrane (110.0, 0.9, 4.5, 0.2, { { 1.0, 1.0, 1.0 }, { 1.59, 0.35, 1.4 }, { 2.14, 0.2, 1.8 } }); }, 0.2, 0.3);
            add ("Percussion", "Whistle",      [] { return tone (2200, 2300, 5, 0.5, 0.0); });
            add ("Percussion", "Shaker Tight", [] { return shaker (0.08, 0.004, 40); });
            // 808 and sub bass on every note, wobbles and FM
            add ("Bass", "808 Sub Db1", [] { return bass808 (34.65, 2.4, 1.3); });
            add ("Bass", "808 Sub Eb1", [] { return bass808 (38.89, 2.4, 1.3); });
            add ("Bass", "808 Sub Gb1", [] { return bass808 (46.25, 2.2, 1.4); });
            add ("Bass", "808 Sub Ab1", [] { return bass808 (51.91, 2.2, 1.4); });
            add ("Bass", "808 Sub Bb1", [] { return bass808 (58.27, 2.0, 1.5); });
            add ("Bass", "808 Sub B1",  [] { return bass808 (61.74, 2.0, 1.5); });
            add ("Bass", "FM Bass A1",  [] { return fmBass (55.0, 1.2, 3.0); });
            add ("Bass", "FM Bass E1",  [] { return fmBass (41.2, 1.4, 3.0); });
            add ("Bass", "Wobble Bass A1", [] { return wobble (55.0, 2.0, 3.0, 900.0); });
            add ("Bass", "Wobble Bass E1", [] { return wobble (41.2, 2.0, 2.0, 1200.0); });
            add ("Bass", "Bass String E1", [] { return pluckString (41.2, 2.0, 0.998, 0.25, 21); });
            add ("Bass", "Bass String A1", [] { return pluckString (55.0, 2.0, 0.998, 0.25, 22); });
            // keys
            add ("Keys", "Organ C3",      [] { return organ (midiHz (48), 2.0, { 1.0, 0.8, 0.6, 0.4, 0.3, 0.2 }); });
            add ("Keys", "Organ E3",      [] { return organ (midiHz (52), 2.0, { 1.0, 0.8, 0.6, 0.4, 0.3, 0.2 }); });
            add ("Keys", "Organ G3",      [] { return organ (midiHz (55), 2.0, { 1.0, 0.8, 0.6, 0.4, 0.3, 0.2 }); });
            add ("Keys", "Organ Soft C3", [] { return organ (midiHz (48), 2.0, { 1.0, 0.5, 0.1, 0.0, 0.1, 0.0 }); });
            add ("Keys", "Organ Soft E3", [] { return organ (midiHz (52), 2.0, { 1.0, 0.5, 0.1, 0.0, 0.1, 0.0 }); });
            add ("Keys", "Organ Soft G3", [] { return organ (midiHz (55), 2.0, { 1.0, 0.5, 0.1, 0.0, 0.1, 0.0 }); });
            add ("Keys", "Clav C3",       [] { return clav (midiHz (48), 1.0); });
            add ("Keys", "Clav E3",       [] { return clav (midiHz (52), 1.0); });
            add ("Keys", "Clav G3",       [] { return clav (midiHz (55), 1.0); });
            add ("Keys", "Kalimba C5",    [] { return kalimba (midiHz (72), 2.0); });
            add ("Keys", "Kalimba E5",    [] { return kalimba (midiHz (76), 2.0); });
            add ("Keys", "Kalimba G5",    [] { return kalimba (midiHz (79), 2.0); });
            add ("Keys", "Marimba E4",    [] { return marimba (midiHz (64), 1.5); });
            add ("Keys", "Bell E5",       [] { return bell (midiHz (76), 3.0); }, 0.0, 0.3, 0.5);
            add ("Keys", "E-Piano A3",    [] { return epiano (midiHz (57), 2.5); }, 0.0, 0.3, 0.4);
            // plucked strings
            add ("Plucks", "Guitar A2",       [] { return pluckString (110.0, 2.5, 0.996, 0.5, 7); });
            add ("Plucks", "Guitar D3",       [] { return pluckString (146.8, 2.5, 0.996, 0.5, 8); });
            add ("Plucks", "Guitar E3",       [] { return pluckString (164.8, 2.5, 0.996, 0.5, 9); });
            add ("Plucks", "Guitar G3",       [] { return pluckString (196.0, 2.5, 0.996, 0.5, 10); });
            add ("Plucks", "Guitar Muted E3", [] { return pluckString (164.8, 0.5, 0.97, 0.4, 11); });
            add ("Plucks", "Nylon C4",        [] { return pluckString (261.6, 2.5, 0.997, 0.35, 12); });
            add ("Plucks", "Nylon E4",        [] { return pluckString (329.6, 2.5, 0.997, 0.35, 13); });
            add ("Plucks", "Harp C4",         [] { return pluckString (261.6, 3.5, 0.9985, 0.6, 14); }, 0.0, 0.3, 0.5);
            add ("Plucks", "Harp G4",         [] { return pluckString (392.0, 3.5, 0.9985, 0.6, 15); }, 0.0, 0.3, 0.5);
            add ("Plucks", "Banjo D4",        [] { return pluckString (293.7, 1.2, 0.99, 0.9, 16); });
            add ("Plucks", "Pizzicato C4",    [] { return pluckString (261.6, 0.6, 0.97, 0.7, 17); });
            add ("Plucks", "Pizzicato G3",    [] { return pluckString (196.0, 0.7, 0.97, 0.7, 18); });
            // voices
            add ("Vocal", "Vox Ah A3", [] { return vox (220.0, 2.5, 800, 1150, 2900); });
            add ("Vocal", "Vox Oh A3", [] { return vox (220.0, 2.5, 450, 800, 2830); });
            add ("Vocal", "Vox Ee A3", [] { return vox (220.0, 2.5, 400, 1700, 2600); });
            add ("Vocal", "Vox Oo A3", [] { return vox (220.0, 2.5, 325, 700, 2530); });
            add ("Vocal", "Vox Ah E4", [] { return vox (329.6, 2.5, 800, 1150, 2900); });
            add ("Vocal", "Vox Oh E4", [] { return vox (329.6, 2.5, 450, 800, 2830); });
            add ("Vocal", "Choir Ah Am", [] { Sig s = make (4.0); for (int note : { 57, 60, 64, 69 }) { auto v = vox (midiHz (note), 4.0, 800, 1150, 2900, 0.5); mix (s, v, 0, 0.5f); } finish (s); return s; }, 0.0, 0.3, 0.6);
            add ("Vocal", "Choir Oh Fmaj7", [] { Sig s = make (4.0); for (int note : { 53, 57, 60, 64 }) { auto v = vox (midiHz (note), 4.0, 450, 800, 2830, 0.5); mix (s, v, 0, 0.5f); } finish (s); return s; }, 0.0, 0.3, 0.6);
            add ("Vocal", "Choir Oo Cmaj", [] { Sig s = make (4.0); for (int note : { 48, 55, 60, 64 }) { auto v = vox (midiHz (note), 4.0, 325, 700, 2530, 0.5); mix (s, v, 0, 0.5f); } finish (s); return s; }, 0.0, 0.3, 0.6);
            // brass and strings
            add ("Synth", "Brass Stab Cm",   [] { return brassStab ({ 48, 51, 55, 60 }, 0.7); });
            add ("Synth", "Brass Stab F",    [] { return brassStab ({ 53, 57, 60, 65 }, 0.7); });
            add ("Synth", "Brass Stab G",    [] { return brassStab ({ 55, 59, 62, 67 }, 0.7); });
            add ("Synth", "Strings Pad Am",  [] { return stringsPad ({ 57, 60, 64, 69 }, 4.0); }, 0.0, 0.3, 0.6);
            add ("Synth", "Strings Pad C",   [] { return stringsPad ({ 48, 55, 60, 64 }, 4.0); }, 0.0, 0.3, 0.6);
            add ("Synth", "Strings Pad Dm",  [] { return stringsPad ({ 50, 57, 62, 65 }, 4.0); }, 0.0, 0.3, 0.6);
            // effects and textures
            add ("FX", "Glitch",        [] { return glitch (1.0); });
            add ("FX", "Tape Stop",     [] { return tapeStop (1.5); });
            add ("FX", "Sweep Up 4s",   [] { return sweepNoise (4.0, true); });
            add ("FX", "Sweep Down 4s", [] { return sweepNoise (4.0, false); });
            add ("Textures", "Wind 8s",       [] { return windNoise (8.0); });
            add ("Textures", "Rain 8s",       [] { return rainNoise (8.0); });
            add ("Textures", "Tape Hiss 4s",  [] { return tapeHiss (4.0); });
            add ("Textures", "Drone A1",      [] { return drone (55.0, 8.0); }, 0.0, 0.3, 0.6);
            add ("Textures", "Drone E2",      [] { return drone (82.4, 8.0); }, 0.0, 0.3, 0.6);
            add ("Textures", "Drone C2",      [] { return drone (65.4, 8.0); }, 0.0, 0.3, 0.6);

            // more loops: grooves from other styles, arpeggios and melodies
            add ("Loops", "Reggaeton 95 BPM",    [] { return drumLoop (95, "X..X..X...X..X..", "...X..X....X..X.", "x.x.x.x.x.x.x.x.", "................", kick (110, 45, 20, 4.5, 0.6, 1.3, 0.05), snare (250, 32, 22, 0.28, 2800, 1.15), metallic (0.07, 95, 9000, 12000, 79), metallic (0.4, 10, 7500, 10500, 83), 0.0); });
            add ("Loops", "UK Garage 130 BPM",   [] { return drumLoop (130, "X.......X.......", "....X.......X..x", "x.x.x.x.x.x.x.x.", "..x.......x.....", kick (125, 45, 28, 5.5, 0.5, 1.2, 0.12), clap (4, 0.012, 16, 0.4, 1300), metallic (0.05, 130, 9500, 12500, 80), metallic (0.4, 10, 7500, 10500, 83), 0.2); });
            add ("Loops", "Dubstep 140 BPM",     [] { return drumLoop (140, "X.........X.....", "........X.......", "x.x.x.x.x.x.x.x.", "..............x.", kick (85, 40, 10, 1.6, 1.0, 1.5, 0.05), snare (210, 26, 18, 0.3, 1600, 1.0, 6), metallic (0.07, 95, 9000, 12000, 79), metallic (0.4, 10, 7500, 10500, 83), 0.0); });
            add ("Loops", "Jersey Club 140 BPM", [] { return drumLoop (140, "X..X..X...X.X...", "....X.......X...", "x.x.x.x.x.x.x.x.", "................", kick (110, 45, 20, 4.5, 0.5, 1.3, 0.1), clap (4, 0.012, 20, 0.3, 1400), metallic (0.06, 110, 9000, 12000, 80), metallic (0.4, 10, 7500, 10500, 83), 0.0); });
            add ("Loops", "Afrobeat 105 BPM",    [] { return drumLoop (105, "X..X..X.X..X....", "....X.......X...", "x.xxx.xxx.xxx.xx", "................", kick (110, 48, 20, 4.5, 0.5, 1.2, 0.08), snare (190, 20, 15, 0.35, 1000, 0.95, 4), metallic (0.08, 85, 8500, 11500, 79), metallic (0.4, 10, 7500, 10500, 83), 0.05); });
            add ("Loops", "Disco 118 BPM",       [] { return drumLoop (118, "X...X...X...X...", "....X.......X...", "x.x.x.x.x.x.x.x.", "..x...x...x...x.", kick (120, 48, 26, 5.0, 0.5, 1.2, 0.15), clap (5, 0.013, 11, 0.5, 1300), metallic (0.06, 120, 9000, 12000, 80), metallic (0.5, 8, 7000, 10000, 83), 0.0); });
            add ("Loops", "Funk 100 BPM",        [] { return drumLoop (100, "X..x..X...x.X...", "....X..x....X.x.", "x.xxx.xxx.xxx.xx", "................", kickAcoustic (0.0, 12), snareAcoustic (0.0, 36), metallic (0.08, 85, 8500, 11500, 79), metallic (0.4, 10, 7500, 10500, 83), 0.08); });
            add ("Loops", "Rock 120 BPM",        [] { return drumLoop (120, "X.....X.X.......", "....X.......X...", "x.x.x.x.x.x.x.x.", "..............x.", kickAcoustic(), snareAcoustic(), metallic (0.1, 60, 7000, 10000, 71), metallic (0.6, 7, 6500, 9000, 74), 0.0); });
            add ("Loops", "Half-time 70 BPM",    [] { return drumLoop (70, "X.......X.......", "........X.......", "x.x.x.x.x.x.x.x.", "..............x.", kick (100, 52, 16, 3, 0.55, 1.1, 0.05), snare (190, 20, 15, 0.35, 1000, 0.95, 4), metallic (0.1, 60, 7000, 10000, 71), metallic (0.5, 8, 6500, 9000, 74), 0.12); });
            add ("Loops", "Arp Am 120 BPM",      [] { return arpLoop (120, { 57, 60, 64, 69, 64, 60 }, 3000); });
            add ("Loops", "Arp Cm 130 BPM",      [] { return arpLoop (130, { 48, 51, 55, 60, 55, 51 }, 2800); });
            add ("Loops", "Arp Dm 100 BPM",      [] { return arpLoop (100, { 50, 53, 57, 62, 57, 53 }, 3200); });
            add ("Loops", "Arp Gm 140 BPM",      [] { return arpLoop (140, { 55, 58, 62, 67, 62, 58 }, 2600); });
            add ("Loops", "Arp Em 110 BPM",      [] { return arpLoop (110, { 52, 55, 59, 64, 59, 55, 52, 59 }, 3400); });
            add ("Loops", "Melody Am 100 BPM",   [] { return melodyLoop (100, { { 69, 4 }, { 72, 2 }, { 71, 2 }, { 69, 4 }, { 67, 2 }, { 64, 2 }, { 67, 4 }, { 0, 2 }, { 69, 2 }, { 71, 4 }, { 69, 4 } }, 3000); });
            add ("Loops", "Melody C 120 BPM",    [] { return melodyLoop (120, { { 72, 2 }, { 76, 2 }, { 79, 4 }, { 76, 2 }, { 74, 2 }, { 72, 4 }, { 67, 2 }, { 69, 2 }, { 72, 4 }, { 0, 4 } }, 3400); });
            add ("Loops", "Melody Em 90 BPM",    [] { return melodyLoop (90, { { 71, 4 }, { 74, 2 }, { 76, 2 }, { 74, 4 }, { 71, 4 }, { 67, 4 }, { 69, 2 }, { 71, 2 }, { 69, 4 }, { 0, 4 } }, 2600); });
            add ("Loops", "Chords Fm Db Ab Eb 85 BPM", [] { return chordLoop (85, { { 53, 56, 60 }, { 49, 53, 56 }, { 56, 60, 63 }, { 51, 55, 58 } }, 16, 1500, 0.03); });
            add ("Loops", "Chords C Am F G 110 BPM",   [] { return chordLoop (110, { { 48, 52, 55 }, { 57, 60, 64 }, { 53, 57, 60 }, { 55, 59, 62 } }, 16, 2000, 0.01); });

            // ---- pack version 6: much more
            // more drums, each a little different
            add ("Kicks", "Kick Tight",   [] { return kick (150, 50, 40, 8.0, 0.35, 1.2, 0.15); });
            add ("Kicks", "Kick Round",   [] { return kick (100, 46, 14, 4.0, 0.6, 1.0, 0.0); });
            add ("Kicks", "Kick Boomy",   [] { return kick (75, 38, 9, 2.0, 1.2, 1.2, 0.0); });
            add ("Kicks", "Kick Snap",    [] { return kick (200, 58, 60, 9.0, 0.3, 1.5, 0.4); });
            add ("Kicks", "Kick Dub",     [] { return kick (85, 42, 11, 2.5, 1.0, 1.1, 0.0); });
            add ("Kicks", "Kick Rubber",  [] { return kick (135, 44, 22, 5.0, 0.55, 1.8, 0.1); });
            add ("Kicks", "Kick Pillow",  [] { return kick (95, 52, 15, 6.0, 0.4, 0.7, 0.0); });
            add ("Kicks", "Kick Chunk",   [] { return kick (170, 55, 35, 6.5, 0.45, 2.0, 0.25); });
            add ("Kicks", "Kick Gabber",  [] { return kick (180, 50, 30, 3.5, 0.7, 3.5, 0.3); });
            add ("Kicks", "Kick Sine",    [] { return kick (80, 48, 14, 3.0, 0.7, 0.4, 0.0); });
            add ("Snares", "Snare Wide",   [] { return snare (190, 18, 11, 0.42, 1300, 1.05); }, 0.3, 0.35);
            add ("Snares", "Snare Thin",   [] { return snare (300, 50, 30, 0.2, 3000, 1.0); });
            add ("Snares", "Snare Boxy",   [] { return snare (170, 24, 22, 0.28, 800, 0.8); });
            add ("Snares", "Snare Slap",   [] { return snare (260, 45, 28, 0.22, 2600, 1.2); });
            add ("Snares", "Snare Smack",  [] { return snare (230, 40, 26, 0.25, 2000, 1.3, 7); });
            add ("Snares", "Snare Sizzle", [] { return snare (200, 22, 9, 0.5, 2200, 1.4); });
            add ("Snares", "Snare Pop",    [] { return snare (330, 60, 36, 0.18, 3200, 0.9); });
            add ("Snares", "Snare Whip",   [] { return snare (210, 30, 14, 0.36, 3600, 1.5); });
            add ("Claps", "Clap Dry",     [] { return clap (3, 0.009, 30, 0.2, 1500); });
            add ("Claps", "Clap Fat",     [] { return clap (6, 0.015, 10, 0.5, 900); });
            add ("Claps", "Clap Crisp",   [] { return clap (4, 0.011, 24, 0.28, 2000); });
            add ("Claps", "Clap Hall",    [] { return clap (5, 0.014, 9, 0.6, 1200); }, 0.5, 0.6);
            add ("Claps", "Clap Layered", [] { return clap (7, 0.012, 14, 0.45, 1100); });
            add ("Hats", "Hat Closed 5",    [] { return metallic (0.08, 85, 8500, 11500, 301); });
            add ("Hats", "Hat Closed 6",    [] { return metallic (0.1, 70, 7500, 10500, 302); });
            add ("Hats", "Hat Closed Dark", [] { return metallic (0.12, 50, 5000, 7500, 303); });
            add ("Hats", "Hat Closed Bright", [] { return metallic (0.06, 110, 10000, 13000, 304); });
            add ("Hats", "Hat Open 4",      [] { return metallic (0.5, 9, 7000, 10000, 305); });
            add ("Hats", "Hat Open Dark",   [] { return metallic (0.8, 5, 5000, 7500, 306); });
            add ("Toms", "Tom Sub",     [] { return tom (80, 45, 6, 1.0); });
            add ("Toms", "Tom Tight",   [] { return tom (170, 110, 14, 0.4); });
            add ("Toms", "Tom Rumble",  [] { return tom (120, 60, 5, 1.0); });
            add ("Toms", "Tom Syn",     [] { return tom (260, 90, 18, 0.5); });
            add ("Cymbals", "Ride Tight", [] { return cymbal (1.2, 3.0, 3000, 9000, 311, 0.3); }, 0.0, 0.3, 0.5);
            add ("Cymbals", "Crash Dark", [] { return cymbal (3.5, 1.1, 1400, 8000, 312); }, 0.0, 0.3, 0.7);
            add ("Cymbals", "Crash Bright", [] { return cymbal (2.4, 1.6, 2800, 13000, 313); }, 0.0, 0.3, 0.7);
            add ("Percussion", "Conga High",   [] { return tone (400, 300, 14, 0.4, 0.0); });
            add ("Percussion", "Shaker Soft",  [] { return shaker (0.2, 0.02, 18); });
            add ("Percussion", "Click",        [] { return tone (4000, 3800, 200, 0.03, 0.0); });
            add ("Percussion", "Wood Hit",     [] { return tone (700, 650, 40, 0.18, 0.4); });

            // chords as one-shots, in every key: minor 7, major 7 and dominant 7
            for (int r = 0; r < 12; ++r)
            {
                add ("Chords", juce::String ("Min7 ") + noteNames[r], [r] { return chordShot (48 + r, 0, 1.2); });
                add ("Chords", juce::String ("Maj7 ") + noteNames[r], [r] { return chordShot (48 + r, 1, 1.2); });
                add ("Chords", juce::String ("Dom7 ") + noteNames[r], [r] { return chordShot (48 + r, 2, 1.2); });
            }

            // more synth, voices and effects
            add ("Synth", "Supersaw A3",   [] { return superSaw (midiHz (57), 1.5); }, 0.0, 0.3, 0.7);
            add ("Synth", "Supersaw C4",   [] { return superSaw (midiHz (60), 1.5); }, 0.0, 0.3, 0.7);
            add ("Synth", "Supersaw E4",   [] { return superSaw (midiHz (64), 1.5); }, 0.0, 0.3, 0.7);
            add ("Synth", "Sine Lead A4",  [] { return sineLead (midiHz (69), 1.5); });
            add ("Synth", "Sine Lead E4",  [] { return sineLead (midiHz (64), 1.5); });
            add ("Synth", "Pad Detuned Am", [] { auto s = make (5.0); for (int note : { 45, 57, 60, 64 }) saw (s, 0, midiHz (note), 5.0, 2000, 1500, 1.0, 1.5, 0.35f, 0.04, 6); finish (s); return s; }, 0.0, 0.3, 0.7);
            add ("Synth", "Pad Detuned C",  [] { auto s = make (5.0); for (int note : { 48, 55, 60, 64 }) saw (s, 0, midiHz (note), 5.0, 2000, 1500, 1.0, 1.5, 0.35f, 0.04, 6); finish (s); return s; }, 0.0, 0.3, 0.7);
            add ("Synth", "Pluck Poly C",   [] { auto s = make (1.2); for (int note : { 60, 64, 67 }) saw (s, 0, midiHz (note), 1.2, 6000, 500, 0.001, 0.2, 0.6f, 0.006, 2); finish (s); return s; });
            add ("Synth", "Pluck Poly Am",  [] { auto s = make (1.2); for (int note : { 57, 60, 64 }) saw (s, 0, midiHz (note), 1.2, 6000, 500, 0.001, 0.2, 0.6f, 0.006, 2); finish (s); return s; });
            for (int note : { 57, 60, 64, 67 })
            {
                const juce::String nm = juce::String (noteNames[note % 12]) + juce::String (note / 12 - 1);
                add ("Vocal", "Chop Ah " + nm, [note] { return vox (midiHz (note), 0.4, 800, 1150, 2900, 0.01); });
                add ("Vocal", "Chop Oh " + nm, [note] { return vox (midiHz (note), 0.4, 450, 800, 2830, 0.01); });
                add ("Vocal", "Chop Ee " + nm, [note] { return vox (midiHz (note), 0.4, 400, 1700, 2600, 0.01); });
            }
            add ("FX", "Riser 16s",     [] { return riser (16.0, true); });
            add ("FX", "Riser 1s",      [] { return riser (1.0, true); });
            add ("FX", "Sub Drop",      [] { return subDrop (2.0); });
            add ("FX", "Snare Roll 2s", [] { return snareRoll (2.0); });
            add ("FX", "Snare Roll 4s", [] { return snareRoll (4.0); });
            add ("FX", "Siren",         [] { return siren (3.0, 600, 1400, 0.7); });
            add ("FX", "Alarm",         [] { return siren (2.0, 800, 1000, 3.5); });
            add ("FX", "Zap",           [] { return laser (0.3, true); });
            add ("Textures", "Pink Noise 4s",  [] { return colouredNoise (4.0, 900.0, 401); });
            add ("Textures", "White Noise 4s", [] { return colouredNoise (4.0, 15000.0, 402); });
            add ("Textures", "Brown Noise 4s", [] { return colouredNoise (4.0, 200.0, 403); });

            // drum loops: every genre at several tempos, in two grooves
            {
                struct Genre { const char* name; std::vector<int> tempos; std::vector<Groove> grooves; };
                const std::vector<Genre> genres = {
                    { "Hip-hop", { 80, 85, 90, 95, 100 }, { { "X.......X.X.....", "....X.......X..X", "x.x.x.x.x.x.x.x.", "................", 0.12 }, { "X..x....X.X..x..", "....X.......X...", "x.xxx.xxx.xxx.xx", "..............x.", 0.08 } } },
                    { "House",   { 120, 124, 128 },        { { "X...X...X...X...", "....X.......X...", "..x...x...x...x.", "..............x.", 0.0 }, { "X...X...X...X...", "....X.......X...", "x.x.x.x.x.x.x.x.", "..x...x...x...x.", 0.0 } } },
                    { "Techno",  { 125, 130, 135, 140 },   { { "X...X...X...X...", "............X...", "..x...x...x...x.", ".x.x.x.x.x.x.x.x", 0.0 }, { "X...X...X...X.x.", "....X.......X...", "x.x.x.x.x.x.x.x.", "................", 0.0 } } },
                    { "Trap",    { 130, 140, 150, 160 },   { { "X.....x...X.....", "........X.......", "x.x.xxxxx.x.x.xx", "..............x.", 0.0 }, { "X.x...x.X.....x.", "........X.......", "x.x.x.x.xxx.xxxx", "................", 0.0 } } },
                    { "Drum and Bass", { 170, 174, 178 },   { { "X.........x.....", "....X.......X...", "x.x.x.x.x.x.x.x.", "..............x.", 0.0 }, { "X.....x...X.....", "....X.......X..x", "x.xxx.x.x.xxx.x.", "................", 0.0 } } },
                    { "Lo-fi",    { 70, 75, 80, 85 },       { { "X.......x.X.....", "....X.......X...", "x...x...x...x...", "................", 0.18 }, { "X.....x.x.......", "....X.......X..x", "x.x.x.x.x.x.x.x.", "..............x.", 0.15 } } },
                    { "Drill",   { 140, 144 },             { { "X.......X..x....", "........X.......", "x.x.x.x.x.xxx.xx", "................", 0.0 }, { "X.....x...x.....", "........X......x", "x.xxx.x.xxx.x.xx", "................", 0.0 } } },
                    { "Garage",  { 130, 132 },             { { "X.......X.......", "....X.......X..x", "x.x.x.x.x.x.x.x.", "..x.......x.....", 0.2 }, { "X.....x.X.......", "....X.......X...", ".x.x.x.x.x.x.x.x", "x...............", 0.2 } } } };
                for (auto& g : genres)
                    for (size_t gi = 0; gi < g.grooves.size(); ++gi)
                        for (int bpm : g.tempos)
                        {
                            const Groove groove = g.grooves[gi];
                            const juce::String genre = g.name;
                            add ("Loops", genre + (gi == 0 ? " A " : " B ") + juce::String (bpm) + " BPM", [genre, groove, bpm]
                            {
                                Sig k, sn, h, o;
                                if (genre == "Hip-hop")       { k = kick (130, 50, 25, 6, 0.5, 1.4, 0.1);  sn = snare (190, 16, 12, 0.35, 1400, 1.0); h = metallic (0.1, 60, 7000, 10000, 71); o = metallic (0.5, 8, 6500, 9000, 74); }
                                else if (genre == "House")    { k = kick (125, 45, 28, 5.5, 0.5, 1.2, 0.12); sn = clap (4, 0.012, 16, 0.4, 1300);       h = metallic (0.05, 130, 9500, 12500, 80); o = metallic (0.4, 10, 7500, 10500, 83); }
                                else if (genre == "Techno")   { k = kick (140, 48, 40, 6.5, 0.45, 1.8, 0.2); sn = clap (3, 0.01, 26, 0.25, 1600);        h = metallic (0.05, 130, 9500, 12500, 80); o = metallic (0.4, 10, 7500, 10500, 83); }
                                else if (genre == "Trap")     { k = kick (85, 40, 10, 1.6, 1.0, 1.5, 0.05); sn = snare (250, 32, 22, 0.28, 2800, 1.15);  h = metallic (0.07, 95, 9000, 12000, 79);  o = metallic (0.4, 10, 7500, 10500, 83); }
                                else if (genre == "Drum and Bass") { k = kick (120, 48, 30, 5, 0.5, 1.4, 0.2); sn = snare (280, 35, 24, 0.25, 3200, 1.2); h = metallic (0.06, 110, 9000, 12000, 80); o = metallic (0.35, 12, 7500, 10500, 83); }
                                else if (genre == "Lo-fi")    { k = kick (115, 50, 18, 8, 0.45, 0.9, 0.02); sn = snare (180, 18, 13, 0.4, 900, 0.9, 5);  h = metallic (0.14, 40, 5500, 8000, 73);   o = metallic (0.5, 8, 6500, 9000, 74); }
                                else if (genre == "Drill")    { k = kick (90, 42, 12, 2, 1.2, 1.4, 0.05);   sn = snare (240, 30, 20, 0.3, 2600, 1.1);   h = metallic (0.06, 100, 9000, 12000, 79); o = metallic (0.4, 10, 7500, 10500, 83); }
                                else                          { k = kick (125, 45, 28, 5.5, 0.5, 1.2, 0.12); sn = clap (4, 0.012, 16, 0.4, 1300);       h = metallic (0.05, 130, 9500, 12500, 80); o = metallic (0.4, 10, 7500, 10500, 83); }
                                return drumLoop (bpm, groove.kick, groove.snare, groove.hat, groove.open, k, sn, h, o, groove.swing);
                            });
                        }
            }
            // top loops and fills
            add ("Loops", "Hat Loop 8th 100 BPM",   [] { return drumLoop (100, "................", "................", "x.x.x.x.x.x.x.x.", "................", kick (130, 50, 25, 6, 0.5, 1.4, 0.1), snare (190, 16, 12, 0.35, 1400, 1.0), metallic (0.08, 85, 8500, 11500, 301), metallic (0.5, 8, 6500, 9000, 74), 0.1); });
            add ("Loops", "Hat Loop 16th 130 BPM",  [] { return drumLoop (130, "................", "................", "xxxxxxxxxxxxxxxx", "................", kick (130, 50, 25, 6, 0.5, 1.4, 0.1), snare (190, 16, 12, 0.35, 1400, 1.0), metallic (0.06, 110, 10000, 13000, 304), metallic (0.5, 8, 6500, 9000, 74), 0.0); });
            add ("Loops", "Hat Roll 140 BPM",       [] { return drumLoop (140, "................", "................", "x.x.x.x.xxxxxxxx", "................", kick (130, 50, 25, 6, 0.5, 1.4, 0.1), snare (190, 16, 12, 0.35, 1400, 1.0), metallic (0.06, 110, 10000, 13000, 304), metallic (0.5, 8, 6500, 9000, 74), 0.0); });
            add ("Loops", "Offbeat Hat 124 BPM",    [] { return drumLoop (124, "................", "................", "................", "..x...x...x...x.", kick (130, 50, 25, 6, 0.5, 1.4, 0.1), snare (190, 16, 12, 0.35, 1400, 1.0), metallic (0.06, 110, 10000, 13000, 304), metallic (0.4, 10, 7500, 10500, 83), 0.0); });
            add ("Loops", "Shaker Loop 100 BPM",    [] { return drumLoop (100, "................", "................", "x.x.x.x.x.x.x.x.", "................", kick (130, 50, 25, 6, 0.5, 1.4, 0.1), snare (190, 16, 12, 0.35, 1400, 1.0), shaker (0.08, 0.004, 40), metallic (0.5, 8, 6500, 9000, 74), 0.1); });
            add ("Loops", "Shaker Loop 120 BPM",    [] { return drumLoop (120, "................", "................", "xxxxxxxxxxxxxxxx", "................", kick (130, 50, 25, 6, 0.5, 1.4, 0.1), snare (190, 16, 12, 0.35, 1400, 1.0), shaker (0.07, 0.004, 45), metallic (0.5, 8, 6500, 9000, 74), 0.0); });
            add ("Loops", "Tambourine Loop 110 BPM", [] { return drumLoop (110, "................", "................", "..x...x...x...x.", "................", kick (130, 50, 25, 6, 0.5, 1.4, 0.1), snare (190, 16, 12, 0.35, 1400, 1.0), metallic (0.35, 14, 6000, 9000, 91, 0.004), metallic (0.5, 8, 6500, 9000, 74), 0.05); });
            add ("Loops", "Conga Loop 95 BPM",      [] { return drumLoop (95, "x..x..x...x.x...", "..x...x..x......", "................", "................", tone (340, 220, 14, 0.4, 0.0), tone (400, 300, 14, 0.4, 0.0), shaker (0.08, 0.004, 40), shaker (0.08, 0.004, 40), 0.05); });
            add ("Loops", "Bongo Loop 100 BPM",     [] { return drumLoop (100, "x.x..x.x.x..x...", "..x.x...x...x.x.", "................", "................", tone (320, 250, 15, 0.35, 0.0), tone (480, 380, 18, 0.3, 0.0), shaker (0.08, 0.004, 40), shaker (0.08, 0.004, 40), 0.05); });
            add ("Loops", "Clap Loop 125 BPM",      [] { return drumLoop (125, "................", "..x...x...x...x.", "................", "................", kick (130, 50, 25, 6, 0.5, 1.4, 0.1), clap (4, 0.012, 16, 0.4, 1300), shaker (0.08, 0.004, 40), shaker (0.08, 0.004, 40), 0.0); });
            add ("Loops", "Snare Fill 90 BPM",      [] { return fillBar (90, "........x.x.xxXX", "................", snare (190, 16, 12, 0.35, 1400, 1.0), tom (130, 70, 8, 0.7)); });
            add ("Loops", "Snare Fill 120 BPM",     [] { return fillBar (120, "........x.x.xxXX", "................", snare (200, 20, 15, 0.3, 1500, 1.0), tom (130, 70, 8, 0.7)); });
            add ("Loops", "Tom Fill 100 BPM",        [] { return fillBar (100, "............X...", "X.x.x.X.X.x.xxX.", snare (190, 16, 12, 0.35, 1400, 1.0), tom (150, 80, 8, 0.7)); });
            add ("Loops", "Tom Fill 130 BPM",        [] { return fillBar (130, "............X...", "X.x.x.X.X.x.xxX.", snare (200, 20, 15, 0.3, 1500, 1.0), tom (170, 100, 9, 0.6)); });

            // bass loops in six keys, chords in six keys, arpeggios in six keys
            {
                const int roots[6] = { 33, 36, 38, 40, 41, 43 };   // A1, C2, D2, E2, F2, G2
                const char* rootNames[6] = { "Am", "Cm", "Dm", "Em", "Fm", "Gm" };
                for (int i = 0; i < 6; ++i)
                {
                    const int r = roots[i];
                    const juce::String name = rootNames[i];
                    add ("Loops", "Bass Loop " + name + " A 100 BPM", [r] { return bassLoopV (100, { { r, 3 }, { 0, 1 }, { r, 2 }, { r + 3, 2 }, { r + 5, 3 }, { 0, 1 }, { r + 3, 2 }, { r - 2, 2 } }, 900); });
                    add ("Loops", "Bass Loop " + name + " B 130 BPM", [r] { return bassLoopV (130, { { r, 2 }, { r, 2 }, { 0, 2 }, { r + 7, 2 }, { r, 2 }, { r + 3, 2 }, { 0, 2 }, { r - 2, 2 } }, 1200); });
                    const int c = r + 24;   // chords an octave and a half higher
                    add ("Loops", "Chords " + name + " VI III VII 90 BPM", [c] { return chordLoopV (90, { { c, c + 3, c + 7 }, { c + 8, c + 12, c + 15 }, { c + 3, c + 7, c + 10 }, { c + 10, c + 14, c + 17 } }, 16, 1800, 0.01); });
                    add ("Loops", "Chords " + name + " iv VII III 100 BPM", [c] { return chordLoopV (100, { { c, c + 3, c + 7 }, { c + 5, c + 8, c + 12 }, { c + 10, c + 14, c + 17 }, { c + 3, c + 7, c + 10 } }, 16, 1600, 0.02); });
                    add ("Loops", "Arp " + name + " B 124 BPM", [c] { return arpLoop (124, { c, c + 3, c + 7, c + 12, c + 7, c + 3 }, 3000); });
                    add ("Loops", "Arp " + name + " C 132 BPM", [c] { return arpLoop (132, { c + 12, c + 7, c + 3, c, c + 3, c + 7, c + 10, c + 7 }, 2800); });
                }
            }
            return e;
        }();
        return list;
    }

    bool writeWavStereo (const juce::File& file, const Stereo& s)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
        if (stream == nullptr)
            return false;
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), rate, 2, 16, {}, 0));
        if (writer == nullptr)
            return false;
        stream.release();
        juce::AudioBuffer<float> buffer (2, (int) s.l.size());
        std::copy (s.l.begin(), s.l.end(), buffer.getWritePointer (0));
        std::copy (s.r.begin(), s.r.end(), buffer.getWritePointer (1));
        return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    bool writeWav (const juce::File& file, const Sig& s)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
        if (stream == nullptr)
            return false;
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), rate, 1, 16, {}, 0));
        if (writer == nullptr)
            return false;
        stream.release();
        juce::AudioBuffer<float> buffer (1, (int) s.size());
        std::copy (s.begin(), s.end(), buffer.getWritePointer (0));
        return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }
}

namespace StarterPack
{
    juce::File folder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("UFODAW").getChildFile ("StarterPack");
    }

    static juce::File marker() { return folder().getChildFile (".version"); }

    bool isReady()
    {
        return marker().existsAsFile() && marker().loadFileAsString().trim().getIntValue() == packVersion;
    }

    void ensureInstalled()
    {
        if (isReady())
            return;
        // (a pack of an older version is written again completely; a run of this version that was cut short carries on where it stopped)
        const bool resuming = marker().loadFileAsString().trim() == "writing " + juce::String (packVersion);
        if (! resuming)
            marker().replaceWithText ("writing " + juce::String (packVersion));
        for (auto& entry : catalog())
        {
            const auto file = sample (entry.category, entry.name);
            if (resuming && file.existsAsFile())
                continue;
            auto signal = entry.make();
            if (entry.room > 0.0 || entry.wide > 0.0)
            {
                auto stereo = entry.room > 0.0 ? roomify (signal, entry.room, entry.roomDecay, 1.0, entry.wide > 0.0 ? entry.wide : 0.8) : widen (signal, entry.wide);
                finishStereo (stereo);
                writeWavStereo (file, stereo);
            }
            else
                writeWav (file, signal);
        }
        marker().replaceWithText (juce::String (packVersion));
    }

    juce::File sample (const juce::String& category, const juce::String& name)
    {
        return folder().getChildFile (category).getChildFile (name + ".wav");
    }

    const std::vector<Kit>& kits()
    {
        static const std::vector<Kit> list = []
        {
            auto P = [] (const char* c, const char* n) { return std::make_pair (juce::String (c), juce::String (n)); };
            std::vector<Kit> k;
            // pads: kick, snare, clap, closed hat, open hat, low tom, high tom, crash, then rim, shaker, cowbell, tambourine, mid tom, ride, block, conga
            k.push_back ({ "Punch", "A tight, modern kit: a kick with a click, a short snare, a bright hat",
                           { P ("Kicks", "Kick Punch"), P ("Snares", "Snare Tight"), P ("Claps", "Clap Tight"), P ("Hats", "Hat Closed 1"), P ("Hats", "Hat Open 1"),
                             P ("Toms", "Tom Low"), P ("Toms", "Tom High"), P ("Cymbals", "Crash"), P ("Snares", "Rim Shot"), P ("Percussion", "Shaker"),
                             P ("Percussion", "Cowbell"), P ("Percussion", "Tambourine"), P ("Toms", "Tom Mid"), P ("Cymbals", "Ride"), P ("Percussion", "Block"), P ("Percussion", "Conga") } });
            k.push_back ({ "808", "A long sub kick, a snappy snare and wide claps for trap and hip-hop",
                           { P ("Kicks", "Kick 808"), P ("Snares", "Snare Snap"), P ("Claps", "Clap Wide"), P ("Hats", "Hat Closed 2"), P ("Hats", "Hat Open 2"),
                             P ("Toms", "Tom Low"), P ("Toms", "Tom High"), P ("Cymbals", "Crash"), P ("Snares", "Rim Shot"), P ("Percussion", "Shaker"),
                             P ("Percussion", "Cowbell"), P ("Percussion", "Tambourine"), P ("Toms", "Tom Mid"), P ("Cymbals", "Ride"), P ("Percussion", "Block"), P ("Percussion", "Conga") } });
            k.push_back ({ "Lo-fi", "Soft kick, crunchy snare and a dull hat: dusty beats",
                           { P ("Kicks", "Kick Soft"), P ("Snares", "Snare Lofi"), P ("Claps", "Clap Tight"), P ("Hats", "Hat Closed 3"), P ("Hats", "Hat Open 1"),
                             P ("Toms", "Tom Low"), P ("Toms", "Tom Mid"), P ("Cymbals", "Ride"), P ("Snares", "Rim Shot"), P ("Percussion", "Shaker"),
                             P ("Percussion", "Block"), P ("Percussion", "Tambourine"), P ("Toms", "Tom High"), P ("Cymbals", "Crash"), P ("Percussion", "Conga"), P ("Percussion", "Cowbell") } });
            k.push_back ({ "House", "A deep kick, a big clap and open hats for four on the floor",
                           { P ("Kicks", "Kick Deep"), P ("Snares", "Snare Tight"), P ("Claps", "Clap Wide"), P ("Hats", "Hat Closed 1"), P ("Hats", "Hat Open 2"),
                             P ("Toms", "Tom Low"), P ("Toms", "Tom High"), P ("Cymbals", "Crash"), P ("Snares", "Rim Shot"), P ("Percussion", "Shaker"),
                             P ("Percussion", "Cowbell"), P ("Percussion", "Tambourine"), P ("Toms", "Tom Mid"), P ("Cymbals", "Ride"), P ("Percussion", "Block"), P ("Percussion", "Conga") } });
            k.push_back ({ "Tight", "Short and dry: a clicky kick and a rimshot snare, for fast patterns",
                           { P ("Kicks", "Kick Click"), P ("Snares", "Rim Shot"), P ("Claps", "Clap Tight"), P ("Hats", "Hat Closed 2"), P ("Hats", "Hat Open 1"),
                             P ("Toms", "Tom Mid"), P ("Toms", "Tom High"), P ("Cymbals", "Crash"), P ("Snares", "Snare Snap"), P ("Percussion", "Shaker"),
                             P ("Percussion", "Cowbell"), P ("Percussion", "Tambourine"), P ("Toms", "Tom Low"), P ("Cymbals", "Ride"), P ("Percussion", "Block"), P ("Percussion", "Conga") } });
            k.push_back ({ "909", "Electronic: a punchy kick, a two-tone snare, crisp hats and a clap",
                           { P ("Kicks", "Kick 909"), P ("Snares", "Snare 909"), P ("Claps", "Clap Tight"), P ("Hats", "Hat Closed 909"), P ("Hats", "Hat Open 909"),
                             P ("Toms", "Tom Low"), P ("Toms", "Tom High"), P ("Cymbals", "Crash"), P ("Snares", "Rim Click"), P ("Percussion", "Shaker"),
                             P ("Percussion", "Cowbell"), P ("Percussion", "Tambourine"), P ("Toms", "Tom Mid"), P ("Cymbals", "Ride"), P ("Percussion", "Clave"), P ("Percussion", "Conga") } });
            k.push_back ({ "Acoustic", "A drum kit in a small room: membrane toms, a rattling snare, cymbals with shimmer",
                           { P ("Kicks", "Kick Acoustic"), P ("Snares", "Snare Room"), P ("Claps", "Clap Room"), P ("Hats", "Hat Closed 2"), P ("Hats", "Hat Open 1"),
                             P ("Toms", "Tom Floor"), P ("Toms", "Tom Rack"), P ("Cymbals", "Crash"), P ("Snares", "Rim Shot"), P ("Percussion", "Shaker"),
                             P ("Percussion", "Triangle"), P ("Percussion", "Tambourine"), P ("Toms", "Tom Mid"), P ("Cymbals", "Ride"), P ("Percussion", "Snap"), P ("Snares", "Snare Brush") } });
            k.push_back ({ "Trap Hard", "Hard 808s: a long sub kick, a cracking snare, big claps and fast tick hats",
                           { P ("Kicks", "Kick Trap"), P ("Snares", "Snare Trap"), P ("Claps", "Clap Big"), P ("Hats", "Hat Closed 4"), P ("Hats", "Hat Open 3"),
                             P ("Toms", "Tom Deep"), P ("Toms", "Tom Electro"), P ("Cymbals", "Crash 2"), P ("Snares", "Rim Soft"), P ("Percussion", "Maracas"),
                             P ("Percussion", "Cowbell"), P ("Percussion", "Tambourine"), P ("Toms", "Tom Mid"), P ("Cymbals", "Ride Bell"), P ("Percussion", "Woodblock"), P ("Percussion", "Bongo Low") } });
            k.push_back ({ "Boom Bap", "Dusty 90s hip-hop: a round kick, a gritty snare and a loose hat",
                           { P ("Kicks", "Kick Thump"), P ("Snares", "Snare Dust"), P ("Claps", "Clap Lofi"), P ("Hats", "Hat Closed 3"), P ("Hats", "Hat Open 1"),
                             P ("Toms", "Tom Low"), P ("Toms", "Tom Mid"), P ("Cymbals", "Crash"), P ("Snares", "Rim Click"), P ("Percussion", "Shaker"),
                             P ("Percussion", "Cowbell"), P ("Percussion", "Tambourine"), P ("Toms", "Tom High"), P ("Cymbals", "Ride"), P ("Percussion", "Woodblock"), P ("Percussion", "Conga Low") } });
            k.push_back ({ "Techno", "Hard, dry and fast: a driving kick, a cracking snare and tick hats",
                           { P ("Kicks", "Kick Techno"), P ("Snares", "Snare Crack"), P ("Claps", "Clap Snappy"), P ("Hats", "Hat Closed Tick"), P ("Hats", "Hat Open Long"),
                             P ("Toms", "Tom Electro"), P ("Toms", "Tom Deep"), P ("Cymbals", "Crash 2"), P ("Snares", "Rim Click"), P ("Percussion", "Maracas"),
                             P ("Percussion", "Perc Metal"), P ("Percussion", "Tambourine"), P ("Toms", "Tom Mid"), P ("Cymbals", "Ride Bell"), P ("Percussion", "Clave"), P ("Percussion", "Bongo High") } });
            k.push_back ({ "Rock", "A band in a room: a solid kick, a loud snare, open hats and a splash",
                           { P ("Kicks", "Kick Thump"), P ("Snares", "Snare Rock"), P ("Claps", "Clap Room"), P ("Hats", "Hat Closed 2"), P ("Hats", "Hat Open 2"),
                             P ("Toms", "Tom Floor"), P ("Toms", "Tom Rack"), P ("Cymbals", "Crash 2"), P ("Snares", "Rim Shot"), P ("Percussion", "Shaker"),
                             P ("Percussion", "Cowbell"), P ("Percussion", "Tambourine"), P ("Toms", "Tom Mid"), P ("Cymbals", "China"), P ("Cymbals", "Splash"), P ("Snares", "Snare Piccolo") } });
            k.push_back ({ "Vintage 707", "An old drum machine: a short kick, a snappy snare, square-wave hats and a cowbell",
                           { P ("Kicks", "Kick 707"), P ("Snares", "Snare 707"), P ("Claps", "Clap 808"), P ("Hats", "Hat 808 Closed"), P ("Hats", "Hat 808 Open"),
                             P ("Toms", "Tom 808 Low"), P ("Toms", "Tom 808 High"), P ("Cymbals", "Crash"), P ("Snares", "Rim Click"), P ("Percussion", "Maracas"),
                             P ("Percussion", "Cowbell 808"), P ("Percussion", "Tambourine"), P ("Toms", "Tom 808 Mid"), P ("Cymbals", "Ride"), P ("Percussion", "Clave"), P ("Percussion", "Conga 808") } });
            k.push_back ({ "Disco Funk", "Four on the floor with a handclap, open hats and a bit of percussion",
                           { P ("Kicks", "Kick Disco"), P ("Snares", "Snare Disco"), P ("Claps", "Clap Disco"), P ("Hats", "Hat Closed 909"), P ("Hats", "Hat Open 909"),
                             P ("Toms", "Tom Rack"), P ("Toms", "Tom Floor"), P ("Cymbals", "Crash 2"), P ("Snares", "Rim Shot"), P ("Percussion", "Shaker Tight"),
                             P ("Percussion", "Cowbell 808"), P ("Percussion", "Tambourine"), P ("Toms", "Tom 808 Mid"), P ("Cymbals", "Ride"), P ("Percussion", "Clave"), P ("Percussion", "Agogo High") } });
            k.push_back ({ "World Percussion", "Hand drums and small percussion: djembe, tabla, timbales, guiro and more",
                           { P ("Percussion", "Djembe Bass"), P ("Percussion", "Djembe Slap"), P ("Percussion", "Tabla High"), P ("Percussion", "Timbale High"), P ("Percussion", "Timbale Low"),
                             P ("Percussion", "Tabla Low"), P ("Percussion", "Conga Low"), P ("Percussion", "Bongo High"), P ("Percussion", "Guiro"), P ("Percussion", "Shaker Long"),
                             P ("Percussion", "Cabasa"), P ("Percussion", "Castanet"), P ("Percussion", "Agogo Low"), P ("Percussion", "Triangle"), P ("Percussion", "Whistle"), P ("Percussion", "Frame Drum") } });
            return k;
        }();
        return list;
    }

    int loopTempo (const juce::String& name)
    {
        const int at = name.indexOf (" BPM");
        if (at < 0)
            return 0;
        int start = at;
        while (start > 0 && juce::CharacterFunctions::isDigit (name[start - 1]))
            --start;
        return name.substring (start, at).getIntValue();
    }
}

namespace StarterPack
{
    DrumVariants variantsOf (const juce::String&, const juce::String& name)
    {
        DrumVariants v;
        const auto soft = sample ("Variations", name + " soft");
        if (soft.existsAsFile())
            v.soft = soft;
        for (auto* suffix : { " b", " c" })
            if (const auto f = sample ("Variations", name + suffix); f.existsAsFile())
                v.roundRobin.push_back (f);
        return v;
    }
}
