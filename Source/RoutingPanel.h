#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "Theme.h"

// Where a track plays (the master, or a bus) and how much of it goes to each bus as a send.
class RoutingPanel : public juce::Component
{
public:
    RoutingPanel (Engine& e, Track& t, std::function<void()> beginEdit) : engine (e), track (t), begin (std::move (beginEdit))
    {
        buses = engine.getBuses();

        outLabel.setText ("Output", juce::dontSendNotification);
        outLabel.setColour (juce::Label::textColourId, Theme::textDim);
        addAndMakeVisible (outLabel);

        outBox.addItem ("Master", 1);
        int selected = 1;
        for (size_t i = 0; i < buses.size(); ++i)
        {
            outBox.addItem (buses[i]->name, (int) i + 2);
            if (track.output == buses[i])
                selected = (int) i + 2;
        }
        outBox.setSelectedId (selected, juce::dontSendNotification);
        outBox.onChange = [this]
        {
            begin();
            const int id = outBox.getSelectedId();
            engine.setTrackOutput (track, id >= 2 ? buses[(size_t) id - 2] : nullptr);
        };
        addAndMakeVisible (outBox);

        sendsLabel.setText ("Sends", juce::dontSendNotification);
        sendsLabel.setColour (juce::Label::textColourId, Theme::textDim);
        addAndMakeVisible (sendsLabel);

        for (auto* bus : buses)
        {
            auto row = std::make_unique<Row>();
            row->bus = bus;
            row->name.setText (bus->name, juce::dontSendNotification);
            row->name.setColour (juce::Label::textColourId, Theme::text);
            row->name.setFont (juce::FontOptions (13.0f));
            addAndMakeVisible (row->name);

            float level = 0.0f;
            bool pre = false;
            engine.getSend (track, *bus, level, pre);

            row->level.setSliderStyle (juce::Slider::LinearHorizontal);
            row->level.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            row->level.setRange (0.0, 1.5, 0.01);
            row->level.setValue (level, juce::dontSendNotification);
            row->level.setDoubleClickReturnValue (true, 0.0);
            row->level.textFromValueFunction = [] (double v)
            {
                return v < 0.005 ? juce::String ("off") : juce::String (20.0 * std::log10 (v), 1) + " dB";
            };
            row->level.setPopupDisplayEnabled (true, true, nullptr);
            row->level.setTooltip ("How much of this track goes to the bus (double-click: off)");
            auto* r = row.get();
            row->level.onDragStart = [this] { begin(); };
            row->level.onValueChange = [this, r] { engine.setSend (track, *r->bus, (float) r->level.getValue(), r->pre.getToggleState()); };
            addAndMakeVisible (row->level);

            row->pre.setButtonText ("Pre");
            row->pre.setToggleState (pre, juce::dontSendNotification);
            row->pre.setTooltip ("Pre-fader: the send ignores this track's fader (and pan)");
            row->pre.onClick = [this, r] { begin(); engine.setSend (track, *r->bus, (float) r->level.getValue(), r->pre.getToggleState()); };
            addAndMakeVisible (row->pre);

            rows.push_back (std::move (row));
        }

        if (buses.empty())
        {
            hint.setText ("No buses yet. Add one with  + Track  >  Bus, put an effect (reverb, delay) on it, then send tracks to it here.",
                          juce::dontSendNotification);
            hint.setFont (juce::FontOptions (12.5f));
            hint.setColour (juce::Label::textColourId, Theme::textFaint);
            hint.setJustificationType (juce::Justification::topLeft);
            addAndMakeVisible (hint);
        }

        setSize (300, 16 + 26 + 10 + 20 + (buses.empty() ? 52 : (int) buses.size() * 28) + 10);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        auto top = r.removeFromTop (26);
        outLabel.setBounds (top.removeFromLeft (60));
        outBox.setBounds (top);
        r.removeFromTop (10);
        sendsLabel.setBounds (r.removeFromTop (20));
        if (buses.empty())
        {
            hint.setBounds (r.removeFromTop (52));
            return;
        }
        for (auto& row : rows)
        {
            auto line = r.removeFromTop (28);
            row->name.setBounds (line.removeFromLeft (90));
            row->pre.setBounds (line.removeFromRight (48));
            row->level.setBounds (line);
        }
    }

private:
    struct Row
    {
        Track* bus = nullptr;
        juce::Label name;
        juce::Slider level;
        juce::ToggleButton pre;
    };

    Engine& engine;
    Track& track;
    std::function<void()> begin;
    std::vector<Track*> buses;
    std::vector<std::unique_ptr<Row>> rows;
    juce::Label outLabel, sendsLabel, hint;
    juce::ComboBox outBox;
};
