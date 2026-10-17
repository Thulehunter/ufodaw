#include "PluginPanel.h"

namespace
{
    constexpr int panelWidth = 460;
    constexpr int rowH = 30;
    constexpr int maxRows = 6;
}

PluginPanel::PluginPanel (Track& t, Engine& e, PluginHost& h, std::function<void()> onBeginEdit)
    : track (t), engine (e), host (h), beginEdit (std::move (onBeginEdit))
{
    const bool isMidi = track.type == Track::Type::Midi;

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

        midiOutBox.addItem ("None - this track plays its own sound", 1);
        int selectedOut = 1;
        for (int i = 0; i < engine.getNumTracks(); ++i)
            if (auto* other = engine.getTrack (i); other != &track && other->type == Track::Type::Midi)
            {
                midiTargets.push_back (other);
                midiOutBox.addItem ("Send notes to: " + other->name, 100 + (int) midiTargets.size() - 1);
                if (track.midiOut == other)
                    selectedOut = 100 + (int) midiTargets.size() - 1;
            }
        midiOutBox.setSelectedId (selectedOut, juce::dontSendNotification);
        midiOutBox.setTooltip ("For a sequencer plugin (e.g. Stochas): the track whose synth should play the notes it makes");
        midiOutBox.onChange = [this] { chooseMidiOut(); };
        addAndMakeVisible (midiOutLabel);
        addAndMakeVisible (midiOutBox);
    }

    addAndMakeVisible (insertsLabel);
    hint.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.55f));
    addAndMakeVisible (hint);

    addButton.onClick = [this] { addEffectMenu(); };
    addAndMakeVisible (addButton);
    manageButton.onClick = [this] { host.showManager(); };
    addAndMakeVisible (manageButton);

    rebuild();
    setSize (panelWidth, (isMidi ? 90 : 0) + 40 + maxRows * rowH + 56);
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

void PluginPanel::chooseMidiOut()
{
    const int id = midiOutBox.getSelectedId();
    beginEdit();
    engine.setMidiOut (track, id >= 100 ? midiTargets[(size_t) (id - 100)] : nullptr);
}

void PluginPanel::addEffectMenu()
{
    juce::Array<juce::PluginDescription> effects;
    for (auto& d : host.getList().getTypes())
        if (! d.isInstrument)
            effects.add (d);

    juce::PopupMenu menu;
    for (int i = 0; i < effects.size(); ++i)
        menu.addItem (i + 1, effects[i].name + "  (" + effects[i].manufacturerName + ")");
    if (effects.isEmpty())
        menu.addItem (-1, "No effect plugins found - use \"Manage plugins...\"", false);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addButton),
                        [this, effects, safe = juce::Component::SafePointer<PluginPanel> (this)] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;

                            beginEdit();
                            juce::String error;
                            auto slot = host.createSlot (effects[result - 1], engine, error);
                            if (slot == nullptr)
                            {
                                alert ("Cannot load plugin", error);
                                return;
                            }
                            engine.addInsert (track, slot);
                            host.showEditor (*slot);
                            rebuild();
                        });
}

void PluginPanel::rebuild()
{
    rows.clear();

    for (int i = 0; i < (int) track.inserts.size(); ++i)
    {
        auto slot = track.inserts[(size_t) i];
        auto row = std::make_unique<Row>();
        row->name.setText (slot->description.name, juce::dontSendNotification);
        row->edit.onClick = [this, slot] { host.showEditor (*slot); };
        row->bypass.setToggleState (slot->bypass, juce::dontSendNotification);
        row->bypass.onClick = [this, slot, b = &row->bypass] { beginEdit(); slot->bypass = b->getToggleState(); };
        row->remove.onClick = [this, i, slot, safe = juce::Component::SafePointer<PluginPanel> (this)]
        {
            // Deferred: the button being clicked belongs to the row that gets rebuilt.
            juce::MessageManager::callAsync ([this, i, slot, safe]
            {
                if (safe == nullptr)
                    return;
                beginEdit();
                host.closeEditorsFor (slot.get());
                engine.removeInsert (track, i);
                rebuild();
            });
        };
        for (auto* c : std::initializer_list<juce::Component*> { &row->name, &row->edit, &row->bypass, &row->remove })
            addAndMakeVisible (c);
        rows.push_back (std::move (row));
    }

    hint.setText (rows.empty() ? "No effect plugins on this track." : "", juce::dontSendNotification);
    addButton.setEnabled ((int) rows.size() < maxRows);
    resized();
    repaint();
}

void PluginPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff24272e));
}

void PluginPanel::resized()
{
    auto area = getLocalBounds().reduced (12, 10);

    if (track.type == Track::Type::Midi)
    {
        auto line = area.removeFromTop (30);
        instrumentLabel.setBounds (line.removeFromLeft (74));
        instrumentEdit.setBounds (line.removeFromRight (50));
        line.removeFromRight (6);
        instrumentBox.setBounds (line);
        area.removeFromTop (6);

        auto outLine = area.removeFromTop (30);
        midiOutLabel.setBounds (outLine.removeFromLeft (74));
        midiOutBox.setBounds (outLine);
        area.removeFromTop (14);
    }

    insertsLabel.setBounds (area.removeFromTop (24));
    hint.setBounds (area.removeFromTop (rowH));

    area.translate (0, -rowH);   // the hint line and the first row share the same spot
    for (auto& row : rows)
    {
        auto line = area.removeFromTop (rowH).reduced (0, 2);
        row->remove.setBounds (line.removeFromRight (26));
        line.removeFromRight (4);
        row->bypass.setBounds (line.removeFromRight (74));
        row->edit.setBounds (line.removeFromRight (50));
        line.removeFromRight (6);
        row->name.setBounds (line);
    }
    area = getLocalBounds().reduced (12, 10).removeFromBottom (30);
    manageButton.setBounds (area.removeFromRight (150));
    area.removeFromRight (8);
    addButton.setBounds (area.removeFromRight (190));
}
