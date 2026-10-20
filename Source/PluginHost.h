#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"

// Everything about finding, loading and showing VST3 plugins. The list of known plugins is
// kept in %APPDATA%\UFODAW\plugins.xml between runs.
class PluginHost : private juce::ChangeListener
{
public:
    PluginHost();
    ~PluginHost() override;

    juce::KnownPluginList& getList() { return knownPlugins; }

    // Where plugins run: in a helper process of their own, so a plugin that crashes, hangs or eats memory cannot take the program along (the track
    // goes silent and the user can restart the plugin), or inside the program (a little less memory and CPU). "Automatic" (the default) uses a helper
    // process for the plugins that are known to give trouble: ones that crashed before, and Waves. Remembered between runs.
    enum class SandboxMode { automatic = 0, always = 1, never = 2 };
    SandboxMode getSandboxMode() const;
    void setSandboxMode (SandboxMode);
    bool wouldSandbox (const juce::PluginDescription&) const;   // with the current setting

    // Loads a plugin and prepares it for the engine's current audio setup. Returns null and fills `error` on failure.
    std::shared_ptr<PluginSlot> createSlot (const juce::PluginDescription&, Engine&, juce::String& error);

    // Starts the plugin of a slot that failed (its helper process died or hung) again, with the state it had. False and `error` if that fails.
    bool restartSlot (PluginSlot&, juce::String& error);

    // Project files: a plugin is stored as its description plus its state.
    juce::var slotToVar (const PluginSlot&) const;
    std::shared_ptr<PluginSlot> slotFromVar (const juce::var&, Engine&, juce::String& error);

    // Plugin windows. The owner hears when an editor opens and closes (to make undo steps of what is done inside the plugin's own window).
    // (The window of a plugin in a helper process belongs to that process, but it is opened, closed and counted from here in the same way.)
    void showEditor (PluginSlot&);
    std::function<void (PluginSlot&)> onEditorOpened, onEditorClosed;
    void closeEditorsFor (const PluginSlot*);
    void closeAllEditors();

    // The "scan for plugins" window
    void showManager();

private:
    class EditorWindow;
    class ManagerWindow;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;   // known plugin list changed: save it

    juce::File listFile() const;
    juce::File blockedFile() const;
    juce::File riskyFile() const;
    bool isBlocked (const juce::PluginDescription&) const;
    bool isRisky (const juce::PluginDescription&) const;
    void markRisky (const juce::PluginDescription&);

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList knownPlugins;
    std::unique_ptr<juce::PropertiesFile> properties;
    juce::OwnedArray<EditorWindow> editors;
    std::vector<PluginSlot*> remoteEditors;   // slots whose window is open in a helper process
    std::unique_ptr<ManagerWindow> manager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
};
