#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <memory>
#include "Sampler.h"

// A drum rack: 16 pads, each one a small Simpler (a Sampler) with its own sample, start / end, pitch, envelope and filter, plus a
// pad level, pan, mute, solo and a choke group (pads of one group cut each other off: closed hat / open hat). The first eight pads
// start with generated sounds, so a new drum track works without any files; any pad can get your own sample.
// Pads are triggered by MIDI notes, so drum clips are ordinary MIDI clips.
class DrumKit
{
public:
    static constexpr int numPads = 16;

    // A pad can hold more samples than one (up to maxLayers extra ones). Each has a velocity range (`velLo`..`velHi`, 0..1): a hit plays the
    // samples whose range contains its velocity. When several fit (the ranges overlap, which is how they start: all of 0..1) they take turns,
    // round-robin, so a repeated snare does not sound the same every time. One sample over 0..0.6 and another over 0.6..1 is a soft / hard pair.
    static constexpr int maxLayers = 3;

    struct Layer
    {
        Sampler sampler;                                         // plays this layer's sample (it starts as a copy of the pad's own settings)
        std::shared_ptr<const juce::AudioBuffer<float>> sample;
        double sampleRate = 44100.0;
        juce::File file;
        std::atomic<bool> used { false };
        std::atomic<float> velLo { 0.0f }, velHi { 1.0f };
    };

    struct Pad
    {
        std::atomic<float> velLo { 0.0f }, velHi { 1.0f };       // the velocity range of the pad's own sample
        std::array<Layer, maxLayers> layers;                     // the extra samples
        int roundRobin = 0;                                      // (audio thread) which of the matching samples is next
        juce::String name;                                       // (message thread)
        int note = 36;                                           // MIDI note that triggers this pad
        std::shared_ptr<const juce::AudioBuffer<float>> sample;  // mono or stereo (what the pad's sampler plays)
        double sampleRate = 44100.0;
        juce::File file;                                         // empty while the generated sound (or none) is in use
        std::atomic<float> gain { 1.0f };                        // the pad's level, 0..1.5
        std::atomic<float> pan { 0.0f };                         // -1 .. 1
        std::atomic<bool> mute { false }, solo { false };
        std::atomic<int> chokeGroup { 0 };                       // 0 = none; pads in the same group cut each other off
        Sampler sampler;                                         // the Simpler of this pad
    };

    DrumKit();
    // For undo / redo: every pad's mixer settings, sample and Simpler (the audio data is shared, not copied)
    struct LayerState
    {
        bool used = false;
        float velLo = 0.0f, velHi = 1.0f;
        std::shared_ptr<const juce::AudioBuffer<float>> sample;
        double sampleRate = 44100.0;
        juce::File file;
        Sampler::State sampler;
    };

    struct PadState
    {
        float velLo = 0.0f, velHi = 1.0f;
        std::array<LayerState, maxLayers> layers;
        float gain = 1.0f, pan = 0.0f;
        bool mute = false, solo = false;
        int chokeGroup = 0;
        std::shared_ptr<const juce::AudioBuffer<float>> sample;
        double sampleRate = 44100.0;
        juce::File file;
        Sampler::State sampler;
    };
    std::vector<PadState> captureState() const;
    void applyState (const std::vector<PadState>&);   // (callers hold the engine lock)


    // Message thread (callers hold the engine lock while changing a live kit)
    void setSample (int pad, std::shared_ptr<const juce::AudioBuffer<float>> buffer, double rate, const juce::File& file);
    void setLayer (int pad, int layer, std::shared_ptr<const juce::AudioBuffer<float>> buffer, double rate, const juce::File& file);   // layer 0..maxLayers-1
    void clearLayer (int pad, int layer);
    int layerCount (int pad) const;                              // the extra samples in use
    void resetPad (int pad);                                     // back to the generated sound (none for pads 9..16)
    const Pad& getPad (int pad) const { return pads[(size_t) pad]; }
    Pad& getPad (int pad) { return pads[(size_t) pad]; }
    int padForNote (int note) const;                             // -1 if no pad listens to this note
    bool hasSample (int pad) const { return pads[(size_t) pad].sample != nullptr; }

    // Audio thread
    void prepare (double outputRate);
    void noteOn (int note, float velocity);
    void noteOff (int note);
    void stopAll();                                              // everything releases
    void reset();
    bool isActive() const;
    void render (float* outL, float* outR, int numSamples);      // adds stereo output

private:
    static void chokePad (Pad&);
    void generateDefault (int pad);
    void setupSampler (int pad);

    std::array<Pad, numPads> pads;
    double outRate = 44100.0;
    std::vector<float> scratchL, scratchR, kitL, kitR;   // (the kit sums its pads into kitL / kitR, then a soft limiter keeps the sum below full scale)
};
