#include "PluginPanel.h"

namespace
{
    constexpr int panelWidth = 480;
    constexpr int rowH = 30;
    constexpr int sectionGap = 12;
}

PluginPanel::PluginPanel (Track& t, Engine& e, PluginHost& h, std::function<void()> onBeginEdit)
    : track (t), engine (e), host (h), beginEdit (std::move (onBeginEdit))
{
    isMidi = track.type == Track::Type::Midi;

    if (isMidi)
    {
        addAndMakeVisible (instrumentLabel);

        instrumentBox.addItem (track.kit != nullptr ? "Built-in drum kit" : "Built-in synth", 1);
        for (int pass = 0; pass < 2; ++pass)   // instruments first, then everything else (MIDI sequencers like Stochas)
        {
            bool headingAdded = false;
            for (auto& d : host.getList().getTypes())
                if (d.isInstrument == (pass == 0))
                {
                    if (pass == 1 && ! headingAdded)
                    {
                        instrumentBox.addSeparator();
                        instrumentBox.addSectionHeading ("Sequencers / MIDI effects / other");
                        headingAdded = true;
                    }
                    instruments.add (d);
                    instrumentBox.addItem (d.name + "  (" + d.manufacturerName + ")", 100 + instruments.size() - 1);
                }
        }
        int selected = 1;
        if (track.instrument != nullptr)
            for (int i = 0; i < instruments.size(); ++i)
                if (instruments[i].uniqueId == track.instrument->description.uniqueId
                    && instruments[i].name == track.instrument->description.name)
                    selected = 100 + i;
        instrumentBox.setSelectedId (selected, juce::dontSendNotification);
        instrumentBox.onChange = [this] { chooseInstrument(); };
        addAndMakeVisible (instrumentBox);

        instrumentEdit.onClick = [this]
        {
            if (track.instrument != nullptr)
                host.showEditor (*track.instrument);
        };
        addAndMakeVisible (instrumentEdit);

        midiOutButton.setTooltip ("For a sequencer plugin (e.g. Stochas): the tracks whose synths should play the notes it makes");
        midiOutButton.onClick = [this] { showMidiOutMenu(); };
        updateMidiOutText();
        addAndMakeVisible (midiOutLabel);
        addAndMakeVisible (midiOutButton);

        setupList (midiFxList, "MIDI effects (before the instrument)", "+ Add MIDI effect...", true, 3);
    }

    setupList (insertList, "Audio effects (after the built-in effects)", "+ Add effect plugin...", false, 5);
    manageButton.onClick = [this] { host.showManager(); };
    addAndMakeVisible (manageButton);

    setSize (panelWidth, layoutAll (false));
}

void PluginPanel::setupList (PluginList& list, const juce::String& title, const juce::String& addText, bool midi, int maxRows)
{
    list.midi = midi;
    list.maxRows = maxRows;
    list.heading.setText (title, juce::dontSendNotification);
    list.heading.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    list.hint.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.55f));
    list.addButton.setButtonText (addText);
    list.addButton.onClick = [this, &list] { addPluginMenu (list); };
    addAndMakeVisible (list.heading);
    addAndMakeVisible (list.hint);
    addAndMakeVisible (list.addButton);
    rebuild (list);
}

std::vector<std::shared_ptr<PluginSlot>>& PluginPanel::slotsOf (PluginList& list)
{
    return list.midi ? track.midiFx : track.inserts;
}

void PluginPanel::alert (const juce::String& title, const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, title, message);
}

void PluginPanel::chooseInstrument()
{
    const int id = instrumentBox.getSelectedId();
    beginEdit();

    if (track.instrument != nullptr)
        host.closeEditorsFor (track.instrument.get());

    if (id < 100)
    {
        engine.setInstrument (track, nullptr);
        return;
    }

    juce::String error;
    auto slot = host.createSlot (instruments[id - 100], engine, error);
    if (slot == nullptr)
    {
        alert ("Cannot load plugin", error);
        engine.setInstrument (track, nullptr);
        instrumentBox.setSelectedId (1, juce::dontSendNotification);
        return;
    }
    engine.setInstrument (track, slot);
    host.showEditor (*slot);   // so the sound can be set up right away
}

void PluginPanel::updateMidiOutText()
{
    if (track.midiOuts.empty())
    {
        midiOutButton.setButtonText ("None - this track plays its own sound");
        return;
    }
    juce::StringArray names;
    for (auto* target : track.midiOuts)
        names.add (target->name);
    midiOutButton.setButtonText ("Notes go to: " + names.joinIntoString (", "));
}

// Tick the tracks that should play this track's notes (one pick per opening of the menu).
void PluginPanel::showMidiOutMenu()
{
    std::vector<Track*> others;
    juce::PopupMenu menu;
    menu.addItem (1000, "None - this track plays its own sound", true, track.midiOuts.empty());
    menu.addSeparator();
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (auto* other = engine.getTrack (i); other != &track && other->type == Track::Type::Midi)
        {
            others.push_back (other);
            const bool ticked = std::find (track.midiOuts.begin(), track.midiOuts.end(), other) != track.midiOuts.end();
            menu.addItem ((int) others.size(), other->name, true, ticked);
        }
    if (others.empty())
        menu.addItem (-1, "Add another MIDI track first", false);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&midiOutButton),
                        [this, others, safe = juce::Component::SafePointer<PluginPanel> (this)] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            auto targets = track.midiOuts;
                            if (result == 1000)
                            {
                                targets.clear();
                            }
                            else
                            {
                                auto* picked = others[(size_t) result - 1];
                                auto it = std::find (targets.begin(), targets.end(), picked);
                                if (it == targets.end()) targets.push_back (picked); else targets.erase (it);
                            }
                            beginEdit();
                            engine.setMidiOuts (track, targets);
                            updateMidiOutText();
                        });
}

