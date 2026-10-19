#include "ProjectTemplates.h"
#include "StarterPack.h"
#include <initializer_list>

namespace
{
    using juce::var;
    constexpr int templatesVersion = 1;

    var obj (std::initializer_list<std::pair<const char*, var>> props)
    {
        auto* o = new juce::DynamicObject();
        for (auto& p : props)
            o->setProperty (p.first, p.second);
        return var (o);
    }

    struct N { int pitch; double beat; double beats; float vel; };   // a note, in beats from the start of its clip

    var midiClip (double bpm, double startBar, double bars, const std::vector<N>& notes, int beatsPerBar = 4)
    {
        const double beat = 60.0 / bpm;
        juce::Array<var> list;
        for (auto& n : notes)
            list.add (obj ({ { "pitch", n.pitch }, { "start", n.beat * beat }, { "length", n.beats * beat }, { "velocity", (double) n.vel } }));
        return obj ({ { "start", startBar * beatsPerBar * beat }, { "length", bars * beatsPerBar * beat }, { "notes", list } });
    }

    // a drum pattern: for every pad note a string of 16 steps per bar ('X' hard, 'x' medium, 'o' ghost, '.' nothing), repeated for the bars
    var drumClip (double bpm, double startBar, int bars, std::initializer_list<std::pair<int, const char*>> lanes, double swing = 0.0)
    {
        std::vector<N> notes;
        for (int b = 0; b < bars; ++b)
            for (auto& lane : lanes)
            {
                const juce::String pattern (lane.second);
                for (int i = 0; i < pattern.length(); ++i)
                {
                    const auto c = pattern[i];
                    if (c == '.')
                        continue;
                    const double step = b * 4.0 + i * 0.25 + ((i % 2) ? swing * 0.25 : 0.0);
                    notes.push_back ({ lane.first, step, 0.125, c == 'X' ? 0.95f : c == 'x' ? 0.7f : 0.4f });
                }
            }
        return midiClip (bpm, startBar, bars, notes);
    }

    std::vector<N> chordNotes (std::initializer_list<int> pitches, double beat, double beats, float vel)
    {
        std::vector<N> v;
        for (int p : pitches)
            v.push_back ({ p, beat, beats, vel });
        return v;
    }

    void append (std::vector<N>& to, const std::vector<N>& from) { to.insert (to.end(), from.begin(), from.end()); }

    struct SynthSound { int wave; double a, d, s, r, cutoff; };
    const SynthSound sPad { 1, 0.6, 0.5, 0.8, 1.4, 1800 }, sKeys { 3, 0.004, 0.5, 0.35, 0.4, 7000 }, sSub { 0, 0.003, 0.2, 0.9, 0.12, 600 },
                     sFat { 1, 0.003, 0.25, 0.6, 0.15, 900 }, sLead { 2, 0.005, 0.2, 0.7, 0.2, 5000 }, sPluck { 1, 0.002, 0.18, 0.0, 0.12, 4500 },
                     sBell { 0, 0.001, 1.2, 0.0, 1.2, 12000 }, sStrings { 1, 0.35, 0.3, 0.85, 0.8, 3000 };

    var synthTrack (const juce::String& name, const SynthSound& s, const juce::Array<var>& clips, double gain = 0.8, juce::int64 colour = 0, var fx = {})
    {
        auto* t = new juce::DynamicObject();
        t->setProperty ("name", name);
        t->setProperty ("type", "midi");
        t->setProperty ("gain", gain);
        t->setProperty ("clips", juce::Array<var>());
        t->setProperty ("synth", obj ({ { "waveform", s.wave }, { "attack", s.a }, { "decay", s.d }, { "sustain", s.s }, { "release", s.r }, { "cutoff", s.cutoff } }));
        t->setProperty ("midiClips", clips);
        if (colour != 0) t->setProperty ("colour", colour);
        if (! fx.isVoid()) t->setProperty ("fx", fx);
        return var (t);
    }

    var drumTrack (const juce::String& name, int kitIndex, const juce::Array<var>& clips, double gain = 0.85, var fx = {})
    {
        juce::Array<var> pads;
        const auto& kit = StarterPack::kits()[(size_t) kitIndex];
        for (auto& p : kit.pads)
            pads.add (StarterPack::sample (p.first, p.second).getFullPathName());
        auto* t = new juce::DynamicObject();
        t->setProperty ("name", name);
        t->setProperty ("type", "midi");
        t->setProperty ("gain", gain);
        t->setProperty ("clips", juce::Array<var>());
        t->setProperty ("midiClips", clips);
        t->setProperty ("drumPads", pads);
        if (! fx.isVoid()) t->setProperty ("fx", fx);
        return var (t);
    }

