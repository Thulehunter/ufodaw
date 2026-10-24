#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

// SoundFont (.sf2) and SFZ (.sfz) instruments.
//
// Both formats are turned into the same thing: a list of regions (a key range, a velocity range, a sample and how to play it: root key, tuning,
// loop, envelope, pan, volume...). One voice engine plays them, so a SoundFont and an SFZ sound the same way for what they share.
//
// Samples are not decoded while the file is read: SfPool owns them and a helper thread decodes them (everything that fits a memory budget right
// after loading, the rest when a note asks for it). A voice that starts on a sample that is not there yet stays silent, the audio thread never waits.

// One sample of an instrument. Written by the pool's thread (once), read by the audio thread through `ready`.
struct SfSample
{
    juce::File file;                    // the audio file (SFZ) or the SoundFont
    juce::int64 byteOffset = -1;        // >= 0: 16 bit mono data inside the SoundFont, `frames` long
    juce::int64 frames = 0;
    double rate = 44100.0;
    std::atomic<int> state { 0 };       // 0 not loaded, 1 asked for by a note, 2 loaded, 3 failed (missing / unreadable)
    std::atomic<const juce::AudioBuffer<float>*> ready { nullptr };
    std::unique_ptr<juce::AudioBuffer<float>> data;
};

// The samples of one instrument file and the thread that decodes them.
class SfPool : private juce::Thread
{
public:
    SfPool();
    ~SfPool() override;

    SfSample* add (SfSample*);   // takes ownership; message thread, before start()
    void start();                // begins decoding in the background (the order of `add`)
    bool preloadFinished() const { return preloadDone.load(); }
    bool waitForPreload (int timeoutMs) const;   // for tests and for project loading: true when everything that was going to be loaded is
    juce::int64 loadedBytes() const { return bytesLoaded.load(); }
    int numSamples() const { return (int) samples.size(); }
    SfSample& sample (int i) { return *samples[(size_t) i]; }
    void request (SfSample&);    // audio thread: lock-free, the helper thread picks it up

    static constexpr juce::int64 preloadBudget = 384ll * 1024 * 1024;   // decoded bytes loaded right away
    static constexpr juce::int64 totalBudget = 1536ll * 1024 * 1024;    // never more than this in memory

private:
    void run() override;
    bool load (SfSample&);

    std::vector<std::unique_ptr<SfSample>> samples;
    std::atomic<bool> requested { false };
    juce::AudioFormatManager formats;
    std::atomic<bool> preloadDone { false };
    std::atomic<juce::int64> bytesLoaded { 0 };
};

// How a note plays one sample (a "region" of an SFZ, a zone of a SoundFont).
struct SfRegion
{
    enum LoopMode { NoLoop = 0, OneShot, Continuous, Sustain };

    SfSample* sample = nullptr;
    int loKey = 0, hiKey = 127, loVel = 1, hiVel = 127;
    int keyCenter = 60;
    float tuneCents = 0.0f, keyTrack = 100.0f;   // cents added, cents per key
    float volumeDb = 0.0f, pan = 0.0f;           // pan -1..1
    float ampVelTrack = 1.0f;                    // 0..1: how much the velocity changes the level
    int loopMode = NoLoop;
    juce::int64 offset = 0, end = -1;            // frames; end < 0 = the end of the sample
    juce::int64 loopStart = -1, loopEnd = -1;    // frames; < 0 = the start / the end
    float delay = 0.0f, attack = 0.0f, hold = 0.0f, decay = 0.0f, sustain = 1.0f, release = 0.0f;   // seconds, level 0..1
    float keyToHold = 0.0f, keyToDecay = 0.0f;   // timecents per key below / above middle C (SoundFonts)
    float cutoffHz = 0.0f, resonanceDb = 0.0f;   // 0 = no filter
    bool triggerRelease = false;                 // plays when the key is released (the noise of a key coming up)
    int triggerMode = 0;                         // 0 always, 1 first (no other key is down), 2 legato (another key is down)
    int seqLength = 1, seqPosition = 1;          // round robin: plays at note number `seqPosition` of every `seqLength` (per key)
    float loRand = 0.0f, hiRand = 1.0f;
    int group = 0, offBy = 0;                    // a note of group `offBy` cuts this one
    bool offFast = true;
    int swLast = -1;                             // plays only after this key switch
};

