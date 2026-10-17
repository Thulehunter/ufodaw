#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <memory>

// An 8-pad drum machine. Each pad plays a one-shot sample; the pads start with generated sounds
// (so a new drum track works without any files) and can be replaced by the user's own samples.
// Pads are triggered by MIDI notes, so drum clips are ordinary MIDI clips.
class DrumKit
{
public:
    static constexpr int numPads = 8;

    struct Pad
    {
        juce::String name;
        int note = 36;                                           // MIDI note that triggers this pad
        std::shared_ptr<const juce::AudioBuffer<float>> sample;  // mono or stereo
        double sampleRate = 44100.0;
        juce::File file;                                         // empty while the generated sound is in use
        float gain = 1.0f;
    };

    DrumKit();

    // Message thread (callers hold the engine lock while changing a live kit)
    void setSample (int pad, std::shared_ptr<const juce::AudioBuffer<float>> buffer, double rate, const juce::File& file);
    void resetPad (int pad);                                     // back to the generated sound
    const Pad& getPad (int pad) const { return pads[(size_t) pad]; }
    int padForNote (int note) const;                             // -1 if no pad listens to this note

    // Audio thread
    void prepare (double outputRate) { sampleRate = outputRate; }
    void noteOn (int note, float velocity);
    void stopAll();                                              // quick fade-out of everything
    void reset();
    bool isActive() const;
    void render (float* out, int numSamples);                    // adds mono output into `out`

private:
    struct Voice
    {
        bool active = false;
        int pad = 0;
        double position = 0.0;     // in source frames
        float velocity = 1.0f;
        bool fading = false;
        float fade = 1.0f;
        unsigned long long age = 0;
    };

    void generateDefault (int pad);

    std::array<Pad, numPads> pads;
    std::array<Voice, 24> voices;
    double sampleRate = 44100.0;
    unsigned long long counter = 0;
};
