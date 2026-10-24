#pragma once

#include "Synth.h"
#include <functional>
#include <algorithm>
#include <string>
#include <vector>

// Ready-made sounds for the built-in synth. Waves: 0 sine, 1 saw, 2 square, 3 triangle.
namespace SynthPresets
{
    struct Preset
    {
        const char* category;
        const char* name;
        SynthValues values;
    };

    inline const std::vector<Preset>& all()
    {
        static const std::vector<Preset> list = []
        {
            std::vector<Preset> out;
            auto add = [&out] (const char* category, const char* name, const std::function<void (SynthValues&)>& set)
            {
                SynthValues v;
                set (v);
                out.push_back ({ category, name, v });
            };

            add ("Basic", "Default saw", [] (SynthValues&) {});

            // ---- bass
            add ("Bass", "Sub bass", [] (SynthValues& v) { v.waveform = 0; v.attack = 0.003f; v.decay = 0.2f; v.sustain = 0.9f; v.release = 0.12f; v.cutoff = 600; v.level = 1.2f; });
            add ("Bass", "Fat bass", [] (SynthValues& v)
            {
                v.waveform = 1; v.osc2Wave = 1; v.osc2Mix = 0.6f; v.osc2Detune = 8; v.subLevel = 0.5f;
                v.attack = 0.003f; v.decay = 0.25f; v.sustain = 0.6f; v.release = 0.15f;
                v.cutoff = 700; v.resonance = 0.25f; v.filterEnv = 1.5f; v.fDecay = 0.25f; v.fSustain = 0.2f; v.keyTrack = 0.3f; v.drive = 0.25f; v.level = 0.9f;
            });
            add ("Bass", "Reese", [] (SynthValues& v)
            {
                v.waveform = 1; v.unison = 3; v.unisonDetune = 22; v.spread = 0.5f; v.osc2Wave = 1; v.osc2Mix = 0.8f; v.osc2Detune = -12; v.subLevel = 0.3f;
                v.attack = 0.01f; v.decay = 0.3f; v.sustain = 0.9f; v.release = 0.3f;
                v.cutoff = 700; v.resonance = 0.2f; v.lfoCutoff = 0.8f; v.lfoRate = 0.25f; v.drive = 0.2f; v.level = 0.8f;
            });
            add ("Bass", "Acid", [] (SynthValues& v)
            {
                v.waveform = 1; v.attack = 0.002f; v.decay = 0.2f; v.sustain = 0.8f; v.release = 0.08f;
                v.cutoff = 500; v.resonance = 0.8f; v.filterEnv = 3.0f; v.fDecay = 0.18f; v.fSustain = 0.0f; v.keyTrack = 0.2f; v.glide = 0.06f; v.drive = 0.3f; v.level = 0.8f;
            });
            add ("Bass", "Pluck bass", [] (SynthValues& v)
            {
                v.waveform = 2; v.pulseWidth = 0.35f; v.subLevel = 0.4f; v.attack = 0.002f; v.decay = 0.25f; v.sustain = 0.0f; v.release = 0.1f;
                v.cutoff = 450; v.filterEnv = 2.5f; v.fDecay = 0.2f; v.fSustain = 0.0f; v.resonance = 0.2f;
            });
            add ("Bass", "Round bass", [] (SynthValues& v)
            {
                v.waveform = 3; v.subLevel = 0.5f; v.osc2Wave = 0; v.osc2Mix = 0.3f; v.attack = 0.004f; v.decay = 0.3f; v.sustain = 0.8f; v.release = 0.15f; v.cutoff = 900; v.keyTrack = 0.2f;
            });
            add ("Bass", "Wobble", [] (SynthValues& v)
            {
                v.waveform = 1; v.unison = 2; v.unisonDetune = 10; v.subLevel = 0.4f; v.attack = 0.005f; v.sustain = 1.0f; v.release = 0.15f;
                v.cutoff = 400; v.resonance = 0.5f; v.lfoCutoff = 2.5f; v.lfoRate = 2.0f; v.drive = 0.3f; v.level = 0.8f;
            });
            add ("Bass", "808 sub", [] (SynthValues& v)
            {
                v.waveform = 0; v.mono = 1; v.glide = 0.04f; v.pitchEnv = 12; v.pitchEnvTime = 0.05f; v.attack = 0.002f; v.decay = 1.4f; v.sustain = 0.0f; v.release = 0.4f;
                v.cutoff = 3000; v.drive = 0.25f; v.level = 1.2f;
            });
            add ("Bass", "808 distorted", [] (SynthValues& v)
            {
                v.waveform = 0; v.mono = 1; v.glide = 0.03f; v.pitchEnv = 10; v.pitchEnvTime = 0.04f; v.attack = 0.002f; v.decay = 1.2f; v.sustain = 0.0f; v.release = 0.3f;
                v.cutoff = 2500; v.drive = 0.7f; v.level = 1.0f;
            });
            add ("Bass", "Moog bass", [] (SynthValues& v)
            {
                v.waveform = 1; v.osc2Wave = 1; v.osc2Semi = -12; v.osc2Mix = 0.7f; v.osc2Detune = 5; v.mono = 1; v.glide = 0.04f;
                v.attack = 0.003f; v.decay = 0.3f; v.sustain = 0.8f; v.release = 0.12f;
                v.cutoff = 450; v.resonance = 0.35f; v.filterEnv = 3.0f; v.fDecay = 0.3f; v.fSustain = 0.1f; v.keyTrack = 0.3f; v.drive = 0.2f;
            });
            add ("Bass", "Legato bass", [] (SynthValues& v)
            {
                v.waveform = 1; v.subLevel = 0.5f; v.mono = 1; v.glide = 0.08f; v.attack = 0.005f; v.decay = 0.2f; v.sustain = 0.9f; v.release = 0.15f;
                v.cutoff = 700; v.resonance = 0.15f; v.filterEnv = 1.5f; v.fDecay = 0.25f; v.fSustain = 0.4f; v.keyTrack = 0.25f;
            });
            add ("Bass", "Growl", [] (SynthValues& v)
            {
                v.waveform = 1; v.unison = 3; v.unisonDetune = 18; v.osc2Wave = 2; v.osc2Semi = -12; v.osc2Mix = 0.6f; v.mono = 1; v.glide = 0.03f;
                v.attack = 0.004f; v.decay = 0.3f; v.sustain = 0.9f; v.release = 0.15f;
                v.cutoff = 500; v.resonance = 0.5f; v.lfoShape = 1; v.lfoCutoff = 1.5f; v.lfoRate = 4.0f; v.drive = 0.5f;
            });
            add ("Bass", "Rubber bass", [] (SynthValues& v)
            {
                v.waveform = 2; v.pulseWidth = 0.4f; v.mono = 1; v.glide = 0.03f; v.attack = 0.002f; v.decay = 0.3f; v.sustain = 0.4f; v.release = 0.1f;
                v.cutoff = 350; v.resonance = 0.45f; v.filterEnv = 3.0f; v.fDecay = 0.15f; v.fSustain = 0.0f; v.subLevel = 0.3f; v.keyTrack = 0.3f;
            });

            // ---- pianos (the piano engine)
            add ("Piano", "Grand piano", [] (SynthValues& v)
            {
                v.engine = 1; v.release = 0.35f; v.cutoff = 12000; v.spread = 0.8f; v.pianoDecay = 1.0f; v.pianoBright = 0.5f; v.pianoDetune = 0.5f; v.pianoHammer = 0.35f;
            });
            add ("Piano", "Bright piano", [] (SynthValues& v)
            {
                v.engine = 1; v.release = 0.3f; v.cutoff = 15000; v.spread = 0.8f; v.pianoDecay = 0.9f; v.pianoBright = 0.85f; v.pianoDetune = 0.5f; v.pianoHammer = 0.5f;
            });
            add ("Piano", "Felt piano", [] (SynthValues& v)
            {
                v.engine = 1; v.release = 0.5f; v.cutoff = 3500; v.spread = 0.6f; v.pianoDecay = 1.2f; v.pianoBright = 0.1f; v.pianoDetune = 0.3f; v.pianoHammer = 0.08f;
            });
            add ("Piano", "Honky-tonk", [] (SynthValues& v)
            {
                v.engine = 1; v.release = 0.25f; v.cutoff = 9000; v.spread = 0.5f; v.pianoDecay = 0.8f; v.pianoBright = 0.65f; v.pianoDetune = 1.0f; v.pianoHammer = 0.45f;
            });
            add ("Piano", "Upright piano", [] (SynthValues& v)
            {
                v.engine = 1; v.release = 0.25f; v.cutoff = 9000; v.spread = 0.3f; v.pianoDecay = 0.7f; v.pianoBright = 0.55f; v.pianoDetune = 0.6f; v.pianoHammer = 0.6f;
            });
            add ("Piano", "Dream piano", [] (SynthValues& v)
            {
                v.engine = 1; v.release = 1.0f; v.cutoff = 6000; v.spread = 1.0f; v.pianoDecay = 2.2f; v.pianoBright = 0.3f; v.pianoDetune = 0.7f; v.pianoHammer = 0.15f;
            });

            // ---- leads
            add ("Lead", "Saw lead", [] (SynthValues& v)
            {
                v.waveform = 1; v.unison = 3; v.unisonDetune = 12; v.spread = 0.6f; v.attack = 0.005f; v.decay = 0.2f; v.sustain = 0.8f; v.release = 0.2f;
                v.cutoff = 3500; v.resonance = 0.25f; v.filterEnv = 1.0f; v.fDecay = 0.4f; v.fSustain = 0.5f; v.lfoPitch = 12; v.lfoRate = 5.5f; v.lfoDelay = 0.4f; v.glide = 0.05f; v.drive = 0.1f; v.level = 0.8f;
            });
            add ("Lead", "Super saw", [] (SynthValues& v)
            {
                v.waveform = 1; v.unison = 5; v.unisonDetune = 25; v.spread = 0.9f; v.attack = 0.01f; v.decay = 0.2f; v.sustain = 0.85f; v.release = 0.35f;
                v.cutoff = 7000; v.resonance = 0.1f; v.level = 0.7f;
            });
            add ("Lead", "Square lead", [] (SynthValues& v)
            {
                v.waveform = 2; v.unison = 2; v.unisonDetune = 8; v.osc2Wave = 2; v.osc2Semi = 12; v.osc2Mix = 0.3f;
                v.attack = 0.005f; v.decay = 0.2f; v.sustain = 0.7f; v.release = 0.2f; v.cutoff = 3000; v.resonance = 0.3f; v.filterEnv = 1.5f; v.fDecay = 0.3f; v.fSustain = 0.4f; v.level = 0.8f;
            });
            add ("Lead", "Soft lead", [] (SynthValues& v)
            {
                v.waveform = 3; v.osc2Wave = 0; v.osc2Semi = 12; v.osc2Mix = 0.4f; v.attack = 0.03f; v.decay = 0.2f; v.sustain = 0.85f; v.release = 0.3f;
                v.cutoff = 6000; v.lfoPitch = 15; v.lfoRate = 5.0f; v.lfoDelay = 0.5f;
            });
            add ("Lead", "Hollow lead", [] (SynthValues& v)
            {
                v.waveform = 2; v.pulseWidth = 0.2f; v.attack = 0.01f; v.decay = 0.2f; v.sustain = 0.8f; v.release = 0.25f; v.cutoff = 2500; v.resonance = 0.4f; v.lfoCutoff = 0.5f; v.lfoRate = 3.0f;
            });

            // ---- pads
            add ("Pad", "Warm pad", [] (SynthValues& v)
            {
                v.waveform = 1; v.unison = 4; v.unisonDetune = 14; v.spread = 0.9f; v.attack = 0.7f; v.decay = 0.6f; v.sustain = 0.8f; v.release = 1.6f;
                v.cutoff = 1800; v.resonance = 0.1f; v.lfoCutoff = 0.3f; v.lfoRate = 0.15f; v.level = 0.9f;
            });
            add ("Pad", "Glass pad", [] (SynthValues& v)
            {
                v.waveform = 3; v.osc2Wave = 1; v.osc2Semi = 12; v.osc2Mix = 0.3f; v.unison = 3; v.unisonDetune = 8; v.attack = 1.0f; v.decay = 0.5f; v.sustain = 0.85f; v.release = 2.0f;
                v.cutoff = 3000; v.filterEnv = 0.8f; v.fAttack = 1.2f; v.fDecay = 0.5f; v.fSustain = 0.6f; v.lfoCutoff = 0.4f; v.lfoRate = 0.2f;
            });
            add ("Pad", "Strings", [] (SynthValues& v)
            {
                v.waveform = 1; v.unison = 5; v.unisonDetune = 18; v.spread = 1.0f; v.attack = 0.35f; v.decay = 0.3f; v.sustain = 0.85f; v.release = 0.8f;
                v.cutoff = 3200; v.lfoPitch = 6; v.lfoRate = 5.2f; v.lfoDelay = 1.0f; v.level = 0.75f;
            });
            add ("Pad", "Dark pad", [] (SynthValues& v)
            {
                v.waveform = 1; v.osc2Wave = 1; v.osc2Semi = -12; v.osc2Mix = 0.5f; v.unison = 3; v.unisonDetune = 10; v.attack = 1.2f; v.decay = 0.6f; v.sustain = 0.8f; v.release = 2.2f;
                v.cutoff = 900; v.resonance = 0.15f; v.lfoCutoff = 0.6f; v.lfoRate = 0.1f;
            });
            add ("Pad", "Airy pad", [] (SynthValues& v)
            {
                v.waveform = 3; v.unison = 5; v.unisonDetune = 20; v.spread = 1.0f; v.attack = 0.9f; v.decay = 0.5f; v.sustain = 0.8f; v.release = 1.8f;
                v.cutoff = 2200; v.resonance = 0.35f; v.lfoCutoff = 0.5f; v.lfoRate = 0.3f; v.level = 0.9f;
            });

            // ---- keys
            add ("Keys", "Electric piano", [] (SynthValues& v)
            {
                v.waveform = 3; v.osc2Wave = 0; v.osc2Semi = 12; v.osc2Mix = 0.35f; v.attack = 0.003f; v.decay = 0.8f; v.sustain = 0.2f; v.release = 0.4f;
                v.cutoff = 3500; v.keyTrack = 0.5f; v.filterEnv = 1.2f; v.fDecay = 0.35f; v.fSustain = 0.1f; v.lfoAmp = 0.15f; v.lfoRate = 4.5f;
            });
            add ("Keys", "Clav", [] (SynthValues& v)
            {
                v.waveform = 2; v.pulseWidth = 0.3f; v.attack = 0.002f; v.decay = 0.25f; v.sustain = 0.25f; v.release = 0.1f;
                v.cutoff = 1800; v.resonance = 0.3f; v.filterEnv = 2.5f; v.fDecay = 0.12f; v.fSustain = 0.0f; v.keyTrack = 0.7f;
            });
            add ("Keys", "Organ", [] (SynthValues& v)
            {
                v.waveform = 0; v.osc2Wave = 0; v.osc2Semi = 12; v.osc2Mix = 0.6f; v.subLevel = 0.5f; v.attack = 0.008f; v.decay = 0.1f; v.sustain = 1.0f; v.release = 0.08f;
                v.cutoff = 9000; v.lfoAmp = 0.1f; v.lfoRate = 6.0f; v.level = 0.9f;
            });
            add ("Keys", "Soft keys", [] (SynthValues& v)
            {
                v.waveform = 3; v.subLevel = 0.3f; v.attack = 0.004f; v.decay = 0.6f; v.sustain = 0.3f; v.release = 0.4f; v.cutoff = 4000; v.keyTrack = 0.4f;
            });

            // ---- plucks and mallets
            add ("Pluck", "Pluck", [] (SynthValues& v)
            {
                v.waveform = 1; v.unison = 2; v.unisonDetune = 6; v.attack = 0.002f; v.decay = 0.18f; v.sustain = 0.0f; v.release = 0.12f;
                v.cutoff = 2500; v.filterEnv = 2.5f; v.fDecay = 0.15f; v.fSustain = 0.0f;
            });
            add ("Pluck", "Harp", [] (SynthValues& v)
            {
                v.waveform = 3; v.osc2Wave = 0; v.osc2Semi = 12; v.osc2Mix = 0.5f; v.attack = 0.002f; v.decay = 0.9f; v.sustain = 0.0f; v.release = 0.6f; v.cutoff = 5000; v.keyTrack = 0.6f;
            });
            add ("Pluck", "Marimba", [] (SynthValues& v)
            {
                v.waveform = 0; v.osc2Wave = 0; v.osc2Semi = 24; v.osc2Mix = 0.25f; v.attack = 0.001f; v.decay = 0.45f; v.sustain = 0.0f; v.release = 0.3f; v.cutoff = 5000;
            });
            add ("Pluck", "Bell", [] (SynthValues& v)
            {
                v.waveform = 0; v.osc2Wave = 0; v.osc2Semi = 19; v.osc2Mix = 0.5f; v.attack = 0.001f; v.decay = 1.8f; v.sustain = 0.0f; v.release = 1.6f; v.cutoff = 12000; v.lfoAmp = 0.05f; v.lfoRate = 5.0f;
            });
            add ("Pluck", "Arp pluck", [] (SynthValues& v)
            {
                v.waveform = 2; v.pulseWidth = 0.25f; v.attack = 0.002f; v.decay = 0.12f; v.sustain = 0.0f; v.release = 0.1f;
                v.cutoff = 2500; v.filterEnv = 2.0f; v.fDecay = 0.1f; v.fSustain = 0.0f; v.resonance = 0.2f;
            });

            // ---- brass and winds
            add ("Brass", "Brass", [] (SynthValues& v)
            {
                v.waveform = 1; v.unison = 3; v.unisonDetune = 8; v.attack = 0.06f; v.decay = 0.25f; v.sustain = 0.7f; v.release = 0.2f;
                v.cutoff = 1800; v.resonance = 0.15f; v.filterEnv = 2.0f; v.fAttack = 0.08f; v.fDecay = 0.3f; v.fSustain = 0.5f; v.drive = 0.15f;
            });
            add ("Brass", "Brass section", [] (SynthValues& v)
            {
                v.waveform = 1; v.unison = 5; v.unisonDetune = 12; v.spread = 0.8f; v.attack = 0.1f; v.decay = 0.3f; v.sustain = 0.75f; v.release = 0.3f;
                v.cutoff = 1600; v.filterEnv = 2.2f; v.fAttack = 0.12f; v.fDecay = 0.35f; v.fSustain = 0.5f; v.drive = 0.2f; v.level = 0.8f;
            });
            add ("Brass", "Flute", [] (SynthValues& v)
            {
                v.waveform = 3; v.osc2Wave = 0; v.osc2Semi = 12; v.osc2Mix = 0.2f; v.attack = 0.08f; v.decay = 0.2f; v.sustain = 0.85f; v.release = 0.25f;
                v.cutoff = 5000; v.lfoPitch = 8; v.lfoRate = 5.0f; v.lfoDelay = 0.6f;
            });

            // ---- wavetable (the wavetable engine: Wavetable.h)
            add ("Wavetable", "WT Morph pad", [] (SynthValues& v)
            {
                v.engine = 2; v.wtTable = 0; v.wtPos = 0.2f; v.wtLfo = 0.6f; v.lfoRate = 0.3f; v.unison = 3; v.unisonDetune = 14; v.spread = 0.8f;
                v.attack = 0.4f; v.decay = 0.5f; v.sustain = 0.8f; v.release = 0.9f; v.cutoff = 6000; v.level = 0.9f;
            });
            add ("Wavetable", "WT Glass bell", [] (SynthValues& v)
            {
                v.engine = 2; v.wtTable = 3; v.wtPos = 0.35f; v.wtEnv = -0.3f; v.unison = 2; v.unisonDetune = 6; v.attack = 0.002f; v.decay = 1.2f; v.sustain = 0.0f; v.release = 1.0f;
                v.cutoff = 12000; v.fAttack = 0.002f; v.fDecay = 0.8f; v.fSustain = 0.0f;
            });
            add ("Wavetable", "WT Vowel lead", [] (SynthValues& v)
            {
                v.engine = 2; v.wtTable = 2; v.wtPos = 0.0f; v.wtLfo = 0.5f; v.lfoRate = 0.5f; v.mono = 1; v.glide = 0.05f; v.unison = 2; v.unisonDetune = 8;
                v.attack = 0.02f; v.decay = 0.2f; v.sustain = 0.85f; v.release = 0.25f; v.cutoff = 9000;
            });
            add ("Wavetable", "WT Organ", [] (SynthValues& v)
            {
                v.engine = 2; v.wtTable = 4; v.wtPos = 0.3f; v.wtLfo = 0.3f; v.lfoRate = 0.15f; v.attack = 0.01f; v.decay = 0.1f; v.sustain = 1.0f; v.release = 0.08f; v.cutoff = 9000;
            });
            add ("Wavetable", "WT PWM bass", [] (SynthValues& v)
            {
                v.engine = 2; v.wtTable = 5; v.wtPos = 0.1f; v.wtEnv = 0.8f; v.subLevel = 0.4f; v.attack = 0.003f; v.decay = 0.3f; v.sustain = 0.7f; v.release = 0.12f;
                v.cutoff = 900; v.resonance = 0.2f; v.filterEnv = 1.0f; v.fDecay = 0.3f; v.fSustain = 0.0f; v.keyTrack = 0.3f;
            });
            add ("Wavetable", "WT Digital pluck", [] (SynthValues& v)
            {
                v.engine = 2; v.wtTable = 1; v.wtPos = 0.9f; v.wtEnv = -0.8f; v.attack = 0.002f; v.decay = 0.35f; v.sustain = 0.0f; v.release = 0.2f;
                v.cutoff = 10000; v.fAttack = 0.002f; v.fDecay = 0.2f; v.fSustain = 0.0f; v.unison = 2; v.unisonDetune = 5;
            });

            // ---- effects
            add ("FX", "Riser", [] (SynthValues& v)
            {
                v.waveform = 1; v.unison = 5; v.unisonDetune = 50; v.spread = 1.0f; v.attack = 2.0f; v.decay = 0.5f; v.sustain = 1.0f; v.release = 1.0f;
                v.cutoff = 600; v.resonance = 0.3f; v.filterEnv = 3.0f; v.fAttack = 2.0f; v.fDecay = 0.5f; v.fSustain = 1.0f; v.level = 0.7f;
            });

            // loudness balance: the level that brings a three-note chord of each sound to about the same peak (measured with EngineTest)
            static const std::pair<const char*, float> trims[] = {
                { "Sub bass", 0.791f }, { "Fat bass", 1.382f }, { "Reese", 1.345f }, { "Acid", 0.828f },
                { "Pluck bass", 0.982f }, { "Round bass", 1.293f }, { "Wobble", 1.081f }, { "808 sub", 2.468f },
                { "808 distorted", 3.453f }, { "Moog bass", 2.79f }, { "Legato bass", 2.677f }, { "Growl", 2.635f },
                { "Rubber bass", 1.783f }, { "Grand piano", 0.77f }, { "Bright piano", 0.769f }, { "Felt piano", 0.82f },
                { "Honky-tonk", 0.804f }, { "Upright piano", 0.834f }, { "Dream piano", 0.747f }, { "Saw lead", 1.371f },
                { "Super saw", 1.203f }, { "Square lead", 0.822f }, { "Soft lead", 0.961f }, { "Hollow lead", 0.529f },
                { "Warm pad", 1.039f }, { "Glass pad", 0.884f }, { "Strings", 1.08f }, { "Dark pad", 1.099f },
                { "Airy pad", 0.951f }, { "Electric piano", 0.983f }, { "Clav", 0.671f }, { "Organ", 0.879f },
                { "Soft keys", 1.096f }, { "Pluck", 1.263f }, { "Harp", 0.897f }, { "Marimba", 0.938f },
                { "Bell", 0.867f }, { "Arp pluck", 0.987f }, { "Brass", 1.032f }, { "Brass section", 1.318f },
                { "Flute", 1.086f }, { "Riser", 1.657f } };
            for (auto& p : out)
                for (auto& t : trims)
                    if (std::string (p.name) == t.first)
                        p.values.level = std::min (4.0f, p.values.level * t.second);

            return out;
        }();
        return list;
    }

    // A sound by name (the default sound when there is none of that name).
    inline SynthValues find (const std::string& name)
    {
        for (auto& p : all())
            if (name == p.name)
                return p.values;
        return SynthValues();
    }

    // The distinct categories, in order.
    inline std::vector<const char*> categories()
    {
        std::vector<const char*> out;
        for (auto& p : all())
            if (out.empty() || std::string (out.back()) != p.category)
                out.push_back (p.category);
        return out;
    }
}
