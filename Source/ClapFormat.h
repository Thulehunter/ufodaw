#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// CLAP plugin hosting (https://cleveraudio.org). JUCE can not host CLAP itself, so this is a plugin *format* of our own for JUCE's plugin classes:
// a ClapPluginFormat finds .clap files and a ClapPluginInstance wraps one plugin as a juce::AudioPluginInstance. Everything that works with plugins
// (the scan window, the plugin list, instrument and effect slots, automation, project files, delay compensation) then treats it like a VST3.
//
// Supported: instruments and effects with audio ports (mono / stereo main port, other ports get silence), note and MIDI input, parameters (also
// automation, with the plugin's own value text), state save / load, latency, tail, the transport (tempo, position, playing) and the plugin's window
// (embedded in the editor window, or its own floating window). Not supported: note expressions and per-note modulation, plugin-to-host note output,
// voice info, presets (the plugin's own preset browser works inside its window), 64 bit audio.
class ClapPluginFormat : public juce::AudioPluginFormat
{
public:
    ClapPluginFormat() = default;

    static constexpr const char* formatName = "CLAP";

    juce::String getName() const override { return formatName; }
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String& fileOrIdentifier) override;
    bool fileMightContainThisPluginType (const juce::String& fileOrIdentifier) override;
    juce::String getNameOfPluginFromIdentifier (const juce::String& fileOrIdentifier) override;
    bool pluginNeedsRescanning (const juce::PluginDescription&) override;
    bool doesPluginStillExist (const juce::PluginDescription&) override;
    bool canScanForPlugins() const override { return true; }
    bool isTrivialToScan() const override { return false; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool recursive, bool allowAsync = false) override;
    juce::FileSearchPath getDefaultLocationsToSearch() override;
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }

protected:
    void createPluginInstance (const juce::PluginDescription&, double initialSampleRate, int initialBufferSize, PluginCreationCallback) override;
};
