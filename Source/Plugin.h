#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <functional>

// A plugin that runs in a helper process (see PluginProxy.h): what the program needs to know about it, apart from the AudioProcessor interface.
struct RemotePlugin
{
    virtual ~RemotePlugin() = default;

    // The helper process died, hung or the plugin faulted: the plugin makes silence until restart() works. Safe to call from any thread.
    virtual bool hasFailed() const = 0;
    virtual juce::String failureReason() const = 0;
    virtual bool restart (juce::String& error) = 0;   // message thread: a new helper process with the same plugin and the last known state

    // The plugin's window lives in the helper process
    virtual void showEditor (const juce::String& title) = 0;
    virtual void closeEditor() = 0;
    virtual bool isEditorOpen() const = 0;

    virtual void killHelper() = 0;   // ends the helper process from outside, like the task manager does (for the automatic tests)

    std::function<void()> onFailed;         // message thread: the plugin has just failed
    std::function<void()> onEditorClosed;   // message thread: the user closed the plugin's window
};

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
    RemotePlugin* remote = nullptr;   // set when `instance` is a proxy for a plugin in a helper process (the same object)
    juce::PluginDescription description;
    std::atomic<bool> bypass { false };
    std::atomic<bool> editorOpen { false };   // its editor window is open: the engine keeps the plugin processing (many plugins say "audio engine inactive" otherwise)
    juce::AudioBuffer<float> scratch;   // processing buffer sized for the plugin's channel count
    juce::MidiBuffer emptyMidi;         // for effects: no notes (every plugin has its own, so tracks can be processed at the same time)

    // Set when the plugin crashed while it was processing sound (see guardedProcess): it is switched off for good (until the project is opened again,
    // or, for a plugin in a helper process, until it is restarted).
    std::atomic<bool> crashed { false };

    // (a plugin that crashed inside the program is not destroyed: that would likely crash again; it is left alone. A helper process is simply ended.)
    ~PluginSlot() { if (crashed.load() && remote == nullptr) instance.release(); }
};

// processBlock of a hosted plugin with a safety net: a crash inside the plugin (an access violation, a divide by zero) is caught instead of ending the
// program. The plugin is then marked as crashed and never called again; the caller makes silence (or passes the sound through). Windows only: elsewhere
// the plugin runs unguarded. A plugin in a helper process cannot take the program down; if the helper fails the proxy says so and the slot is marked
// the same way.
inline bool guardedProcess (PluginSlot& slot, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    if (slot.crashed.load())
        return false;
    if (slot.remote != nullptr)
    {
        slot.instance->processBlock (buffer, midi);
        if (slot.remote->hasFailed())
        {
            slot.crashed = true;
            return false;
        }
        return true;
    }
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
