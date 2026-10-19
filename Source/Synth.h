#pragma once

#include <array>
#include <atomic>
#include <vector>

// name, default, lowest, highest, label (the label shows in the automation list and the synth window)
#define UFODAW_SYNTH_FLOATS(X) \
    X (attack,       0.005f, 0.001f, 2.0f,     "Attack")              /* seconds */ \
    X (decay,        0.15f,  0.001f, 2.0f,     "Decay")               \
    X (sustain,      0.7f,   0.0f,   1.0f,     "Sustain")             \
    X (release,      0.25f,  0.01f,  3.0f,     "Release")             \
    X (cutoff,       8000.0f, 100.0f, 18000.0f, "Cutoff")             /* Hz */ \
    X (resonance,    0.0f,   0.0f,   1.0f,     "Resonance")           \
    X (drive,        0.0f,   0.0f,   1.0f,     "Drive")               \
    X (keyTrack,     0.0f,   0.0f,   1.0f,     "Key tracking")        /* how much the cutoff follows the note */ \
    X (filterEnv,    0.0f,  -4.0f,   4.0f,     "Filter envelope")     /* octaves the envelope moves the cutoff */ \
    X (fAttack,      0.005f, 0.001f, 2.0f,     "Filter attack")       \
    X (fDecay,       0.3f,   0.001f, 2.0f,     "Filter decay")        \
    X (fSustain,     0.0f,   0.0f,   1.0f,     "Filter sustain")      \
    X (fRelease,     0.3f,   0.01f,  3.0f,     "Filter release")      \
    X (osc2Semi,     0.0f,  -24.0f,  24.0f,    "Osc 2 pitch")         /* semitones */ \
    X (osc2Detune,   0.0f,  -50.0f,  50.0f,    "Osc 2 detune")        /* cents */ \
    X (osc2Mix,      0.0f,   0.0f,   1.0f,     "Osc 2 level")         \
    X (subLevel,     0.0f,   0.0f,   1.0f,     "Sub oscillator")      /* a sine, one octave down */ \
    X (pulseWidth,   0.5f,   0.05f,  0.5f,     "Pulse width")         /* of the square wave */ \
    X (unisonDetune, 15.0f,  0.0f,   50.0f,    "Unison detune")       /* cents */ \
    X (spread,       0.7f,   0.0f,   1.0f,     "Stereo spread")       \
    X (lfoRate,      5.0f,   0.05f,  20.0f,    "LFO rate")            /* Hz */ \
    X (lfoPitch,     0.0f,   0.0f,   100.0f,   "LFO to pitch")        /* cents */ \
    X (lfoCutoff,    0.0f,   0.0f,   4.0f,     "LFO to cutoff")       /* octaves */ \
    X (lfoAmp,       0.0f,   0.0f,   1.0f,     "LFO to volume")       \
    X (lfoDelay,     0.0f,   0.0f,   3.0f,     "LFO fade-in")         /* seconds */ \
    X (glide,        0.0f,   0.0f,   1.0f,     "Glide")               /* seconds */ \
    X (pitchEnv,     0.0f,  -24.0f,  24.0f,    "Pitch envelope")      /* semitones the note starts above (or below) its pitch */ \
    X (pitchEnvTime, 0.08f,  0.005f, 1.0f,     "Pitch envelope time") \
    X (pianoDecay,   1.0f,   0.3f,   3.0f,     "Piano ring time")     /* 1 = like a real piano */ \
    X (pianoBright,  0.5f,   0.0f,   1.0f,     "Piano brightness")    \
    X (pianoDetune,  0.5f,   0.0f,   1.0f,     "Piano string detune") \
    X (pianoHammer,  0.35f,  0.0f,   1.0f,     "Piano hammer noise")  \
    X (level,        1.0f,   0.0f,   4.0f,     "Synth volume")

// name, default: waveform, osc 2 wave (0 sine, 1 saw, 2 square, 3 triangle), unison voices (1..5), LFO shape (0 sine, 1 triangle, 2 square, 3 random steps)
#define UFODAW_SYNTH_INTS(X) \
    X (waveform, 1) \
    X (osc2Wave, 1) \
    X (unison,   1) \
    X (lfoShape, 0) \
    X (engine,   0) /* 0 = subtractive synth, 1 = piano */ \
    X (mono,     0) /* 1 = one note at a time, legato: a held note glides to the next */

// Every setting of a synth as plain numbers (for undo, presets and project files).
struct SynthValues
{
   #define UFODAW_X(name, def, lo, hi, label) float name = def;
    UFODAW_SYNTH_FLOATS (UFODAW_X)
   #undef UFODAW_X
   #define UFODAW_X(name, def) int name = def;
    UFODAW_SYNTH_INTS (UFODAW_X)
   #undef UFODAW_X
};

