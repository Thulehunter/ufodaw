#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <memory>
#include <vector>

// A piece of audio placed on the timeline. The sample data is immutable and shared, so
// splitting and duplicating clips is free; only start/offset/length differ.
struct Clip
{
    int id = 0;
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    double fileRate = 44100.0;
    juce::File sourceFile;

    double start = 0.0;   // position on the timeline (seconds)
    double offset = 0.0;  // where in the source audio the clip begins (seconds)
    double length = 0.0;  // how long the clip plays (seconds)

    double end() const { return start + length; }
    double sourceLength() const { return audio != nullptr && fileRate > 0 ? audio->getNumSamples() / fileRate : 0.0; }
};

struct Track
{
    juce::String name;
    std::vector<Clip> clips;   // guarded by Engine::lock

    std::atomic<float> gain { 0.8f };
    std::atomic<float> pan { 0.0f };
    std::atomic<bool> mute { false };
    std::atomic<bool> solo { false };
    std::atomic<float> level { 0.0f };
};

// Mixes all tracks against a shared transport. The audio thread only ever try-locks,
// so editing on the message thread can never block audio.
class Engine : public juce::AudioSource
{
public:
    Engine();

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;

    // ---- Message thread: tracks
    Track* addTrack (const juce::String& name);
    void removeTrack (Track*);
    void removeAllTracks();
    int getNumTracks() const;
    Track* getTrack (int index) const;
    int indexOfTrack (const Track*) const;

    // ---- Message thread: clips (all return -1 / false when nothing was done)
    int addClipFromFile (Track&, const juce::File&, double startSeconds);   // new clip id
    int addClip (Track&, Clip);                                             // used when loading projects
    std::vector<Clip> getClips (const Track&) const;                        // snapshot, safe to read
    bool getClip (const Track&, int clipId, Clip& out) const;
    bool moveClip (Track&, int clipId, double newStart);
    int splitClip (Track&, int clipId, double atSeconds);                   // id of the right-hand part
    int duplicateClip (Track&, int clipId);                                 // id of the copy, placed after the original
    bool deleteClip (Track&, int clipId);
    bool setClipRegion (Track&, int clipId, double start, double offset, double length);

    double getTotalLengthSeconds() const;

    // Mixes the whole project offline (faster than realtime) into a 24-bit stereo WAV.
    bool renderToFile (const juce::File&, double lengthSeconds, bool withMetronome);

    void play()  { playing = true; }
    void stop()  { playing = false; }
    bool isPlaying() const { return playing; }
    void setPositionSeconds (double s) { positionSamples = (juce::int64) juce::jmax (0.0, s * sampleRate); }
    double getPositionSeconds() const { return (double) positionSamples / sampleRate; }
    double getSampleRate() const { return sampleRate; }

    std::atomic<float> bpm { 120.0f };
    std::atomic<bool> metronome { false };
    std::atomic<float> masterGain { 0.9f };
    std::atomic<float> masterLevel { 0.0f };

private:
    // Caller must hold `lock`; adds into outL/outR (which must be pre-cleared).
    void mixBlock (float* outL, float* outR, int numSamples, juce::int64 pos, bool withMetronome);
    Clip* findClip (Track&, int clipId);
    const Clip* findClip (const Track&, int clipId) const;

    juce::AudioFormatManager formats;
    mutable juce::CriticalSection lock;
    std::vector<std::unique_ptr<Track>> tracks;
    int nextClipId = 1;

    std::atomic<bool> playing { false };
    std::atomic<juce::int64> positionSamples { 0 };
    double sampleRate = 44100.0;
};
