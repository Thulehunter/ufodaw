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

    // Loads a plugin and prepares it for the engine's current audio setup. Returns null and fills `error` on failure.
    std::shared_ptr<PluginSlot> createSlot (const juce::PluginDescription&, Engine&, juce::String& error);

    // Project files: a plugin is stored as its description plus its state.
    juce::var slotToVar (const PluginSlot&) const;
    std::shared_ptr<PluginSlot> slotFromVar (const juce::var&, Engine&, juce::String& error);

    // Plugin windows. The owner hears when an editor opens and closes (to make undo steps of what is done inside the plugin's own window).
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
    bool isBlocked (const juce::PluginDescription&) const;

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList knownPlugins;
    std::unique_ptr<juce::PropertiesFile> properties;
    juce::OwnedArray<EditorWindow> editors;
    std::unique_ptr<ManagerWindow> manager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
};
