#include "StarterPack.h"
#include <cmath>
#include <functional>
#include <map>

namespace
{
    using Sig = std::vector<float>;
    constexpr double rate = 44100.0;
    constexpr double twoPi = 6.283185307179586;
    constexpr int packVersion = 3;

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
    struct Entry { const char* category; juce::String name; std::function<Sig()> make; double room = 0.0, roomDecay = 0.3, wide = 0.0; };   // (room / wide: a stereo sound)

    const std::vector<Entry>& catalog()
    {
        static const std::vector<Entry> list = []
        {
            std::vector<Entry> e;
            auto add = [&] (const char* c, const char* name, std::function<Sig()> f, double room = 0.0, double roomDecay = 0.3, double wide = 0.0) { e.push_back ({ c, name, std::move (f), room, roomDecay, wide }); };

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
