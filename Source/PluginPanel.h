#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "PluginHost.h"

// VST3 plugins of one track: the instrument (MIDI tracks) and the effect inserts.
// Shown in a callout from the track header; every change applies immediately.
class PluginPanel : public juce::Component
{
public:
    PluginPanel (Track&, Engine&, PluginHost&, std::function<void()> onBeginEdit);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Row
    {
        juce::Label name;
        juce::TextButton edit { "Edit" };
        juce::ToggleButton bypass { "Bypass" };
        juce::TextButton remove { "x" };
    };

    void rebuild();
    void chooseInstrument();
    void addEffectMenu();
    void alert (const juce::String& title, const juce::String& message);

    Track& track;
    Engine& engine;
    PluginHost& host;
    std::function<void()> beginEdit;

    juce::Label instrumentLabel { {}, "Instrument" };
    juce::ComboBox instrumentBox;
    juce::TextButton instrumentEdit { "Edit" };
    juce::Array<juce::PluginDescription> instruments;

    juce::Label insertsLabel { {}, "Effects (after the built-in chain)" };
    std::vector<std::unique_ptr<Row>> rows;
    juce::Label hint;
    juce::TextButton addButton { "+ Add effect plugin..." }, manageButton { "Manage plugins..." };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginPanel)
};
