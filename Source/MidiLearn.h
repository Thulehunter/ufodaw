#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "Theme.h"

// MIDI learn: a knob, fader or button of a hardware controller (a MIDI controller number on a channel) is tied to something in the program: the volume,
// pan, mute or solo of a track, or any parameter of its effects, synth, sampler or plugins (the ones that can be automated). Moving the control then moves
// the parameter.
struct MidiMapping
{
    int cc = -1;                 // the controller number (-1 = not learned yet)
    int channel = 0;             // 1..16
    int trackIndex = 0;          // -1 = the master
    juce::String id;             // "volume", "pan", "mute", "solo", or an automation target id (see Engine::listAutomationTargets)
    unsigned seen[16] = {};      // (the last message of each channel that was applied)
};

class MidiLearnPanel : public juce::Component
{
public:
    struct Hooks
    {
        std::function<void (std::function<void (int cc, int channel)>)> startLearn;   // the next controller message the hardware sends comes back
        std::function<void()> stopLearn;
        std::function<void()> changed;                                                // the mappings were edited
    };

    MidiLearnPanel (Engine& e, std::vector<MidiMapping>& m, Hooks h) : engine (e), maps (m), hooks (std::move (h))
    {
        addButton.onClick = [this]
        {
            MidiMapping mm;
            mm.trackIndex = engine.getNumTracks() > 0 ? 0 : -1;
            mm.id = "volume";
            maps.push_back (mm);
            if (hooks.changed) hooks.changed();
            rebuild();
        };
        addAndMakeVisible (addButton);

        hint.setText ("1. Add a mapping and choose a track and what to control.  2. Press Learn, then move the knob or fader of your controller.\n"
                      "The control then moves that parameter. A mapping with a track or parameter that is gone does nothing.",
                      juce::dontSendNotification);
        hint.setFont (juce::FontOptions (12.0f));
        hint.setColour (juce::Label::textColourId, Theme::textFaint);
        hint.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (hint);
        rebuild();
    }

    ~MidiLearnPanel() override { if (learning >= 0 && hooks.stopLearn) hooks.stopLearn(); }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        auto head = r.removeFromTop (18);
        head.removeFromLeft (0);
        r.removeFromTop (4);
        for (auto& row : rows)
        {
            auto line = r.removeFromTop (28);
            row->track.setBounds (line.removeFromLeft (150));
            line.removeFromLeft (6);
            row->remove.setBounds (line.removeFromRight (26));
            line.removeFromRight (6);
            row->learn.setBounds (line.removeFromRight (130));
            line.removeFromRight (6);
            row->param.setBounds (line);
            r.removeFromTop (4);
        }
        addButton.setBounds (r.removeFromTop (26).removeFromLeft (150));
        r.removeFromTop (8);
        hint.setBounds (r.removeFromTop (40));
    }

