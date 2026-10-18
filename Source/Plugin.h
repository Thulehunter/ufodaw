#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>

// One hosted plugin (a VST3 instrument or effect) sitting in a track.
// Shared between the track, undo snapshots and the editor window.
struct PluginSlot
{
    int key = 0;   // unique within a project; automation lanes refer to the plugin by it

    static std::atomic<int>& keyCounter() { static std::atomic<int> counter { 1 }; return counter; }
    static int newKey() { return keyCounter()++; }
    static void reserveKey (int k)   // after loading a saved key, make sure new plugins never get the same one
    {
        int current = keyCounter();
        while (current <= k && ! keyCounter().compare_exchange_weak (current, k + 1)) {}
    }

    std::unique_ptr<juce::AudioPluginInstance> instance;
    juce::PluginDescription description;
    std::atomic<bool> bypass { false };
    std::atomic<bool> editorOpen { false };   // its editor window is open: the engine keeps the plugin processing (many plugins say "audio engine inactive" otherwise)
    juce::AudioBuffer<float> scratch;   // processing buffer sized for the plugin's channel count
};
