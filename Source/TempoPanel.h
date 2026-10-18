#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "TempoMap.h"
#include "Theme.h"

// The list of tempo / time signature sections: each starts at a bar and has its own BPM and time signature.
// Every change is applied at once (the whole panel is one undo step).
class TempoPanel : public juce::Component
{
public:
    TempoPanel (const TempoMap& current, std::function<void()> beginEdit, std::function<void (const TempoMap&)> apply)
        : sections (current.getSections()), begin (std::move (beginEdit)), applyMap (std::move (apply))
    {
        for (juce::Label* l : { &nameHeader, &barHeader, &bpmHeader, &meterHeader })
        {
            l->setColour (juce::Label::textColourId, Theme::textDim);
            l->setFont (juce::FontOptions (12.0f));
            addAndMakeVisible (l);
        }
        nameHeader.setText ("Name", juce::dontSendNotification);
        barHeader.setText ("From bar", juce::dontSendNotification);
        bpmHeader.setText ("Tempo (BPM)", juce::dontSendNotification);
        meterHeader.setText ("Time signature", juce::dontSendNotification);

        addButton.setTooltip ("A new section a few bars after the last one, starting with the same tempo and time signature");
        addButton.onClick = [this]
        {
            changed();
            TempoSection s = sections.back();
            s.bar = sections.back().bar + 4;
            s.name = "Section " + std::to_string (sections.size() + 1);
            sections.push_back (s);
            push();
            rebuild();
        };
        addAndMakeVisible (addButton);

        hint.setText ("Tempo is in quarter notes per minute. MIDI follows tempo changes; audio clips stay where they are in time.",
                      juce::dontSendNotification);
        hint.setFont (juce::FontOptions (11.5f));
        hint.setColour (juce::Label::textColourId, Theme::textFaint);
        hint.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (hint);

        rebuild();
    }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        auto head = r.removeFromTop (18);
        nameHeader.setBounds (head.removeFromLeft (124));
        barHeader.setBounds (head.removeFromLeft (92));
        bpmHeader.setBounds (head.removeFromLeft (110));
        meterHeader.setBounds (head.removeFromLeft (140));
        r.removeFromTop (4);
        for (auto& row : rows)
        {
            auto line = r.removeFromTop (28);
            row->name.setBounds (line.removeFromLeft (120).reduced (0, 2));
            line.removeFromLeft (4);
            row->bar.setBounds (line.removeFromLeft (88));
            line.removeFromLeft (4);
            row->bpm.setBounds (line.removeFromLeft (106));
            line.removeFromLeft (4);
            row->num.setBounds (line.removeFromLeft (56));
            line.removeFromLeft (14);   // the slash
            row->den.setBounds (line.removeFromLeft (56));
            line.removeFromLeft (8);
            row->remove.setBounds (line.removeFromLeft (26));
            r.removeFromTop (4);
        }
        addButton.setBounds (r.removeFromTop (26).removeFromLeft (130));
        r.removeFromTop (6);
        hint.setBounds (r.removeFromTop (34));
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (16.0f));
        for (auto& row : rows)
            g.drawText ("/", row->num.getRight(), row->num.getY(), 14, row->num.getHeight(), juce::Justification::centred);
    }

private:
    struct Row
    {
        juce::TextEditor name;
        juce::Slider bar, bpm;
        juce::ComboBox num, den;
        juce::TextButton remove { "x" };
    };

    void changed()
    {
        if (! began)
        {
            began = true;
            begin();
        }
    }

    // Reads the controls back into `sections`, applies the map and shows what the map made of it (values are clamped).
    void push()
    {
        TempoMap m;
        m.setSections (sections);
        sections = m.getSections();
        applyMap (m);
    }

    void rebuild()
    {
        rows.clear();
        for (size_t i = 0; i < sections.size(); ++i)
        {
            auto row = std::make_unique<Row>();
            auto* r = row.get();

            r->name.setText (juce::String (sections[i].name), false);
            r->name.setTextToShowWhenEmpty ("Name (verse, chorus...)", Theme::textFaint);
            r->name.setFont (juce::FontOptions (14.0f));
            r->name.setColour (juce::TextEditor::backgroundColourId, Theme::panelRaised);
            r->name.setColour (juce::TextEditor::textColourId, Theme::text);
            r->name.setColour (juce::TextEditor::outlineColourId, Theme::border);
            r->name.setTooltip ("What this section is called; shown on the ruler");
            r->name.onTextChange = [this, i, r] { changed(); sections[i].name = r->name.getText().trim().toStdString(); push(); };
            addAndMakeVisible (r->name);

            r->bar.setSliderStyle (juce::Slider::IncDecButtons);
            r->bar.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Horizontal);
            r->bar.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 40, 24);
            r->bar.setRange (1.0, 999.0, 1.0);
            r->bar.setValue (sections[i].bar + 1, juce::dontSendNotification);
            r->bar.setEnabled (i > 0);   // the first section always starts at bar 1
            r->bar.onValueChange = [this, i, r]
            {
                changed();
                const int lo = sections[i - 1].bar + 1;
                const int hi = i + 1 < sections.size() ? sections[i + 1].bar - 1 : 999;
                const int bar = juce::jlimit (lo, juce::jmax (lo, hi), (int) r->bar.getValue() - 1);
                sections[i].bar = bar;
                r->bar.setValue (bar + 1, juce::dontSendNotification);
                push();
            };
            addAndMakeVisible (r->bar);

            r->bpm.setSliderStyle (juce::Slider::LinearBar);
            r->bpm.setRange (20.0, 400.0, 0.1);
            r->bpm.setValue (sections[i].bpm, juce::dontSendNotification);
            r->bpm.setTooltip ("Drag, or double-click to type");
            r->bpm.onValueChange = [this, i, r] { changed(); sections[i].bpm = r->bpm.getValue(); push(); };
            addAndMakeVisible (r->bpm);

            for (int n = 1; n <= 16; ++n)
                r->num.addItem (juce::String (n), n);
            r->num.setSelectedId (sections[i].numerator, juce::dontSendNotification);
            r->num.onChange = [this, i, r] { changed(); sections[i].numerator = r->num.getSelectedId(); push(); };
            addAndMakeVisible (r->num);

            for (int d : { 2, 4, 8, 16 })
                r->den.addItem (juce::String (d), d);
            r->den.setSelectedId (sections[i].denominator, juce::dontSendNotification);
            r->den.onChange = [this, i, r] { changed(); sections[i].denominator = r->den.getSelectedId(); push(); };
            addAndMakeVisible (r->den);

            r->remove.setEnabled (i > 0);
            r->remove.setTooltip ("Remove this section");
            r->remove.onClick = [this, i]
            {
                changed();
                juce::Component::SafePointer<TempoPanel> safe (this);
                juce::MessageManager::callAsync ([safe, i]
                {
                    if (safe == nullptr || i >= safe->sections.size() || i == 0)
                        return;
                    safe->sections.erase (safe->sections.begin() + (long) i);
                    safe->push();
                    safe->rebuild();
                });
            };
            addAndMakeVisible (r->remove);

            rows.push_back (std::move (row));
        }

        setSize (528, 10 + 18 + 4 + (int) rows.size() * 32 + 26 + 6 + 34 + 10);
    }

    std::vector<TempoSection> sections;
    std::function<void()> begin;
    std::function<void (const TempoMap&)> applyMap;
    bool began = false;
    std::vector<std::unique_ptr<Row>> rows;
    juce::Label nameHeader, barHeader, bpmHeader, meterHeader, hint;
    juce::TextButton addButton { "+ Add section" };
};