private:
    struct Row
    {
        juce::ComboBox track, param;
        juce::TextButton learn, remove { "x" };
    };

    void fillParams (Row& row, int index)
    {
        row.param.clear (juce::dontSendNotification);
        row.param.addItem ("Volume", 1);
        row.param.addItem ("Pan", 2);
        row.param.addItem ("Mute", 3);
        row.param.addItem ("Solo", 4);
        auto& mm = maps[(size_t) index];
        auto* t = trackOf (mm);
        if (t != nullptr)
        {
            targets.clear();
            juce::String group;
            int id = 10;
            for (auto& tg : engine.listAutomationTargets (*t))
            {
                if (tg.group != group)
                {
                    group = tg.group;
                    row.param.addSeparator();
                    row.param.addSectionHeading (group);
                }
                row.param.addItem (tg.name, id);
                paramIds[(size_t) index].push_back ({ id, tg.id });
                ++id;
            }
        }
        // select the current one
        int selected = mm.id == "volume" ? 1 : mm.id == "pan" ? 2 : mm.id == "mute" ? 3 : mm.id == "solo" ? 4 : 0;
        for (auto& [id, name] : paramIds[(size_t) index])
            if (name == mm.id)
                selected = id;
        if (selected > 0)
            row.param.setSelectedId (selected, juce::dontSendNotification);
    }

    Track* trackOf (const MidiMapping& mm) const
    {
        return mm.trackIndex < 0 ? &engine.getMaster() : engine.getTrack (mm.trackIndex);
    }

    void rebuild()
    {
        rows.clear();
        paramIds.assign (maps.size(), {});
        for (size_t i = 0; i < maps.size(); ++i)
        {
            auto row = std::make_unique<Row>();
            auto* r = row.get();

            r->track.addItem ("Master", 1);
            for (int t = 0; t < engine.getNumTracks(); ++t)
                r->track.addItem (engine.getTrack (t)->name, t + 2);
            r->track.setSelectedId (maps[i].trackIndex + 2, juce::dontSendNotification);
            r->track.onChange = [this, i, r]
            {
                maps[i].trackIndex = r->track.getSelectedId() - 2;
                maps[i].id = "volume";
                if (hooks.changed) hooks.changed();
                juce::Component::SafePointer<MidiLearnPanel> safe (this);
                juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->rebuild(); });
            };
            addAndMakeVisible (r->track);

            fillParams (*r, (int) i);
            r->param.onChange = [this, i, r]
            {
                const int id = r->param.getSelectedId();
                if (id == 1) maps[i].id = "volume";
                else if (id == 2) maps[i].id = "pan";
                else if (id == 3) maps[i].id = "mute";
                else if (id == 4) maps[i].id = "solo";
                else
                    for (auto& [pid, name] : paramIds[i])
                        if (pid == id)
                            maps[i].id = name;
                if (hooks.changed) hooks.changed();
            };
            addAndMakeVisible (r->param);

            r->learn.setButtonText (learning == (int) i ? "Move a knob..." : maps[i].cc >= 0 ? "CC " + juce::String (maps[i].cc) + "  (ch " + juce::String (maps[i].channel) + ")" : "Learn");
            r->learn.setColour (juce::TextButton::buttonColourId, learning == (int) i ? Theme::orange : maps[i].cc >= 0 ? Theme::orangeDim : Theme::buttonOff);
            r->learn.setTooltip ("Press, then move a knob or fader (or press a button) on your controller");
            r->learn.onClick = [this, i]
            {
                if (learning == (int) i)   // pressed again: stop listening
                {
                    learning = -1;
                    if (hooks.stopLearn) hooks.stopLearn();
                    rebuild();
                    return;
                }
                learning = (int) i;
                juce::Component::SafePointer<MidiLearnPanel> safe (this);
                if (hooks.startLearn)
                    hooks.startLearn ([safe, i] (int cc, int channel)
                    {
                        if (safe == nullptr || i >= safe->maps.size())
                            return;
                        safe->maps[i].cc = cc;
                        safe->maps[i].channel = channel;
                        for (auto& s : safe->maps[i].seen) s = 0;
                        safe->learning = -1;
                        if (safe->hooks.changed) safe->hooks.changed();
                        safe->rebuild();
                    });
                rebuild();
            };
            addAndMakeVisible (r->learn);

            r->remove.setTooltip ("Remove this mapping");
            r->remove.onClick = [this, i]
            {
                juce::Component::SafePointer<MidiLearnPanel> safe (this);
                juce::MessageManager::callAsync ([safe, i]
                {
                    if (safe == nullptr || i >= safe->maps.size())
                        return;
                    safe->maps.erase (safe->maps.begin() + (long) i);
                    if (safe->learning >= 0 && safe->hooks.stopLearn)
                        safe->hooks.stopLearn();
                    safe->learning = -1;
                    if (safe->hooks.changed) safe->hooks.changed();
                    safe->rebuild();
                });
            };
            addAndMakeVisible (r->remove);
            rows.push_back (std::move (row));
        }
        setSize (620, 10 + 18 + 4 + (int) rows.size() * 32 + 26 + 8 + 40 + 10);
        resized();
        if (auto* window = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent()))   // (the window follows the number of rows)
            window->setContentComponentSize (getWidth(), getHeight());
    }

    Engine& engine;
    std::vector<MidiMapping>& maps;
    Hooks hooks;
    int learning = -1;
    std::vector<std::pair<int, juce::String>> targets;
    std::vector<std::vector<std::pair<int, juce::String>>> paramIds;   // per mapping: combo id -> parameter id
    std::vector<std::unique_ptr<Row>> rows;
    juce::TextButton addButton { "+ Add mapping" };
    juce::Label hint;
};

class MidiLearnWindow : public juce::DocumentWindow
{
public:
    MidiLearnWindow (MidiLearnPanel* panel, std::function<void()> onClosed)
        : juce::DocumentWindow ("MIDI controllers", Theme::background, juce::DocumentWindow::closeButton), closed (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (panel, true);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { if (closed) closed(); }

private:
    std::function<void()> closed;
};
