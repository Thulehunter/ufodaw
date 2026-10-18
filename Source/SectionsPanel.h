#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "Theme.h"

// The sections of the arrangement (verse, chorus...): a name, the bar it starts at and how many bars it lasts. They show as bands on the ruler.
// Go: move the playhead there. Loop: loop that part. Copy: a copy of the section right after it (everything later moves along).
// < >: swap with the neighbour (the clips go with the section; the sections must touch). x: remove the section (its clips stay).
class SectionsPanel : public juce::Component
{
public:
    struct Hooks
    {
        std::function<std::vector<ArrSection>()> fetch;                 // the sections as they are now
        std::function<void (std::vector<ArrSection>)> edited;           // names, bars, colours (one undo step per panel opening is taken by the owner)
        std::function<void (int index)> goTo, loop, copy, moveEarlier, moveLater;
    };

    static const std::array<juce::uint32, 8>& palette()
    {
        static const std::array<juce::uint32, 8> p = { 0xffd9702a, 0xffff5a47, 0xffffc933, 0xffe86a9a, 0xff9ad06a, 0xff4fb3bf, 0xff6a8cff, 0xffa07cff };
        return p;
    }

    explicit SectionsPanel (Hooks h) : hooks (std::move (h))
    {
        for (auto* l : { &nameHeader, &barHeader, &lenHeader })
        {
            l->setColour (juce::Label::textColourId, Theme::textDim);
            l->setFont (juce::FontOptions (12.0f));
            addAndMakeVisible (l);
        }
        nameHeader.setText ("Name", juce::dontSendNotification);
        barHeader.setText ("From bar", juce::dontSendNotification);
        lenHeader.setText ("Bars", juce::dontSendNotification);

        addButton.setTooltip ("A new section right after the last one");
        addButton.onClick = [this]
        {
            auto s = sections;
            ArrSection n;
            n.startBar = s.empty() ? 0 : s.back().startBar + s.back().bars;
            n.bars = s.empty() ? 8 : s.back().bars;
            n.name = "Section " + std::to_string (s.size() + 1);
            s.push_back (n);
            push (std::move (s));
        };
        addAndMakeVisible (addButton);

        hint.setText ("Sections show as coloured bands on the ruler. Copy puts a copy of a section (with its clips) after it and moves everything "
                      "later along; < and > swap a section with its neighbour.",
                      juce::dontSendNotification);
        hint.setFont (juce::FontOptions (11.5f));
        hint.setColour (juce::Label::textColourId, Theme::textFaint);
        hint.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (hint);

        reload();
    }

    // The list again from the owner (after an action that changed it)
    void reload()
    {
        sections = hooks.fetch ? hooks.fetch() : std::vector<ArrSection>();
        rebuild();
    }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        auto head = r.removeFromTop (18);
        head.removeFromLeft (26);
        nameHeader.setBounds (head.removeFromLeft (124));
        barHeader.setBounds (head.removeFromLeft (94));
        lenHeader.setBounds (head.removeFromLeft (80));
        r.removeFromTop (4);
        for (auto& row : rows)
        {
            auto line = r.removeFromTop (28);
            row->colour.setBounds (line.removeFromLeft (22).reduced (0, 3));
            line.removeFromLeft (4);
            row->name.setBounds (line.removeFromLeft (120).reduced (0, 2));
            line.removeFromLeft (4);
            row->bar.setBounds (line.removeFromLeft (90));
            line.removeFromLeft (4);
            row->bars.setBounds (line.removeFromLeft (76));
            line.removeFromLeft (8);
            row->go.setBounds (line.removeFromLeft (34).reduced (0, 2));
            row->loop.setBounds (line.removeFromLeft (46).reduced (0, 2));
            row->copy.setBounds (line.removeFromLeft (46).reduced (0, 2));
            line.removeFromLeft (4);
            row->earlier.setBounds (line.removeFromLeft (24).reduced (0, 2));
            row->later.setBounds (line.removeFromLeft (24).reduced (0, 2));
            row->remove.setBounds (line.removeFromLeft (24).reduced (0, 2));
            r.removeFromTop (4);
        }
        addButton.setBounds (r.removeFromTop (26).removeFromLeft (130));
        r.removeFromTop (6);
        hint.setBounds (r.removeFromTop (46));
    }

