#pragma once

#include <array>
#include <atomic>

// Settings of a track's built-in synth. Written by the UI, read by the audio thread.
struct SynthParams
{
    enum Waveform { Sine = 0, Saw, Square, Triangle };

    std::atomic<int> waveform { Saw };
    std::atomic<float> attack { 0.005f };    // seconds
    std::atomic<float> decay { 0.15f };      // seconds
    std::atomic<float> sustain { 0.7f };     // 0..1
    std::atomic<float> release { 0.25f };    // seconds
    std::atomic<float> cutoff { 8000.0f };   // Hz, one-pole low-pass
};

// Small polyphonic subtractive-style synth: band-limited oscillator -> low-pass -> ADSR.
// All methods are meant for the audio thread (no allocation, no locking).
class Synth
{
public:
    void prepare (double sampleRate);

    void noteOn (int pitch, float velocity);
    void noteOff (int pitch);        // releases the oldest held voice with this pitch
    void allNotesOff();              // release everything (soft)
    void reset();                    // silence immediately
    bool isActive() const;

    // Adds `numSamples` of mono output into `out`.
    void render (float* out, int numSamples, const SynthParams&);

private:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    struct Voice
    {
        Stage stage = Stage::Idle;
        int pitch = 0;
        float velocity = 0.0f;
        double phase = 0.0;
        double increment = 0.0;
        float level = 0.0f;
        float releaseStep = 0.0f;
        float filterState = 0.0f;
        unsigned long long age = 0;
    };

    std::array<Voice, 16> voices;
    double sampleRate = 44100.0;
    unsigned long long counter = 0;
};
