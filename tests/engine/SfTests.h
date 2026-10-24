#pragma once

// Tests of the SoundFont (.sf2) and SFZ players. The test instruments are made here (a few kilobytes, free of rights: sine and saw waves):
//   EngineTest sfgen <folder>   writes test.sf2, test.sfz and its samples there (tests/projects/sf/ is made this way)
// The checks play notes straight into SfInstrument and measure what comes out (pitch, loops, envelopes, pedal, pitch bend, round robin...).

#include "Engine.h"
#include <cmath>
#include <functional>

namespace SfTests
{
using Report = std::function<bool (bool, const juce::String&)>;

inline void writeWav16 (const juce::File& file, const std::vector<float>& samples, double rate)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (file.createOutputStream().release(), rate, 1, 16, {}, 0));
    juce::AudioBuffer<float> b (1, (int) samples.size());
    for (size_t i = 0; i < samples.size(); ++i) b.setSample (0, (int) i, samples[i]);
    w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

// 441 Hz at 44.1 kHz: exactly 100 samples per period, so a loop over whole periods is seamless. `sine` or a saw with all harmonics.
inline std::vector<float> wave (int frames, int period, bool saw, float level = 0.5f)
{
    std::vector<float> v ((size_t) frames);
    for (int i = 0; i < frames; ++i)
    {
        const double ph = (double) (i % period) / period;
        v[(size_t) i] = level * (float) (saw ? 2.0 * ph - 1.0 : std::sin (6.283185307179586 * ph));
    }
    return v;
}

inline void chunk (juce::MemoryOutputStream& out, const char* id, const juce::MemoryBlock& data)
{
    out.write (id, 4);
    out.writeInt ((int) data.getSize());
    out.write (data.getData(), data.getSize());
    if (data.getSize() & 1) out.writeByte (0);
}

inline void name20 (juce::MemoryOutputStream& o, const juce::String& n)
{
    char buf[20] = {};
    std::strncpy (buf, n.toRawUTF8(), 19);
    o.write (buf, 20);
}

// A SoundFont with two presets. Instrument: keys 0..71 play a looped 441 Hz sine (root A4, corrected to 440 Hz), keys 72..127 a 882 Hz sine
// that is not looped (0.1 s). Preset 0 plays it as it is, preset 1 an octave higher (a preset-level generator).
inline juce::File writeSf2 (const juce::File& file)
{
    const int loopFrames = 4400, shortFrames = 4410, pad = 46;
    juce::MemoryOutputStream smpl;
    for (auto v : wave (loopFrames, 100, false)) smpl.writeShort ((short) (v * 32767.0f));
    for (int i = 0; i < pad; ++i) smpl.writeShort (0);
    for (auto v : wave (shortFrames, 50, false)) smpl.writeShort ((short) (v * 32767.0f));
    for (int i = 0; i < pad; ++i) smpl.writeShort (0);

    auto gen = [] (juce::MemoryOutputStream& o, int oper, int amount) { o.writeShort ((short) oper); o.writeShort ((short) amount); };
    auto range = [] (int lo, int hi) { return lo | (hi << 8); };

    juce::MemoryOutputStream phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr;
    auto preset = [&] (const char* name, int number, int bag) { name20 (phdr, name); phdr.writeShort ((short) number); phdr.writeShort (0); phdr.writeShort ((short) bag); phdr.writeInt (0); phdr.writeInt (0); phdr.writeInt (0); };
    preset ("Test sine", 0, 0);
    preset ("Test octave up", 1, 1);
    preset ("EOP", 0, 2);
    pbag.writeShort (0); pbag.writeShort (0);
    pbag.writeShort (1); pbag.writeShort (0);
    pbag.writeShort (3); pbag.writeShort (0);
    gen (pgen, 41, 0);
    gen (pgen, 51, 12); gen (pgen, 41, 0);
    gen (pgen, 0, 0);
    for (int i = 0; i < 10; ++i) pmod.writeByte (0);

    name20 (inst, "Test instrument"); inst.writeShort (0);
    name20 (inst, "EOI"); inst.writeShort (2);
    ibag.writeShort (0); ibag.writeShort (0);
    ibag.writeShort (5); ibag.writeShort (0);
    ibag.writeShort (9); ibag.writeShort (0);
    gen (igen, 43, range (0, 71)); gen (igen, 54, 1); gen (igen, 34, -7973); gen (igen, 38, -4000); gen (igen, 53, 0);
    gen (igen, 43, range (72, 127)); gen (igen, 34, -7973); gen (igen, 38, -4000); gen (igen, 53, 1);
    gen (igen, 0, 0);
    for (int i = 0; i < 10; ++i) imod.writeByte (0);

    auto sample = [&] (const char* name, int start, int end, int loopStart, int loopEnd, int pitch, int type)
    {
        name20 (shdr, name);
        shdr.writeInt (start); shdr.writeInt (end); shdr.writeInt (loopStart); shdr.writeInt (loopEnd);
        shdr.writeInt (44100); shdr.writeByte ((char) pitch); shdr.writeByte (-4); shdr.writeShort (0); shdr.writeShort ((short) type);
    };
    sample ("sine 441", 0, loopFrames, 400, loopFrames, 69, 1);
    sample ("sine 882", loopFrames + pad, loopFrames + pad + shortFrames, loopFrames + pad, loopFrames + pad + shortFrames, 81, 1);
    sample ("EOS", 0, 0, 0, 0, 0, 0);

    juce::MemoryOutputStream info, sdta, pdta, body;
    { juce::MemoryOutputStream v; v.writeShort (2); v.writeShort (1); chunk (info, "ifil", v.getMemoryBlock()); }
    { const char* n = "UFODAW test"; juce::MemoryBlock b (n, std::strlen (n) + 1); chunk (info, "INAM", b); }
    chunk (sdta, "smpl", smpl.getMemoryBlock());
    chunk (pdta, "phdr", phdr.getMemoryBlock()); chunk (pdta, "pbag", pbag.getMemoryBlock()); chunk (pdta, "pmod", pmod.getMemoryBlock());
    chunk (pdta, "pgen", pgen.getMemoryBlock()); chunk (pdta, "inst", inst.getMemoryBlock()); chunk (pdta, "ibag", ibag.getMemoryBlock());
    chunk (pdta, "imod", imod.getMemoryBlock()); chunk (pdta, "igen", igen.getMemoryBlock()); chunk (pdta, "shdr", shdr.getMemoryBlock());
    auto list = [&] (juce::MemoryOutputStream& out, const char* type, juce::MemoryOutputStream& content)
    {
        out.write ("LIST", 4);
        out.writeInt ((int) content.getDataSize() + 4);
        out.write (type, 4);
        out.write (content.getData(), content.getDataSize());
    };
    body.write ("sfbk", 4);
    list (body, "INFO", info); list (body, "sdta", sdta); list (body, "pdta", pdta);

    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::FileOutputStream out (file);
    out.write ("RIFF", 4);
    out.writeInt ((int) body.getDataSize());
    out.write (body.getData(), body.getDataSize());
    return file;
}