private:
    struct Row
    {
        juce::TextButton colour, go { "Go" }, loop { "Loop" }, copy { "Copy" }, earlier { "<" }, later { ">" }, remove { "x" };
        juce::TextEditor name;
        juce::Slider bar, bars;
    };

    void push (std::vector<ArrSection> s)
    {
        sections = std::move (s);
        if (hooks.edited)
            hooks.edited (sections);
        rebuild();
    }

    juce::Colour colourOf (size_t i) const
    {
        return juce::Colour (sections[i].colour != 0 ? sections[i].colour : palette()[i % palette().size()]);
    }

    void rebuild()
    {
        rows.clear();
        for (size_t i = 0; i < sections.size(); ++i)
        {
            auto row = std::make_unique<Row>();
            auto* r = row.get();

            r->colour.setColour (juce::TextButton::buttonColourId, colourOf (i));
            r->colour.setTooltip ("The colour of the section (click for the next one)");
            r->colour.onClick = [this, i]
            {
                auto s = sections;
                auto& pal = palette();
                size_t current = 0;
                for (size_t k = 0; k < pal.size(); ++k)
                    if (pal[k] == (s[i].colour != 0 ? s[i].colour : pal[i % pal.size()]))
                        current = k;
                s[i].colour = pal[(current + 1) % pal.size()];
                push (std::move (s));
            };
            addAndMakeVisible (r->colour);

            r->name.setText (juce::String (sections[i].name), false);
            r->name.setFont (juce::FontOptions (14.0f));
            r->name.setColour (juce::TextEditor::backgroundColourId, Theme::panelRaised);
            r->name.setColour (juce::TextEditor::textColourId, Theme::text);
            r->name.setColour (juce::TextEditor::outlineColourId, Theme::border);
            r->name.onTextChange = [this, i, r]
            {
                if (i < sections.size())
                {
                    sections[i].name = r->name.getText().trim().toStdString();   // (typing: the rows stay as they are)
                    if (hooks.edited)
                        hooks.edited (sections);
                }
            };
            addAndMakeVisible (r->name);

            for (auto* s : { &r->bar, &r->bars })
            {
                s->setSliderStyle (juce::Slider::IncDecButtons);
                s->setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Horizontal);
                s->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 36, 24);
            }
            r->bar.setRange (1.0, 999.0, 1.0);
            r->bar.setValue (sections[i].startBar + 1, juce::dontSendNotification);
            r->bar.onValueChange = [this, i, r]
            {
                auto s = sections;
                s[i].startBar = (int) r->bar.getValue() - 1;
                push (std::move (s));
            };
            r->bars.setRange (1.0, 128.0, 1.0);
            r->bars.setValue (sections[i].bars, juce::dontSendNotification);
            r->bars.onValueChange = [this, i, r]
            {
                auto s = sections;
                s[i].bars = (int) r->bars.getValue();
                push (std::move (s));
            };
            addAndMakeVisible (r->bar);
            addAndMakeVisible (r->bars);

            auto act = [this, i] (std::function<void (int)>& hook)
            {
                if (hook)
                    hook ((int) i);
                // later: the owner changed the list (and the buttons that were clicked are about to be replaced)
                juce::Component::SafePointer<SectionsPanel> safe (this);
                juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->reload(); });
            };
            r->go.onClick = [this, act] () mutable { act (hooks.goTo); };
            r->loop.onClick = [this, act] () mutable { act (hooks.loop); };
            r->copy.onClick = [this, act] () mutable { act (hooks.copy); };
            r->earlier.onClick = [this, act] () mutable { act (hooks.moveEarlier); };
            r->later.onClick = [this, act] () mutable { act (hooks.moveLater); };
            r->earlier.setTooltip ("Swap with the section before it (the clips go along)");
            r->later.setTooltip ("Swap with the section after it (the clips go along)");
            r->copy.setTooltip ("A copy of this section (and everything in it) right after it; what comes later moves along");
            r->loop.setTooltip ("Loop this part");
            r->go.setTooltip ("Move the playhead to the start of this section");
            r->earlier.setEnabled (i > 0);
            r->later.setEnabled (i + 1 < sections.size());
            r->remove.setTooltip ("Remove the section (the clips stay)");
            r->remove.onClick = [this, i]
            {
                juce::Component::SafePointer<SectionsPanel> safe (this);
                juce::MessageManager::callAsync ([safe, i]
                {
                    if (safe == nullptr || i >= safe->sections.size())
                        return;
                    auto s = safe->sections;
                    s.erase (s.begin() + (long) i);
                    safe->push (std::move (s));
                });
            };
            for (auto* b : { &r->go, &r->loop, &r->copy, &r->earlier, &r->later, &r->remove })
                addAndMakeVisible (b);

            rows.push_back (std::move (row));
        }
        setSize (590, 10 + 18 + 4 + (int) rows.size() * 32 + 26 + 6 + 46 + 10);
        resized();
    }

    Hooks hooks;
    std::vector<ArrSection> sections;
    std::vector<std::unique_ptr<Row>> rows;
    juce::Label nameHeader, barHeader, lenHeader, hint;
    juce::TextButton addButton { "+ Add section" };
};