// Settings of a track's built-in synth. Written by the UI, read by the audio thread.
struct SynthParams
{
    enum Waveform { Sine = 0, Saw, Square, Triangle };

   #define UFODAW_X(name, def, lo, hi, label) std::atomic<float> name { def };
    UFODAW_SYNTH_FLOATS (UFODAW_X)
   #undef UFODAW_X
   #define UFODAW_X(name, def) std::atomic<int> name { def };
    UFODAW_SYNTH_INTS (UFODAW_X)
   #undef UFODAW_X

    struct FloatInfo { const char* name; const char* label; float lo, hi, def; std::atomic<float> SynthParams::* member; };
    static const std::vector<FloatInfo>& floatInfo()
    {
        static const std::vector<FloatInfo> list = {
           #define UFODAW_X(name, def, lo, hi, label) { #name, label, lo, hi, def, &SynthParams::name },
            UFODAW_SYNTH_FLOATS (UFODAW_X)
           #undef UFODAW_X
        };
        return list;
    }

    SynthValues capture() const
    {
        SynthValues v;
       #define UFODAW_X(name, def, lo, hi, label) v.name = name.load();
        UFODAW_SYNTH_FLOATS (UFODAW_X)
       #undef UFODAW_X
       #define UFODAW_X(name, def) v.name = name.load();
        UFODAW_SYNTH_INTS (UFODAW_X)
       #undef UFODAW_X
        return v;
    }

    void apply (const SynthValues& v)
    {
       #define UFODAW_X(name, def, lo, hi, label) name = v.name;
        UFODAW_SYNTH_FLOATS (UFODAW_X)
       #undef UFODAW_X
       #define UFODAW_X(name, def) name = v.name;
        UFODAW_SYNTH_INTS (UFODAW_X)
       #undef UFODAW_X
    }
};

// Polyphonic subtractive synth: up to five detuned unison oscillators (spread over the stereo image), a second oscillator and a sub oscillator, a
// resonant 12 dB low-pass filter with its own envelope, key tracking and drive, an amplitude envelope, one LFO (pitch, cutoff, volume) and glide.
// All methods are meant for the audio thread (no allocation, no locking).
class Synth
{
public:
    void prepare (double sampleRate);

    void noteOn (int pitch, float velocity, const SynthParams&);
    void noteOff (int pitch);        // releases the oldest held voice with this pitch (in mono mode: goes back to the note still held, if any)
    void allNotesOff();              // release everything (soft)
    void reset();                    // silence immediately
    bool isActive() const;

    // Adds `numSamples` of stereo output into `outL` / `outR`.
    void render (float* outL, float* outR, int numSamples, const SynthParams&);

private:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    struct Env
    {
        Stage stage = Stage::Idle;
        float level = 0.0f;
    };

    struct Voice
    {
        Env amp, filt;
        int pitch = 0;
        float velocity = 0.0f;
        double targetLog = 0.0, currentLog = 0.0;     // ln of the frequency in Hz: glide moves one towards the other
        double phase[5] = {};
        double phase2 = 0.0, phaseSub = 0.0;
        float ic1L = 0.0f, ic2L = 0.0f, ic1R = 0.0f, ic2R = 0.0f;   // filter state
        double lfoPhase = 0.0, lfoTime = 0.0;
        float lfoHeld = 0.0f;
        double voiceTime = 0.0;       // seconds since the note began (the pitch envelope)
        unsigned long long age = 0;
        bool active() const { return amp.stage != Stage::Idle; }

        // the piano engine: a sum of decaying sinusoids (one two-pole resonator each), plus a short burst of noise for the hammer
        bool piano = false;
        static constexpr int maxPartials = 40;
        int numPartials = 0;
        double a1[maxPartials] = {}, a2[maxPartials] = {}, y1[maxPartials] = {}, y2[maxPartials] = {};
        float partialL[maxPartials] = {}, partialR[maxPartials] = {};
        float hammerLevel = 0.0f, hammerDecay = 0.99f, hammerLp = 0.0f, hammerCoef = 0.2f;
        double lifetime = 10.0;       // seconds after which the strings have died away
    };

    void startPiano (Voice&, int pitch, float velocity, const SynthParams&);
    void renderPiano (Voice&, float* outL, float* outR, int numSamples, const SynthValues&);

    static void step (Env&, float attackStep, float decayCoef, float sustain, float releaseCoef);

    std::array<Voice, 16> voices;
    double sampleRate = 44100.0;
    unsigned long long counter = 0;
    double lastLog = 0.0;          // the pitch of the last note (glide starts there)
    int heldNotes[16] = {};        // mono mode: the keys that are down, the last one sounds
    int numHeld = 0;
    bool monoMode = false;
    unsigned noise = 22222u;
};