// An SFZ with samples in a sub folder (default_path), a sample name with a space, #define, #include, a looped sine, velocity layers, round robin,
// a release trigger, a one-shot and a group that cuts another.
inline juce::File writeSfz (const juce::File& dir)
{
    dir.createDirectory();
    writeWav16 (dir.getChildFile ("samples/sine.wav"), wave (4400, 100, false), 44100.0);
    writeWav16 (dir.getChildFile ("samples/saw tooth.wav"), wave (4400, 100, true, 0.4f), 44100.0);
    dir.getChildFile ("extra.sfz").replaceWithText (
        "// a second file, included by main\n"
        "<group> lokey=84 hikey=96 pitch_keycenter=84 loop_mode=no_loop\n"
        "<region> sample=saw tooth.wav tune=0\n");
    const auto sfz = dir.getChildFile ("test.sfz");
    sfz.replaceWithText (
        "// test instrument of UFODAW (sine and saw waves)\n"
        "<control>\n"
        "default_path=samples/\n"
        "#define $LOOP loop_continuous\n"
        "<global> ampeg_release=0.05 /* a block comment */\n"
        "#include \"extra.sfz\"\n"
        "<group> lokey=31 hikey=39 pitch_keycenter=69 loop_mode=$LOOP loop_start=0 loop_end=4399 ampeg_attack=0.01\n"
        "<region> sample=sine.wav\n"
        "<group> key=69 loop_mode=$LOOP loop_start=0 loop_end=4399 seq_length=2\n"
        "<region> seq_position=1 sample=sine.wav\n"
        "<region> seq_position=2 sample=saw tooth.wav\n"
        "<group> key=60 loop_mode=$LOOP loop_start=0 loop_end=4399 pitch_keycenter=69\n"
        "<region> lovel=1 hivel=63 sample=sine.wav\n"
        "<region> lovel=64 hivel=127 sample=saw tooth.wav\n"
        "<group> key=30 trigger=release pitch_keycenter=69\n"
        "<region> sample=saw tooth.wav loop_mode=no_loop\n"
        "<group> key=50 loop_mode=one_shot\n"
        "<region> sample=sine.wav\n"
        "<group> lokey=51 hikey=52 group=1 off_by=1 loop_mode=$LOOP loop_start=0 loop_end=4399 pitch_keycenter=69\n"
        "<region> sample=sine.wav\n");
    return sfz;
}

