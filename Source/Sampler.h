#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <memory>

// A sampler instrument in the style of Ableton's Simpler: one sample, played from MIDI notes.
//   Classic  - the sample is pitched by the key (relative to its root note), polyphonic, with an envelope, and can loop.
//   One-shot - like Classic, but the whole sample plays out whatever happens to the key (drums, hits, vocal shots).
//   Slice    - the region is cut into equal slices, one per key from `sliceNote` upwards, played at original pitch.
// The parameters are atomics (the user interface changes them while audio runs); the sample itself is only swapped by the
// message thread while the engine lock is held.
class Sampler
{
public:
    enum Mode { Classic = 0, OneShot = 1, Slice = 2 };

    struct Params
    {
        std::atomic<int> mode { Classic };
        std::atomic<int> rootNote { 60 };           // the key at which the sample plays at its own pitch
        std::atomic<float> tuneCents { 0.0f };
        std::atomic<float> start { 0.0f }, end { 1.0f };            // the part of the sample that plays, as fractions
        std::atomic<bool> loop { false };                           // Classic mode: repeat between loopStart and loopEnd while the key is held
        std::atomic<float> loopStart { 0.0f }, loopEnd { 1.0f };    // fractions of the sample
        std::atomic<bool> reverse { false };
        std::atomic<float> attack { 0.002f }, decay { 0.3f }, sustain { 1.0f }, release { 0.15f };   // seconds, seconds, level, seconds
        std::atomic<float> cutoff { 20000.0f };                     // low-pass, Hz (20000 = off)
        std::atomic<float> volume { 0.8f };
        std::atomic<float> velocitySens { 0.7f };                   // how much the velocity changes the level (0 = not at all)
        std::atomic<int> slices { 8 };
        std::atomic<int> sliceNote { 36 };                          // key of the first slice (C2)
        // Slice mode with slices that follow the hits of the sample (not equal parts): the start of every slice, as a fraction of the
        // whole sample, ascending. markCount 0 = equal slices (`slices` of them). A slice lasts until the next mark (the last one until `end`).
        static constexpr int maxMarks = 64;
        std::atomic<int> markCount { 0 };
        std::atomic<float> marks[maxMarks];
    } params;

    Sampler() { for (auto& m : params.marks) m.store (0.0f); }

    // The slices of the region as (start seconds from the first slice, length seconds) at the original speed: what the pattern of a
    // sliced loop needs to play them in time. Empty when there is no sample or Slice mode has no slices.
    std::vector<std::pair<double, double>> sliceTimes() const;
    // Sets the slice starts (fractions of the whole sample, ascending); an empty list goes back to equal slices
    void setMarks (const std::vector<float>& starts);
    // Everything that can be edited, for undo / redo: the settings and the sample (the audio data is shared, not copied)
    struct State
    {
        int mode = Classic, rootNote = 60, slices = 8, sliceNote = 36;
        std::vector<float> marks;   // (the slice starts, see Params::marks)
        float tuneCents = 0.0f, start = 0.0f, end = 1.0f, loopStart = 0.0f, loopEnd = 1.0f;
        float attack = 0.0f, decay = 0.0f, sustain = 0.0f, release = 0.0f, cutoff = 0.0f, volume = 0.0f, velocitySens = 0.0f;
        bool loop = false, reverse = false;
        std::shared_ptr<const juce::AudioBuffer<float>> sample, original;
        double sampleRate = 44100.0, stretch = 1.0;
        juce::File file;
        juce::String name;
    };
    State captureState() const;
    void applyState (const State&);   // (silences the voices; callers hold the engine lock)


    // Message thread (callers hold the engine lock while changing a sampler that is in use)
    void setSample (std::shared_ptr<const juce::AudioBuffer<float>> buffer, double rate, const juce::File& file, const juce::String& name);
    std::shared_ptr<const juce::AudioBuffer<float>> getSample() const { return sample; }
    std::shared_ptr<const juce::AudioBuffer<float>> getOriginal() const { return original; }   // the sample as loaded (before time-stretch)
    double getStretch() const { return stretchRatio; }                                          // 1 = as loaded; 2 = twice as long, same pitch
    void setStretched (std::shared_ptr<const juce::AudioBuffer<float>> buffer, double ratio) { reset(); sample = std::move (buffer); stretchRatio = ratio; }
    double getSampleRate() const { return sampleRate; }
    const juce::File& getFile() const { return file; }
    const juce::String& getName() const { return name; }

    // Audio thread
    void prepare (double outputRate) { outRate = outputRate; }
    void noteOn (int note, float velocity);
    void noteOff (int note);
    void allNotesOff();   // lets every voice release
    void choke();         // every voice fades out fast (a drum pad that is cut off by another one)
    bool hasSample() const { return sample != nullptr; }
    void reset();         // silences every voice at once
    bool isActive() const;
    void render (float* outL, float* outR, int numSamples);   // adds into outL / outR

private:
    struct Voice
    {
        bool active = false, released = false, oneShotLike = false, choked = false;
        int note = 0;
        float velocity = 1.0f;
        double position = 0.0;            // in source frames
        double step = 1.0;                // source frames per output frame (negative: reverse)
        double regionStart = 0.0, regionEnd = 0.0;
        double loopStart = 0.0, loopEnd = 0.0;
        bool looping = false;
        enum Stage { Attack, Decay, Sustain, Release } stage = Attack;
        float level = 0.0f;
        float lowL = 0.0f, lowR = 0.0f, low2L = 0.0f, low2R = 0.0f;   // low-pass state (two poles)
        unsigned long long age = 0;
    };

    void start (int note, float velocity);

    std::shared_ptr<const juce::AudioBuffer<float>> sample, original;
    double stretchRatio = 1.0;
    double sampleRate = 44100.0;
    juce::File file;
    juce::String name;

    double outRate = 44100.0;
    std::array<Voice, 16> voices;
    unsigned long long counter = 0;
};
