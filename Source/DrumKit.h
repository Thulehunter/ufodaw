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

    struct Pad
    {
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
    struct PadState
    {
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
    void generateDefault (int pad);
    void setupSampler (int pad);

    std::array<Pad, numPads> pads;
    std::vector<float> scratchL, scratchR;
};
