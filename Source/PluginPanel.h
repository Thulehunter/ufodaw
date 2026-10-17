#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "PluginHost.h"

// VST3 plugins of one track, shown in a callout from the track header; every change applies immediately.
//  - MIDI tracks: the instrument, where a plugin sequencer's notes go (MIDI out) and MIDI effects before the instrument
//  - every track: audio effect inserts after the built-in effects
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

    // A list of plugins with an "add" button: either the MIDI effects or the audio inserts of the track.
    struct PluginList
    {
        bool midi = false;
        int maxRows = 5;
        juce::Label heading, hint;
        std::vector<std::unique_ptr<Row>> rows;
        juce::TextButton addButton;
    };

    void setupList (PluginList&, const juce::String& title, const juce::String& addText, bool midi, int maxRows);
    std::vector<std::shared_ptr<PluginSlot>>& slotsOf (PluginList&);
    void rebuild (PluginList&);
    void addPluginMenu (PluginList&);
    void chooseInstrument();
    void showMidiOutMenu();
    void updateMidiOutText();
    int layoutAll (bool apply);
    void alert (const juce::String& title, const juce::String& message);

    Track& track;
    Engine& engine;
    PluginHost& host;
    std::function<void()> beginEdit;
    bool isMidi = false;

    juce::Label instrumentLabel { {}, "Instrument" };
    juce::ComboBox instrumentBox;
    juce::TextButton instrumentEdit { "Edit" };
    juce::Array<juce::PluginDescription> instruments;

    // Where the notes of a plugin sequencer (Stochas, ...) go: other MIDI tracks play them.
    juce::Label midiOutLabel { {}, "MIDI out" };
    juce::TextButton midiOutButton;

    PluginList midiFxList, insertList;
    juce::TextButton manageButton { "Manage plugins..." };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginPanel)
};