    var audioTrack (const juce::String& name, const juce::Array<var>& clips, double gain = 0.8, var fx = {}, bool bus = false)
    {
        auto* t = new juce::DynamicObject();
        t->setProperty ("name", name);
        t->setProperty ("type", "audio");
        t->setProperty ("gain", gain);
        t->setProperty ("clips", clips);
        if (bus) t->setProperty ("bus", true);
        if (! fx.isVoid()) t->setProperty ("fx", fx);
        return var (t);
    }

    var audioClip (const juce::File& file, double start, double length)
    {
        return obj ({ { "file", file.getFullPathName() }, { "start", start }, { "offset", 0.0 }, { "length", length } });
    }

    var section (const char* name, int startBar, int bars)
    {
        return obj ({ { "name", name }, { "startBar", startBar }, { "bars", bars } });
    }

    template <typename... T>
    juce::Array<var> arr (T... v) { return juce::Array<var> { v... }; }

    void write (const juce::String& name, double bpm, juce::Array<var> tracks, juce::Array<var> sections, bool metronome = false, double masterGain = 0.9)
    {
        auto* root = new juce::DynamicObject();
        root->setProperty ("version", 3);
        root->setProperty ("bpm", bpm);
        root->setProperty ("masterGain", masterGain);
        root->setProperty ("metronome", metronome);
        root->setProperty ("tracks", tracks);
        if (! sections.isEmpty())
            root->setProperty ("arrangement", sections);
        const auto file = StarterPack::folder().getChildFile ("Projects").getChildFile (name + ".mdaw");
        file.getParentDirectory().createDirectory();
        file.replaceWithText (juce::JSON::toString (var (root)));
    }

    //==========================================================================
    // the projects (a kit index: 0 Punch, 1 808, 2 Lo-fi, 3 House, 4 Tight)

    void beatStarter()
    {
        const double bpm = 95;
        // A minor: Am F C G, a bar each
        std::vector<N> pad, bass;
        const int roots[4] = { 45, 41, 48, 43 };
        const std::vector<std::vector<int>> chords = { { 57, 60, 64 }, { 53, 57, 60 }, { 55, 60, 64 }, { 55, 59, 62 } };
        for (int b = 0; b < 4; ++b)
        {
            append (pad, chordNotes ({ chords[(size_t) b][0], chords[(size_t) b][1], chords[(size_t) b][2] }, b * 4.0, 3.8, 0.55f));
            bass.push_back ({ roots[b], b * 4.0, 1.5, 0.9f });
            bass.push_back ({ roots[b], b * 4.0 + 2.0, 0.75, 0.8f });
            bass.push_back ({ roots[b] + 12, b * 4.0 + 3.0, 0.5, 0.7f });
        }
        write ("Beat starter", bpm,
               arr (drumTrack ("Drums", 0, arr (drumClip (bpm, 0, 4, { { 36, "X.....X..X......" }, { 38, "....X.......X..." }, { 42, "x.x.x.x.x.x.x.xo" } }),
                                              drumClip (bpm, 4, 4, { { 36, "X.....X..X.X...." }, { 38, "....X.......X..o" }, { 42, "x.x.x.x.x.x.x.xo" }, { 46, "..............x." } }))),
                    synthTrack ("Bass", sSub, arr (midiClip (bpm, 0, 4, bass), midiClip (bpm, 4, 4, bass))),
                    synthTrack ("Chords", sPad, arr (midiClip (bpm, 0, 4, pad), midiClip (bpm, 4, 4, pad)), 0.6)),
               arr (section ("Groove", 0, 4), section ("Groove 2", 4, 4)));
    }

