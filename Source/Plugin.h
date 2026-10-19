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

    // Set when the plugin crashed while it was processing sound (see guardedProcess): it is switched off for good (until the project is opened again).
    std::atomic<bool> crashed { false };

    ~PluginSlot() { if (crashed.load()) instance.release(); }   // (a plugin that crashed is not destroyed: that would likely crash again; it is left alone)
};

// processBlock of a hosted plugin with a safety net: a crash inside the plugin (an access violation, a divide by zero) is caught instead of ending the
// program. The plugin is then marked as crashed and never called again; the caller makes silence (or passes the sound through). Windows only: elsewhere
// the plugin runs unguarded.
inline bool guardedProcess (PluginSlot& slot, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    if (slot.crashed.load())
        return false;
   #if JUCE_WINDOWS
    __try
    {
        slot.instance->processBlock (buffer, midi);
        return true;
    }
    __except (1)   // (EXCEPTION_EXECUTE_HANDLER)
    {
        slot.crashed = true;
        slot.bypass = true;
        return false;
    }
   #else
    slot.instance->processBlock (buffer, midi);
    return true;
   #endif
}
