#include "StarterPack.h"
#include <cmath>
#include <functional>
#include <map>

namespace
{
    using Sig = std::vector<float>;
    constexpr double rate = 44100.0;
    constexpr double twoPi = 6.283185307179586;
    constexpr int packVersion = 1;

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
    // drums
    Sig kick (double f0, double f1, double sweepRate, double decay, double len, double drive, double click)
    {
        auto s = make (len);
        double phase = 0.0;
        juce::Random rng (5);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            phase += twoPi * (f1 + (f0 - f1) * std::exp (-t * sweepRate)) / rate;
            double v = std::tanh (drive * std::sin (phase)) * std::exp (-t * decay);
            v += click * (rng.nextFloat() * 2.0 - 1.0) * std::exp (-t * 600.0);
            s[i] = (float) v;
        }
        finish (s);
        return s;
    }

    Sig snare (double bodyHz, double bodyDecay, double noiseDecay, double len, double noiseHp, double noiseMix, int crushBits = 0)
    {
        auto s = make (len);
        auto nz = filtered (noise (len, 11), 1, noiseHp, 0.7);
        double phase = 0.0;
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            phase += twoPi * (bodyHz * (1.0 + 0.5 * std::exp (-t * 60.0))) / rate;
            s[i] = (float) (std::sin (phase) * std::exp (-t * bodyDecay) * 0.8 + nz[i] * std::exp (-t * noiseDecay) * noiseMix);
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

    Sig clap (int bursts, double spacing, double tailDecay, double len, double bpHz)
    {
        auto nz = filtered (noise (len, 21), 2, bpHz, 1.1, 2);
        auto s = make (len);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            double e = std::exp (-t * tailDecay);
            for (int b = 0; b < bursts - 1; ++b)
            {
                const double d = t - b * spacing;
                if (d >= 0.0 && d < spacing)
                    e = juce::jmax (e, std::exp (-d * 400.0) * 0.9);
            }
            s[i] = nz[i] * (float) e;
        }
        finish (s);
        return s;
    }

    Sig metallic (double seconds, double decay, double hp, double bp, int seed, double attack = 0.0)
    {
        static const double freqs[6] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };   // the classic 808 hat oscillators
        auto s = make (seconds);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            double v = 0.0;
            for (double f : freqs) v += std::sin (twoPi * f * 7.0 * t) > 0.0 ? 1.0 : -1.0;
            s[i] = (float) (v / 6.0);
        }
        auto nz = noise (seconds, seed);
        for (size_t i = 0; i < n (s); ++i) s[i] = s[i] * 0.7f + nz[i] * 0.3f;
        s = filtered (filtered (s, 2, bp, 1.2), 1, hp, 0.7, 2);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            s[i] *= (float) (std::exp (-t * decay) * (attack > 0.0 ? juce::jmin (1.0, t / attack) : 1.0));
        }
        finish (s);
        return s;
    }

    Sig tom (double f0, double f1, double decay, double len)
    {
        auto s = make (len);
        double phase = 0.0;
        juce::Random rng (3);
        for (size_t i = 0; i < n (s); ++i)
        {
            const double t = (double) i / rate;
            phase += twoPi * (f1 + (f0 - f1) * std::exp (-t * 25.0)) / rate;
            s[i] = (float) ((std::sin (phase) + 0.15 * (rng.nextFloat() * 2.0 - 1.0) * std::exp (-t * 120.0)) * std::exp (-t * decay));
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
    struct Entry { const char* category; juce::String name; std::function<Sig()> make; };

    const std::vector<Entry>& catalog()
    {
        static const std::vector<Entry> list = []
        {
            std::vector<Entry> e;
            auto add = [&] (const char* c, const char* name, std::function<Sig()> f) { e.push_back ({ c, name, std::move (f) }); };

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

            add ("Cymbals", "Crash", [] { return metallic (2.2, 1.8, 4500, 7000, 81, 0.002); });
            add ("Cymbals", "Ride",  [] { return metallic (1.4, 3.0, 5000, 7500, 82, 0.001); });

            add ("Percussion", "Shaker",     [] { return shaker (0.18, 0.012, 22); });
            add ("Percussion", "Tambourine", [] { return metallic (0.35, 14, 6000, 9000, 91, 0.004); });
            add ("Percussion", "Cowbell",    [] { return tone (560, 540, 12, 0.5, 0.9); });
            add ("Percussion", "Block",      [] { return tone (900, 850, 60, 0.12, 0.0); });
            add ("Percussion", "Conga",      [] { return tone (340, 220, 14, 0.4, 0.0); });

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
        for (auto& entry : catalog())
        {
            const auto file = sample (entry.category, entry.name);
            if (file.existsAsFile())
                continue;   // (a half-finished earlier run: carry on where it stopped)
            writeWav (file, entry.make());
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
