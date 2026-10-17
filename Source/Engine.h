#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <memory>
#include <vector>

struct Track
{
    juce::String name;
    juce::AudioBuffer<float> audio;
    double fileRate = 44100.0;
    double startSeconds = 0.0;

    std::atomic<float> gain { 0.8f };
    std::atomic<float> pan { 0.0f };
    std::atomic<bool> mute { false };
    std::atomic<bool> solo { false };
    std::atomic<float> level { 0.0f };

    double lengthSeconds() const { return fileRate > 0 ? audio.getNumSamples() / fileRate : 0.0; }
};

// Mixes all tracks against a shared transport. The audio thread only ever try-locks,
// so editing the track list on the message thread can never block audio.
class Engine : public juce::AudioSource
{
public:
    Engine();

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;

    // Message thread
    Track* addTrackFromFile (const juce::File&);
    void removeTrack (Track*);
    int getNumTracks() const;
    Track* getTrack (int index) const;

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
    juce::AudioFormatManager formats;
    juce::CriticalSection lock;
    std::vector<std::unique_ptr<Track>> tracks;

    std::atomic<bool> playing { false };
    std::atomic<juce::int64> positionSamples { 0 };
    double sampleRate = 44100.0;
};
