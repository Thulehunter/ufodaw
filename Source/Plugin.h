#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>

// One hosted plugin (a VST3 instrument or effect) sitting in a track.
// Shared between the track, undo snapshots and the editor window.
struct PluginSlot
{
    std::unique_ptr<juce::AudioPluginInstance> instance;
    juce::PluginDescription description;
    std::atomic<bool> bypass { false };
    juce::AudioBuffer<float> scratch;   // processing buffer sized for the plugin's channel count
};