    void lofi()
    {
        const double bpm = 80;
        // Dm9 G13 Cmaj9 Am9, two bars each
        const std::vector<std::vector<int>> chords = { { 50, 57, 60, 64, 65 }, { 43, 53, 59, 64, 65 }, { 48, 55, 59, 62, 64 }, { 45, 55, 60, 64, 67 } };
        const int roots[4] = { 38, 43, 36, 33 };
        std::vector<N> keys, bass;
        for (int c = 0; c < 4; ++c)
        {
            for (int bar = 0; bar < 2; ++bar)
            {
                const double b0 = c * 8.0 + bar * 4.0;
                for (int p : chords[(size_t) c]) { keys.push_back ({ p, b0, 1.4, 0.5f }); keys.push_back ({ p, b0 + 2.0, 1.4, 0.4f }); }
                bass.push_back ({ roots[c], b0, 1.5, 0.85f });
                bass.push_back ({ roots[c] + (bar ? 7 : 12), b0 + 2.5, 1.0, 0.7f });
            }
        }
        const auto vinylFile = StarterPack::sample ("FX", "Vinyl Crackle");
        juce::Array<var> vinyl;
        for (int i = 0; i < 6; ++i) vinyl.add (audioClip (vinylFile, i * 4.0, 4.0));
        write ("Lo-fi hip hop", bpm,
               arr (drumTrack ("Drums", 2, arr (drumClip (bpm, 0, 8, { { 36, "X.......x.X....." }, { 38, "....X.......X..." }, { 42, "x.x.x.x.x.x.x.x." } }, 0.2),
                                                drumClip (bpm, 8, 8, { { 36, "X.......x.X.X..." }, { 38, "....X.......X..o" }, { 42, "x.xox.x.x.xox.x." } }, 0.2))),
                    synthTrack ("Keys", sKeys, arr (midiClip (bpm, 0, 8, keys), midiClip (bpm, 8, 8, keys)), 0.65, 0,
                                obj ({ { "chorusOn", true }, { "chorusMix", 0.35 }, { "chorusRate", 0.4 }, { "chorusDepth", 0.4 }, { "chorusDelay", 14.0 }, { "satOn", true }, { "satDrive", 6.0 }, { "satType", 1.0 }, { "satMix", 0.6 } })),
                    synthTrack ("Bass", sSub, arr (midiClip (bpm, 0, 8, bass), midiClip (bpm, 8, 8, bass)), 0.8),
                    audioTrack ("Vinyl", vinyl, 0.5)),
               arr (section ("Intro", 0, 4), section ("Beat", 4, 12), section ("Beat 2", 16, 8), section ("Outro", 24, 4)));
    }

    void house()
    {
        const double bpm = 124;
        std::vector<N> stabs, bass, lead;
        const std::vector<std::vector<int>> chords = { { 57, 60, 64, 67 }, { 53, 57, 60, 64 }, { 55, 59, 62, 65 }, { 52, 55, 59, 62 } };
        const int roots[4] = { 33, 29, 31, 28 };
        for (int c = 0; c < 4; ++c)
        {
            const double b0 = c * 4.0;
            for (double off : { 0.5, 1.5, 2.5, 3.5 })
                for (int p : chords[(size_t) c]) stabs.push_back ({ p, b0 + off, 0.3, 0.65f });
            for (int i = 0; i < 4; ++i) bass.push_back ({ roots[c], b0 + i + 0.5, 0.35, 0.9f });
        }
        for (int c = 0; c < 4; ++c)
            for (int i = 0; i < 8; ++i)
                lead.push_back ({ chords[(size_t) c][(size_t) (i % 4)] + 12, c * 4.0 + i * 0.5, 0.4, 0.55f });
        write ("House 124", bpm,
               arr (drumTrack ("Drums", 3, arr (drumClip (bpm, 0, 4, { { 36, "X...X...X...X..." }, { 42, "................" } }),
                                                drumClip (bpm, 4, 4, { { 36, "X...X...X...X..." }, { 39, "....X.......X..." }, { 42, "..x...x...x...x." } }),
                                                drumClip (bpm, 8, 8, { { 36, "X...X...X...X..." }, { 39, "....X.......X..." }, { 42, "..x...x...x...x." }, { 46, "..............x." } }))),
                    synthTrack ("Bass", sFat, arr (midiClip (bpm, 4, 4, bass), midiClip (bpm, 8, 4, bass), midiClip (bpm, 12, 4, bass)), 0.75),
                    synthTrack ("Stabs", sPluck, arr (midiClip (bpm, 4, 4, stabs), midiClip (bpm, 8, 4, stabs), midiClip (bpm, 12, 4, stabs)), 0.55),
                    synthTrack ("Lead", sLead, arr (midiClip (bpm, 8, 4, lead), midiClip (bpm, 12, 4, lead)), 0.45, 0,
                                obj ({ { "delayOn", true }, { "delayTime", 363.0 }, { "delayFeedback", 0.4 }, { "delayMix", 0.3 }, { "reverbOn", true }, { "reverbMix", 0.2 } }))),
               arr (section ("Intro", 0, 4), section ("Build", 4, 4), section ("Drop", 8, 8)), false, 0.85);
    }

