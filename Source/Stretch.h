#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>
#include <memory>

// Time-stretching and pitch-shifting of audio with Rubber Band (the finer engine, processed offline).
namespace Stretch
{
    // A processed copy of `numSamples` samples of `in`, from `startSample`: `timeRatio` times as long (2.0 = twice as long, half the
    // speed; 0.5 = twice as fast) with the pitch moved by `semitones`. `progress` gets 0..1 and returns false to cancel (the result
    // is then null). `formants` keeps the vocal character (the spectral envelope) when the pitch moves. Mono and stereo; more channels are ignored.
    std::shared_ptr<juce::AudioBuffer<float>> process (const juce::AudioBuffer<float>& in, int startSample, int numSamples,
                                                       double sampleRate, double timeRatio, double semitones,
                                                       const std::function<bool (double)>& progress, bool formants = false);
}