void PluginPanel::addPluginMenu (PluginList& list)
{
    juce::Array<juce::PluginDescription> candidates;
    for (auto& d : host.getList().getTypes())
        if (list.midi || ! d.isInstrument)   // MIDI effects can be flagged as anything; audio inserts are effects
            candidates.add (d);

    juce::PopupMenu menu;
    for (int i = 0; i < candidates.size(); ++i)
        menu.addItem (i + 1, candidates[i].name + "  (" + candidates[i].manufacturerName + ")");
    if (candidates.isEmpty())
        menu.addItem (-1, "No plugins found - use \"Manage plugins...\"", false);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&list.addButton),
                        [this, &list, candidates, safe = juce::Component::SafePointer<PluginPanel> (this)] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            beginEdit();
                            juce::String error;
                            auto slot = host.createSlot (candidates[result - 1], engine, error);
                            if (slot == nullptr)
                            {
                                alert ("Cannot load plugin", error);
                                return;
                            }
                            if (list.midi) engine.addMidiFx (track, slot); else engine.addInsert (track, slot);
                            host.showEditor (*slot);
                            rebuild (list);
                        });
}

void PluginPanel::rebuild (PluginList& list)
{
    list.rows.clear();

    auto& slots = slotsOf (list);
    for (int i = 0; i < (int) slots.size(); ++i)
    {
        auto slot = slots[(size_t) i];
        auto row = std::make_unique<Row>();
        row->name.setText (slot->description.name, juce::dontSendNotification);
        row->edit.onClick = [this, slot] { host.showEditor (*slot); };
        row->bypass.setToggleState (slot->bypass, juce::dontSendNotification);
        row->bypass.onClick = [this, slot, b = &row->bypass] { beginEdit(); slot->bypass = b->getToggleState(); };
        row->remove.onClick = [this, &list, i, slot, safe = juce::Component::SafePointer<PluginPanel> (this)]
        {
            // Deferred: the button being clicked belongs to the row that gets rebuilt.
            juce::MessageManager::callAsync ([this, &list, i, slot, safe]
            {
                if (safe == nullptr)
                    return;
                beginEdit();
                host.closeEditorsFor (slot.get());
                if (list.midi) engine.removeMidiFx (track, i); else engine.removeInsert (track, i);
                rebuild (list);
            });
        };
        for (auto* c : std::initializer_list<juce::Component*> { &row->name, &row->edit, &row->bypass, &row->remove })
            addAndMakeVisible (c);
        list.rows.push_back (std::move (row));
    }

    list.hint.setText (list.rows.empty() ? "None yet." : "", juce::dontSendNotification);
    list.addButton.setEnabled ((int) list.rows.size() < list.maxRows);
    setSize (panelWidth, layoutAll (false));   // the callout follows the new height
    resized();
    repaint();
}

void PluginPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff24272e));
}

// Lays everything out from the top; with apply == false it only measures. Returns the height needed.
int PluginPanel::layoutAll (bool apply)
{
    auto area = juce::Rectangle<int> (0, 0, getWidth() > 0 ? getWidth() : panelWidth, 100000).reduced (12, 10);
    auto place = [apply] (juce::Component& c, juce::Rectangle<int> r) { if (apply) c.setBounds (r); };

    auto layoutList = [&] (PluginList& list)
    {
        place (list.heading, area.removeFromTop (24));
        place (list.hint, area.withHeight (rowH));
        for (auto& row : list.rows)
        {
            auto line = area.removeFromTop (rowH).reduced (0, 2);
            place (row->remove, line.removeFromRight (26));
            line.removeFromRight (4);
            place (row->bypass, line.removeFromRight (74));
            place (row->edit, line.removeFromRight (50));
            line.removeFromRight (6);
            place (row->name, line);
        }
        if (list.rows.empty())
            area.removeFromTop (rowH);   // room for the "None yet." hint
        place (list.addButton, area.removeFromTop (28).removeFromLeft (200));
        area.removeFromTop (sectionGap);
    };

    if (isMidi)
    {
        auto line = area.removeFromTop (30);
        place (instrumentLabel, line.removeFromLeft (74));
        place (instrumentEdit, line.removeFromRight (50));
        line.removeFromRight (6);
        place (instrumentBox, line);
        area.removeFromTop (6);

        auto outLine = area.removeFromTop (30);
        place (midiOutLabel, outLine.removeFromLeft (74));
        place (midiOutButton, outLine);
        area.removeFromTop (sectionGap + 4);

        layoutList (midiFxList);
    }

    layoutList (insertList);
    place (manageButton, area.removeFromTop (28).removeFromRight (150));

    return area.getY() + 10 + 4;
}

void PluginPanel::resized()
{
    layoutAll (true);
}