    void trap()
    {
        const double bpm = 140;
        std::vector<N> sub, bell;
        const int roots[4] = { 33, 33, 36, 31 };
        for (int c = 0; c < 4; ++c)
        {
            sub.push_back ({ roots[c], c * 4.0, 1.5, 0.95f });
            sub.push_back ({ roots[c], c * 4.0 + 2.5, 1.0, 0.9f });
            sub.push_back ({ roots[c] + 7, c * 4.0 + 3.5, 0.4, 0.8f });
        }
        const int melody[8] = { 69, 72, 76, 72, 74, 72, 69, 67 };
        for (int i = 0; i < 16; ++i)
            if (i % 3 != 2) bell.push_back ({ melody[i % 8], i * 1.0, 0.8, 0.6f });
        write ("Trap 140", bpm,
               arr (drumTrack ("Drums", 1, arr (drumClip (bpm, 0, 4, { { 36, "X.....x...X....." }, { 38, "........X......." }, { 42, "x.x.x.x.xxx.x.x." }, { 46, "..............x." } }),
                                                drumClip (bpm, 4, 4, { { 36, "X.....x...X.x..." }, { 38, "........X......." }, { 42, "xxxxxxxxxxx.xoxo" }, { 46, "..............x." } }))),
                    synthTrack ("808", sSub, arr (midiClip (bpm, 0, 4, sub), midiClip (bpm, 4, 4, sub)), 0.9),
                    synthTrack ("Bell", sBell, arr (midiClip (bpm, 0, 4, bell), midiClip (bpm, 4, 4, bell)), 0.45, 0,
                                obj ({ { "reverbOn", true }, { "reverbMix", 0.3 }, { "reverbDecay", 3.0 } }))),
               arr (section ("Intro", 0, 4), section ("Verse", 4, 4)));
    }

    void songSketch()
    {
        const double bpm = 100;
        std::vector<N> bass, keys;
        const int roots[4] = { 48, 43, 45, 41 };
        const std::vector<std::vector<int>> chords = { { 60, 64, 67 }, { 55, 59, 62 }, { 57, 60, 64 }, { 53, 57, 60 } };
        for (int c = 0; c < 4; ++c)
        {
            append (keys, chordNotes ({ chords[(size_t) c][0], chords[(size_t) c][1], chords[(size_t) c][2] }, c * 4.0, 3.8, 0.55f));
            bass.push_back ({ roots[c] - 12, c * 4.0, 1.0, 0.85f });
            bass.push_back ({ roots[c] - 12, c * 4.0 + 2.0, 1.0, 0.8f });
        }
        auto repeated = [&] (const std::vector<N>& notes, int fromBar, int count)
        {
            juce::Array<var> clips;
            for (int i = 0; i < count; ++i)
                clips.add (midiClip (bpm, fromBar + i * 4, 4, notes));
            return clips;
        };
        juce::Array<var> drums;
        const int verseBars[] = { 4, 12 };   // verse 1 (bars 4-11), chorus 1 (12-19) ... filled below
        (void) verseBars;
        drums.add (drumClip (bpm, 4, 8, { { 36, "X.......X.X....." }, { 38, "....X.......X..." }, { 42, "x.x.x.x.x.x.x.x." } }));
        drums.add (drumClip (bpm, 12, 8, { { 36, "X...X...X...X..." }, { 38, "....X.......X..." }, { 42, "xxxxxxxxxxxxxxxx" }, { 46, "..............x." } }));
        drums.add (drumClip (bpm, 20, 8, { { 36, "X.......X.X....." }, { 38, "....X.......X..." }, { 42, "x.x.x.x.x.x.x.x." } }));
        drums.add (drumClip (bpm, 28, 8, { { 36, "X...X...X...X..." }, { 38, "....X.......X..." }, { 42, "xxxxxxxxxxxxxxxx" }, { 46, "..............x." } }));
        drums.add (drumClip (bpm, 40, 8, { { 36, "X...X...X...X..." }, { 38, "....X.......X..." }, { 42, "xxxxxxxxxxxxxxxx" } }));
        auto bassClips = repeated (bass, 4, 12);
        auto keyClips = repeated (keys, 0, 12);
        write ("Song sketch", bpm,
               arr (drumTrack ("Drums", 0, drums), synthTrack ("Bass", sFat, bassClips, 0.75), synthTrack ("Keys", sKeys, keyClips, 0.6),
                    audioTrack ("Guitar", {}, 0.8), audioTrack ("Vocals", {}, 0.85)),
               arr (section ("Intro", 0, 4), section ("Verse 1", 4, 8), section ("Chorus 1", 12, 8), section ("Verse 2", 20, 8), section ("Chorus 2", 28, 8),
                    section ("Bridge", 36, 4), section ("Chorus 3", 40, 8), section ("Outro", 48, 4)), true);
    }