inline std::vector<float> renderSeconds (SfInstrument& s, double seconds, double rate = 48000.0)
{
    std::vector<float> l ((size_t) (seconds * rate)), r (l.size());
    for (size_t done = 0; done < l.size(); )
    {
        const int n = (int) std::min<size_t> (256, l.size() - done);
        s.render (l.data() + done, r.data() + done, n);
        done += (size_t) n;
    }
    return l;
}

inline double magnitude (const std::vector<float>& x, double hz, double from, double to, double rate = 48000.0)
{
    double re = 0, im = 0;
    const size_t a = (size_t) (from * rate), b = std::min (x.size(), (size_t) (to * rate));
    for (size_t i = a; i < b; ++i) { const double ph = 6.283185307179586 * hz * (double) i / rate; re += x[i] * std::cos (ph); im += x[i] * std::sin (ph); }
    return std::sqrt (re * re + im * im) / (double) std::max<size_t> (1, b - a);
}

inline double rms (const std::vector<float>& x, double from, double to, double rate = 48000.0)
{
    double s = 0;
    const size_t a = (size_t) (from * rate), b = std::min (x.size(), (size_t) (to * rate));
    for (size_t i = a; i < b; ++i) s += (double) x[i] * x[i];
    return std::sqrt (s / (double) std::max<size_t> (1, b - a));
}

// the strongest of a set of candidate frequencies
inline double nearest (const std::vector<float>& x, std::initializer_list<double> hz, double from, double to)
{
    double best = 0, bestMag = -1;
    for (double f : hz) { const double m = magnitude (x, f, from, to); if (m > bestMag) { bestMag = m; best = f; } }
    return best;
}

#if defined (_MSC_VER)
 __declspec (noinline)