// The result of reading an instrument file: immutable once built, shared with the audio thread (which only reads it).
class SfBank
{
public:
    enum Format { Sf2, Sfz };
    struct Preset { juce::String name; int bank = 0, number = 0; };

    // Reads a .sf2 or .sfz file. Returns null (and an error text) when it is not an instrument. Missing samples do not fail: they are listed in `missingFiles`.
    static std::shared_ptr<SfBank> load (const juce::File&, int presetIndex, juce::String& error);
    // The same SoundFont with another preset (shares the samples); SFZ files have one "preset" only.
    std::shared_ptr<SfBank> withPreset (int presetIndex) const;

    Format format = Sfz;
    juce::File file;
    juce::String name;
    std::vector<Preset> presets;                // SoundFonts: every preset in the file
    int presetIndex = 0;
    std::vector<SfRegion> regions;
    std::array<std::vector<int>, 128> byKey;    // indices into `regions` per key
    int swLo = -1, swHi = -1, swDefault = -1;   // the key switches of an SFZ (-1 = none)
    juce::StringArray missingFiles;             // samples that could not be found (SFZ)
    juce::Array<juce::File> sourceFiles;        // the SFZ and everything it includes
    std::shared_ptr<SfPool> pool;
    std::shared_ptr<const void> sf2Info;        // (the parsed headers of a SoundFont, to switch presets)

    juce::String describe() const;              // "Piano - Acoustic Grand (3 of 128)"
};

// Plays a bank. Audio thread: noteOn / noteOff / control / render (no allocation, no locks). The message thread changes the bank while the engine lock is held.
class SfInstrument
{
public:
    struct State { std::shared_ptr<const SfBank> bank; float volume = 1.0f; };

    SfInstrument();

    std::atomic<float> volume { 1.0f };

    // message thread (under the engine lock)
    void setBank (std::shared_ptr<const SfBank>);
    std::shared_ptr<const SfBank> getBank() const { return bank; }
    State captureState() const { return { bank, volume.load() }; }
    void applyState (const State&);

    // audio thread
    void prepare (double outputRate) { outRate = outputRate; }
    void noteOn (int note, float velocity);
    void noteOff (int note);
    void control (int controller, float value01);   // CC 0..127 as in CcEvent (64 sustain, 7 volume, 10 pan, 11 expression), 128 pitch bend (0.5 = centre)
    void allNotesOff();   // lets everything release (and lifts the pedal)
    void reset();         // silences at once
    bool isActive() const;
    void render (float* outL, float* outR, int numSamples);   // adds into outL / outR

private:
    static constexpr int maxVoices = 64;

    struct Voice
    {
        enum Stage { Idle, Delay, Attack, Hold, Decay, Sustain, Release };
        Stage stage = Idle;
        const SfRegion* region = nullptr;
        const juce::AudioBuffer<float>* buffer = nullptr;
        int note = 0;
        bool released = false, ignoreRelease = false, fast = false;
        double position = 0.0, step = 1.0, end = 0.0, loopStart = 0.0, loopEnd = 0.0;
        bool looping = false;
        double level = 0.0;
        double attackStep = 0.0, decayCoef = 1.0, releaseCoef = 1.0, sustainLevel = 1.0;
        int counter = 0;                  // samples left in the delay / hold stage
        int holdLen = 0;                  // samples of the hold stage
        float gainL = 1.0f, gainR = 1.0f;
        bool filter = false;
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1L = 0, z2L = 0, z1R = 0, z2R = 0;
        unsigned long long age = 0;
    };

    void startVoice (const SfRegion&, int note, int velocity, bool fromRelease);
    void releaseVoice (Voice&, bool fast);
    void releaseKey (int note);
    void renderVoice (Voice&, float* outL, float* outR, int n, double bendFactor, float ccGain, float ccPan);

    std::shared_ptr<const SfBank> bank;
    double outRate = 44100.0;
    std::array<Voice, maxVoices> voices;
    unsigned long long counter = 0;
    bool keyDown[128] = {}, sustained[128] = {};
    int lastVelocity[128] = {};
    int seqCount[128] = {};
    int numDown = 0;
    bool pedal = false;
    float bendSemitones = 0.0f, ccVolume = 1.0f, ccExpression = 1.0f, ccPan = 0.0f;
    int lastSwitch = -1;
    unsigned randomState = 12345u;
};