    void vocals()
    {
        // a vocal track with a ready chain (high-pass, compressor, de-esser, limiter) and a reverb bus it is sent to
        auto vocalFx = obj ({ { "eqOn", true }, { "eqHpFreq", 90.0 }, { "eqMid", -2.0 }, { "eqMidFreq", 300.0 }, { "eqMid2", 3.0 }, { "eqMid2Freq", 4500.0 }, { "eqHigh", 2.0 }, { "eqHighFreq", 10000.0 },
                              { "compOn", true }, { "compThreshold", -20.0 }, { "compRatio", 3.0 }, { "compKnee", 8.0 }, { "compAttack", 8.0 }, { "compRelease", 120.0 }, { "compRms", 0.5 }, { "compAuto", true },
                              { "deEssOn", true }, { "deEssFreq", 6500.0 }, { "deEssAmount", 8.0 },
                              { "limOn", true }, { "limCeiling", -1.0 }, { "limLookahead", 2.0 } });
        auto reverbFx = obj ({ { "reverbOn", true }, { "reverbSize", 0.5 }, { "reverbDecay", 1.8 }, { "reverbPreDelay", 20.0 }, { "reverbMix", 1.0 }, { "reverbLowCut", 200.0 } });
        auto vocal = audioTrack ("Vocal", {}, 0.85, vocalFx);
        if (auto* o = vocal.getDynamicObject())
            o->setProperty ("sends", arr (obj ({ { "bus", 2 }, { "level", 0.22 }, { "pre", false } })));
        write ("Vocal recording", 100, arr (audioTrack ("Backing track", {}, 0.7), vocal, audioTrack ("Vocal reverb", {}, 0.8, reverbFx, true)), {}, true);
    }
}

namespace ProjectTemplates
{
    static juce::File marker() { return StarterPack::folder().getChildFile ("Projects").getChildFile (".version"); }

    void ensureWritten()
    {
        if (marker().existsAsFile() && marker().loadFileAsString().trim().getIntValue() == templatesVersion)
            return;
        beatStarter();
        lofi();
        house();
        trap();
        songSketch();
        vocals();
        marker().replaceWithText (juce::String (templatesVersion));
    }

    std::vector<Info> list()
    {
        return {
            { "Beat starter",     "A groove at 95 BPM with drums, bass and chords in A minor", StarterPack::folder().getChildFile ("Projects/Beat starter.mdaw") },
            { "Lo-fi hip hop",    "80 BPM: dusty drums, jazzy keys, a sub bass and vinyl crackle", StarterPack::folder().getChildFile ("Projects/Lo-fi hip hop.mdaw") },
            { "House 124",        "Four on the floor with an offbeat bass, stabs and a lead, in sections", StarterPack::folder().getChildFile ("Projects/House 124.mdaw") },
            { "Trap 140",         "808 drums with hat rolls, a long sub bass and a bell", StarterPack::folder().getChildFile ("Projects/Trap 140.mdaw") },
            { "Song sketch",      "A song with named sections (verse, chorus, bridge), a rhythm section and empty tracks for guitar and vocals", StarterPack::folder().getChildFile ("Projects/Song sketch.mdaw") },
            { "Vocal recording",  "A vocal track with a ready effects chain and a reverb bus, a backing track and the click on", StarterPack::folder().getChildFile ("Projects/Vocal recording.mdaw") },
        };
    }
}