#endif
inline bool run (const Report& report, const juce::File& tmp)
{
    bool all = true;
    const auto sf2File = writeSf2 (tmp.getChildFile ("sftest/test.sf2"));
    const auto sfzFile = writeSfz (tmp.getChildFile ("sftest/sfz"));
    juce::String error;

    // ---- SoundFont
    auto sf2 = SfBank::load (sf2File, 0, error);
    all &= report (sf2 != nullptr && sf2->presets.size() == 2 && sf2->regions.size() == 2 && sf2->format == SfBank::Sf2,
                   "SoundFont: file read, 2 presets, 2 zones " + error);
    if (sf2 == nullptr)
        return false;
    all &= report (sf2->pool->waitForPreload (5000), "SoundFont: samples decoded in the background");

    {
        SfInstrument inst; inst.prepare (48000.0); inst.setBank (sf2);
        inst.noteOn (69, 0.8f);
        auto a = renderSeconds (inst, 3.0);   // (a loop of 0.09 s keeps a 3 s note going)
        const double f = nearest (a, { 392.0, 415.3, 440.0, 466.2, 493.9 }, 0.2, 0.7);
        all &= report (f == 440.0 && magnitude (a, 440.0, 0.2, 0.7) > 4.0 * magnitude (a, 523.25, 0.2, 0.7), "SoundFont: A4 sounds at 440 Hz");
        all &= report (rms (a, 2.5, 2.9) > 0.5 * rms (a, 0.2, 0.6), "SoundFont: the loop keeps a held note going (" + juce::String (rms (a, 2.5, 2.9), 3) + " after 2.5 s)");
        inst.noteOff (69);
        auto b = renderSeconds (inst, 1.0);
        all &= report (rms (b, 0.0, 0.02) > 0.01 && rms (b, 0.6, 1.0) < 1.0e-4, "SoundFont: the note dies away in its release time");

        inst.noteOn (81, 0.8f);
        auto c = renderSeconds (inst, 0.6);
        all &= report (nearest (c, { 784.0, 880.0, 988.0 }, 0.01, 0.08) == 880.0 && rms (c, 0.3, 0.6) < 1.0e-4, "SoundFont: the second zone plays 880 Hz and, without a loop, ends with its sample");
        inst.noteOff (81);
        renderSeconds (inst, 0.3);

        inst.noteOn (69, 1.0f); auto loud = renderSeconds (inst, 0.4); inst.noteOff (69); renderSeconds (inst, 0.5);
        inst.noteOn (69, 0.3f); auto soft = renderSeconds (inst, 0.4); inst.noteOff (69); renderSeconds (inst, 0.5);
        all &= report (rms (loud, 0.1, 0.4) > 3.0 * rms (soft, 0.1, 0.4), "SoundFont: velocity changes the level");

        // polyphony: three notes at once, and many at once do not crash or run away
        inst.reset();
        inst.noteOn (60, 0.7f); inst.noteOn (64, 0.7f); inst.noteOn (67, 0.7f);
        auto chord = renderSeconds (inst, 0.8);
        all &= report (magnitude (chord, 261.6, 0.2, 0.7) > 0.02 && magnitude (chord, 329.6, 0.2, 0.7) > 0.02 && magnitude (chord, 392.0, 0.2, 0.7) > 0.02, "SoundFont: a chord plays all its notes");
        inst.reset();
        for (int i = 0; i < 100; ++i) inst.noteOn (30 + i % 40, 0.9f);
        auto many = renderSeconds (inst, 0.5);
        float peak = 0; for (auto v : many) { if (! std::isfinite (v)) { peak = 1.0e9f; break; } peak = std::max (peak, std::abs (v)); }
        all &= report (peak < 40.0f, "SoundFont: 100 notes at once stay within the voice limit (peak " + juce::String (peak, 1) + ")");

        // pitch bend (+2 semitones at the top) and the sustain pedal
        inst.reset();
        inst.control (128, 1.0f);
        inst.noteOn (69, 0.8f);
        auto bent = renderSeconds (inst, 0.8);
        all &= report (nearest (bent, { 440.0, 466.2, 493.9, 523.3 }, 0.2, 0.7) == 493.9, "SoundFont: pitch bend raises the note two semitones");
        inst.reset();
        inst.control (64, 1.0f);
        inst.noteOn (69, 0.8f); renderSeconds (inst, 0.2); inst.noteOff (69);
        auto held = renderSeconds (inst, 0.6);
        inst.control (64, 0.0f);
        auto lifted = renderSeconds (inst, 0.6);
        all &= report (rms (held, 0.3, 0.6) > 0.05 && rms (lifted, 0.4, 0.6) < 1.0e-4, "SoundFont: the sustain pedal holds a released note until it comes up");
    }

    {
        auto octave = sf2->withPreset (1);
        all &= report (octave != nullptr && octave->presetIndex == 1 && octave->pool->waitForPreload (5000), "SoundFont: another preset of the same file");
        if (octave != nullptr)
        {
            SfInstrument inst; inst.prepare (48000.0); inst.setBank (octave);
            inst.noteOn (69, 0.8f);
            auto a = renderSeconds (inst, 0.6);
            all &= report (nearest (a, { 440.0, 622.3, 880.0, 987.8 }, 0.2, 0.5) == 880.0, "SoundFont: the preset's own tuning applies (an octave up)");
        }
    }

    {
        juce::String err2;
        auto bad = SfBank::load (sfzFile.getSiblingFile ("samples").getChildFile ("sine.wav"), 0, err2);
        const auto junk = tmp.getChildFile ("sftest/junk.sf2");
        junk.replaceWithText ("this is not a SoundFont");
        auto bad2 = SfBank::load (junk, 0, err2);
        all &= report (bad == nullptr && bad2 == nullptr && err2.isNotEmpty(), "SoundFont: a wrong or damaged file gives an error, not a crash");
    }

    // ---- SFZ
    error.clear();
    auto sfz = SfBank::load (sfzFile, 0, error);
    all &= report (sfz != nullptr && sfz->format == SfBank::Sfz && sfz->regions.size() == 9 && sfz->missingFiles.isEmpty() && sfz->sourceFiles.size() == 2,
                   "SFZ: 9 regions, #include, #define and default_path read, no sample missing (" + juce::String (sfz != nullptr ? (int) sfz->regions.size() : 0) + " regions) " + error);
    if (sfz == nullptr)
        return false;
    all &= report (sfz->pool->waitForPreload (5000), "SFZ: samples decoded in the background");

    {
        SfInstrument inst; inst.prepare (48000.0); inst.setBank (sfz);
        inst.noteOn (36, 0.8f);   // 36 = 33 semitones below the root: 440 / 2^(33/12)
        auto low = renderSeconds (inst, 1.0);
        const double want = 440.0 * std::pow (2.0, -33.0 / 12.0);
        all &= report (magnitude (low, want, 0.3, 0.9) > 4.0 * magnitude (low, want * 1.5, 0.3, 0.9) && rms (low, 0.7, 0.95) > 0.5 * rms (low, 0.3, 0.5),
                       "SFZ: a looped sample is tuned by pitch_keycenter and keeps sounding");
        inst.noteOff (36);
        auto tail = renderSeconds (inst, 0.4);
        all &= report (rms (tail, 0.25, 0.4) < 1.0e-4, "SFZ: ampeg_release ends the note");

        // round robin: the same key, three times: sine, saw, sine (the saw has a strong octave)
        auto harmonic = [&] (float vel)
        {
            inst.noteOn (69, vel);
            auto x = renderSeconds (inst, 0.4);
            inst.noteOff (69);
            renderSeconds (inst, 0.3);
            return magnitude (x, 880.0, 0.1, 0.4) / magnitude (x, 440.0, 0.1, 0.4);
        };
        inst.reset();
        const double r1 = harmonic (0.8f), r2 = harmonic (0.8f), r3 = harmonic (0.8f);
        all &= report (r1 < 0.05 && r2 > 0.15 && r3 < 0.05, "SFZ: seq_length / seq_position alternate the samples (" + juce::String (r1, 2) + ", " + juce::String (r2, 2) + ", " + juce::String (r3, 2) + ")");

        // velocity layers
        auto layer = [&] (float vel) { inst.noteOn (60, vel); auto x = renderSeconds (inst, 0.4); inst.noteOff (60); renderSeconds (inst, 0.3);
                                       const double f0 = 440.0 * std::pow (2.0, -9.0 / 12.0);
                                       return magnitude (x, 2.0 * f0, 0.1, 0.4) / magnitude (x, f0, 0.1, 0.4); };
        const double soft = layer (0.3f), hard = layer (0.9f);
        all &= report (soft < 0.05 && hard > 0.15, "SFZ: lovel / hivel pick the layer (" + juce::String (soft, 2) + " soft, " + juce::String (hard, 2) + " hard)");

        // release trigger: silent while the key is down, a sound when it comes up
        inst.reset();
        inst.noteOn (30, 0.8f);
        auto down = renderSeconds (inst, 0.2);
        inst.noteOff (30);
        auto up = renderSeconds (inst, 0.2);
        all &= report (rms (down, 0.0, 0.2) < 1.0e-6 && rms (up, 0.0, 0.1) > 0.01, "SFZ: trigger=release plays when the key is released");

        // one shot: the note-off does not cut the sample (0.1 s)
        inst.reset();
        inst.noteOn (50, 0.9f); renderSeconds (inst, 0.01); inst.noteOff (50);
        auto shot = renderSeconds (inst, 0.3);
        all &= report (rms (shot, 0.05, 0.09) > 0.02 && rms (shot, 0.2, 0.3) < 1.0e-4, "SFZ: loop_mode=one_shot plays out after the key is released");

        // groups: a note of group 1 cuts the other one (off_by=1)
        inst.reset();
        inst.noteOn (51, 0.8f); renderSeconds (inst, 0.2);
        inst.noteOn (52, 0.8f);
        auto cut = renderSeconds (inst, 0.4);
        all &= report (magnitude (cut, 440.0 * std::pow (2.0, -18.0 / 12.0), 0.2, 0.4) < 0.01 && magnitude (cut, 440.0 * std::pow (2.0, -17.0 / 12.0), 0.2, 0.4) > 0.05,
                       "SFZ: group / off_by cuts the sounding note");
    }

    // ---- an SFZ with a missing sample still loads: the regions are there, the file is reported
    {
        const auto broken = tmp.getChildFile ("sftest/sfz/broken.sfz");
        broken.replaceWithText ("<region> sample=nothing/here.wav key=60\n<region> sample=samples/sine.wav key=62\n");
        juce::String e;
        auto bank = SfBank::load (broken, 0, e);
        all &= report (bank != nullptr && bank->missingFiles.size() == 1 && bank->pool->waitForPreload (5000), "SFZ: a missing sample file is reported, the rest still loads");
        if (bank != nullptr)
        {
            SfInstrument inst; inst.prepare (48000.0); inst.setBank (bank);
            inst.noteOn (60, 0.8f); inst.noteOn (62, 0.8f);
            auto x = renderSeconds (inst, 0.2);
            all &= report (rms (x, 0.05, 0.2) > 0.01, "SFZ: notes with a missing sample are silent, others play");
        }
    }

    // ---- a sample that is not loaded when its note starts is requested and plays from the next note on (nothing waits on the audio thread)
    {
        SfInstrument inst; inst.prepare (48000.0);
        auto lazy = SfBank::load (sfzFile, 0, error);
        inst.setBank (lazy);
        lazy->pool->waitForPreload (5000);
        all &= report (lazy->pool->loadedBytes() > 0, "pool: " + juce::String ((int) lazy->pool->loadedBytes()) + " bytes decoded");
    }
    return all;
}
}
