#include "PluginPanel.h"
#include "PluginSearch.h"
#include "Theme.h"

namespace
{
    constexpr int panelWidth = 500;
    constexpr int rowH = 30;
    constexpr int sectionGap = 12;
}

PluginPanel::PluginPanel (Track& t, Engine& e, PluginHost& h, std::function<void()> onBeginEdit)
    : track (t), engine (e), host (h), beginEdit (std::move (onBeginEdit))
{
    isMidi = track.type == Track::Type::Midi;

    if (isMidi)
    {
        // role: instrument or sequencer
        roleBox.addItem ("Instrument - makes the sound", 1);
        roleBox.addItem ("Sequencer - sends notes to other tracks", 2);
        roleBox.setSelectedId (track.sequencer ? 2 : 1, juce::dontSendNotification);
        roleBox.onChange = [this] { roleChanged(); };
        addAndMakeVisible (roleLabel);
        addAndMakeVisible (roleBox);

        instrumentLabel.setText (track.sequencer ? "Plugin" : "Instrument", juce::dontSendNotification);
        addAndMakeVisible (instrumentLabel);

        for (int pass = 0; pass < 2; ++pass)   // instruments first, then everything else (effects, MIDI tools)
            for (auto& d : host.getList().getTypes())
                if (d.isInstrument == (pass == 0))
                    instruments.add (d);

        instrumentButton.setTooltip ("Pick the instrument: click, then type part of a name to search");
        instrumentButton.onClick = [this] { openInstrumentSearch(); };
        updateInstrumentButton();
        addAndMakeVisible (instrumentButton);

        instrumentEdit.onClick = [this]
        {
            if (track.instrument != nullptr)
                host.showEditor (*track.instrument);
        };
        addAndMakeVisible (instrumentEdit);

        // MIDI input: every input, or one device; and every channel, or one
        inputBox.addItem ("All MIDI inputs", 1);
        int selectedInput = 1;
        for (auto& d : juce::MidiInput::getAvailableDevices())
        {
            inputIds.add (d.identifier);
            inputBox.addItem (d.name, 1 + inputIds.size());
            if (d.identifier == track.midiInputId)
                selectedInput = 1 + inputIds.size();
        }
        if (track.midiInputId.isNotEmpty() && selectedInput == 1)   // chosen earlier, but not connected now
        {
            inputIds.add (track.midiInputId);
            inputBox.addItem ("(not connected)", 1 + inputIds.size());
            selectedInput = 1 + inputIds.size();
        }
        inputBox.setSelectedId (selectedInput, juce::dontSendNotification);
        inputBox.setTooltip ("The track plays and records only what this input sends. Arm it with the red dot, or select its clip.");
        inputBox.onChange = [this]
        {
            beginEdit();
            const int id = inputBox.getSelectedId();
            track.midiInputId = id >= 2 ? inputIds[id - 2] : juce::String();
        };
        addAndMakeVisible (inputLabel);
        addAndMakeVisible (inputBox);

        inputChannelBox.addItem ("All channels", 1);
        for (int ch = 1; ch <= 16; ++ch)
            inputChannelBox.addItem ("Channel " + juce::String (ch), 1 + ch);
        inputChannelBox.setSelectedId (1 + juce::jlimit (0, 16, track.midiInputChannel), juce::dontSendNotification);
        inputChannelBox.onChange = [this] { beginEdit(); track.midiInputChannel = inputChannelBox.getSelectedId() - 1; };
        addAndMakeVisible (inputChannelBox);

        routeHeading.setText ("Play the notes on these tracks", juce::dontSendNotification);
        routeHeading.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        routeHint.setText ("Channel: \"All\" passes the notes on unchanged. A channel number sends only that MIDI channel "
                           "(e.g. one Stochas track per synth), delivered to the synth as channel 1.",
                           juce::dontSendNotification);
        routeHint.setFont (juce::FontOptions (11.5f));
        routeHint.setMinimumHorizontalScale (1.0f);
        routeHint.setColour (juce::Label::textColourId, Theme::text.withAlpha (0.55f));
        addChildComponent (routeHeading);
        addChildComponent (routeHint);
        rebuildRoutes();

        externalHeading.setText ("External MIDI instruments (hardware)", juce::dontSendNotification);
        externalHeading.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        externalHint.setText ("Plays this track's notes (or those of its sequencer plugin) on hardware: connect the interface's MIDI OUT "
                              "to the synth's MIDI IN. The track then makes no sound itself; record the synth's audio through an audio input.",
                              juce::dontSendNotification);
        externalHint.setFont (juce::FontOptions (11.5f));
        externalHint.setMinimumHorizontalScale (1.0f);
        externalHint.setColour (juce::Label::textColourId, Theme::text.withAlpha (0.55f));
        addAndMakeVisible (externalHeading);
        addAndMakeVisible (externalHint);
        rebuildExternal();

        setupList (midiFxList, "MIDI effects (before the instrument)", "+ Add MIDI effect...", true, 3);
    }

    setupList (insertList, "Audio effects (after the built-in effects)", "+ Add effect plugin...", false, 5);
    manageButton.onClick = [this] { host.showManager(); };
    addAndMakeVisible (manageButton);

    refreshLayout();
}

void PluginPanel::setupList (PluginList& list, const juce::String& title, const juce::String& addText, bool midi, int maxRows)
{
    list.midi = midi;
    list.maxRows = maxRows;
    list.heading.setText (title, juce::dontSendNotification);
    list.heading.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    list.hint.setColour (juce::Label::textColourId, Theme::text.withAlpha (0.55f));
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

void PluginPanel::refreshLayout()
{
    setSize (panelWidth, layoutAll (false));   // the callout follows the new height
    resized();
    repaint();
}

//==============================================================================
void PluginPanel::updateInstrumentButton()
{
    instrumentButton.setButtonText (track.instrument != nullptr
                                        ? track.instrument->description.name + "  (" + track.instrument->description.manufacturerName + ")"
                                        : track.kit != nullptr ? juce::String ("Built-in drum kit")
                                                               : track.sampler != nullptr ? juce::String ("Built-in sampler") : juce::String ("Built-in synth"));
}

void PluginPanel::openInstrumentSearch()
{
    const juce::String builtIn = track.kit != nullptr ? "Built-in drum kit" : track.sampler != nullptr ? "Built-in sampler" : "Built-in synth";

    // (the callbacks only use things that outlive this panel: the panel may be closed by the time something is picked)
    auto* tr = &track;
    auto* eng = &engine;
    auto* h = &host;
    auto begin = beginEdit;
    auto candidates = instruments;
    juce::Component::SafePointer<PluginPanel> safe (this);

    juce::CallOutBox::launchAsynchronously (std::make_unique<PluginSearchPanel> (instruments, builtIn,
        [tr, eng, h, begin, candidates, safe] (int index)
        {
            begin();
            if (tr->instrument != nullptr)
                h->closeEditorsFor (tr->instrument.get());

            if (index < 0 || index >= candidates.size())
            {
                eng->setInstrument (*tr, nullptr);   // back to the built-in sound
            }
            else
            {
                juce::String error;
                auto slot = h->createSlot (candidates[index], *eng, error);
                if (slot == nullptr)
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Cannot load plugin", error);
                    eng->setInstrument (*tr, nullptr);
                }
                else
                {
                    eng->setInstrument (*tr, slot);
                    h->showEditor (*slot);   // so the sound can be set up right away
                }
            }
            if (safe != nullptr)
                safe->updateInstrumentButton();
        }),
        instrumentButton.getScreenBounds(), nullptr);
}

void PluginPanel::roleChanged()
{
    const bool sequencer = roleBox.getSelectedId() == 2;
    beginEdit();
    track.sequencer = sequencer;
    if (! sequencer)
        engine.setMidiOuts (track, {});   // an instrument plays its own sound again
    instrumentLabel.setText (sequencer ? "Plugin" : "Instrument", juce::dontSendNotification);
    rebuildRoutes();
    refreshLayout();
}

// One row per other MIDI track: a tick to play the notes there, and the channel it listens to.
void PluginPanel::rebuildRoutes()
{
    routeRows.clear();

    const bool show = isMidi && track.sequencer;
    routeHeading.setVisible (show);
    routeHint.setVisible (show);
    if (! show)
        return;

    for (int i = 0; i < engine.getNumTracks(); ++i)
    {
        auto* other = engine.getTrack (i);
        if (other == &track || other->type != Track::Type::Midi)
            continue;

        auto row = std::make_unique<RouteRow>();
        row->target = other;
        row->use.setButtonText (other->name);

        row->channel.addItem ("All channels", 1);
        for (int ch = 1; ch <= 16; ++ch)
            row->channel.addItem ("Channel " + juce::String (ch), ch + 1);
        row->channel.setSelectedId (1, juce::dontSendNotification);

        for (auto& route : track.midiOuts)
            if (route.target == other)
            {
                row->use.setToggleState (true, juce::dontSendNotification);
                row->channel.setSelectedId (route.channel + 1, juce::dontSendNotification);
            }

        row->use.onClick = [this] { routesChanged(); };
        row->channel.onChange = [this] { routesChanged(); };
        addAndMakeVisible (row->use);
        addAndMakeVisible (row->channel);
        routeRows.push_back (std::move (row));
    }
}

// One row per MIDI output device of this computer: a tick to play on it, the channel to take, the channel to send.
void PluginPanel::rebuildExternal()
{
    externalRows.clear();

    for (auto& device : juce::MidiOutput::getAvailableDevices())
    {
        auto row = std::make_unique<ExternalRow>();
        row->id = device.identifier;
        row->name = device.name;
        row->use.setButtonText (device.name);

        row->from.addItem ("All channels", 1);
        for (int ch = 1; ch <= 16; ++ch)
            row->from.addItem ("From channel " + juce::String (ch), ch + 1);
        row->from.setSelectedId (1, juce::dontSendNotification);

        row->to.addItem ("Keep channel", 1);
        for (int ch = 1; ch <= 16; ++ch)
            row->to.addItem ("Send as channel " + juce::String (ch), ch + 1);
        row->to.setSelectedId (2, juce::dontSendNotification);   // channel 1: what most synths listen to

        for (auto& route : track.externalOuts)
            if (route.deviceId == device.identifier || route.deviceName == device.name)
            {
                row->use.setToggleState (true, juce::dontSendNotification);
                row->from.setSelectedId (route.channel + 1, juce::dontSendNotification);
                row->to.setSelectedId (route.outChannel + 1, juce::dontSendNotification);
            }

        row->use.onClick = [this] { externalChanged(); };
        row->from.onChange = [this] { externalChanged(); };
        row->to.onChange = [this] { externalChanged(); };
        addAndMakeVisible (row->use);
        addAndMakeVisible (row->from);
        addAndMakeVisible (row->to);
        externalRows.push_back (std::move (row));
    }

    if (externalRows.empty())
        externalHint.setText ("No MIDI output devices found. Connect your interface or synth (and install its driver), then open this panel again.",
                              juce::dontSendNotification);
}

void PluginPanel::externalChanged()
{
    std::vector<ExternalRoute> routes;
    for (auto& row : externalRows)
        if (row->use.getToggleState())
        {
            ExternalRoute r;
            r.deviceId = row->id;
            r.deviceName = row->name;
            r.channel = row->from.getSelectedId() - 1;      // id 1 = every channel = 0
            r.outChannel = row->to.getSelectedId() - 1;     // id 1 = keep the channel = 0
            routes.push_back (r);
        }

    beginEdit();
    engine.setExternalOuts (track, routes);

    // a device that could not be opened (unplugged, or another program has it) is switched off again, with an explanation
    for (auto& route : track.externalOuts)
        if (route.port < 0)
            for (auto& row : externalRows)
                if (row->id == route.deviceId)
                {
                    row->use.setToggleState (false, juce::dontSendNotification);
                    alert ("Cannot open " + row->name, "The MIDI output could not be opened. It may be unplugged, or in use by another program.");
                }

    std::vector<ExternalRoute> working;
    for (auto& route : track.externalOuts)
        if (route.port >= 0)
            working.push_back (route);
    if (working.size() != track.externalOuts.size())
        engine.setExternalOuts (track, working);
}

void PluginPanel::routesChanged()
{
    std::vector<MidiRoute> routes;
    for (auto& row : routeRows)
        if (row->use.getToggleState())
            routes.push_back ({ row->target, row->channel.getSelectedId() - 1 });   // id 1 = all channels = 0

    beginEdit();
    engine.setMidiOuts (track, routes);
}

//==============================================================================
void PluginPanel::addPluginMenu (PluginList& list)
{
    juce::Array<juce::PluginDescription> candidates;
    for (auto& d : host.getList().getTypes())
        if (list.midi || ! d.isInstrument)   // MIDI effects can be flagged as anything; audio inserts are effects
            candidates.add (d);

    if (candidates.isEmpty())
    {
        alert ("No plugins found", "Use \"Manage plugins...\" (or Plugins... at the bottom right of the window) to scan for VST3 plugins first.");
        return;
    }

    // a searchable list: type part of a name, a maker or a category
    auto* tr = &track;
    auto* eng = &engine;
    auto* h = &host;
    auto begin = beginEdit;
    const bool midi = list.midi;
    PluginList* listPtr = &list;
    juce::Component::SafePointer<PluginPanel> safe (this);

    juce::CallOutBox::launchAsynchronously (std::make_unique<PluginSearchPanel> (candidates, juce::String(),
        [tr, eng, h, begin, candidates, midi, listPtr, safe] (int index)
        {
            if (index < 0 || index >= candidates.size())
                return;

            begin();
            juce::String error;
            auto slot = h->createSlot (candidates[index], *eng, error);
            if (slot == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Cannot load plugin", error);
                return;
            }
            if (midi) eng->addMidiFx (*tr, slot); else eng->addInsert (*tr, slot);
            h->showEditor (*slot);
            if (safe != nullptr)
                safe->rebuild (*listPtr);   // (the panel's list of plugins, if the panel is still open)
        }),
        list.addButton.getScreenBounds(), nullptr);
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
    refreshLayout();
}

void PluginPanel::paint (juce::Graphics& g)
{
    g.fillAll (Theme::panel);
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
        auto roleLine = area.removeFromTop (30);
        place (roleLabel, roleLine.removeFromLeft (74));
        place (roleBox, roleLine);
        area.removeFromTop (6);

        auto line = area.removeFromTop (30);
        place (instrumentLabel, line.removeFromLeft (74));
        place (instrumentEdit, line.removeFromRight (50));
        line.removeFromRight (6);
        place (instrumentButton, line);
        area.removeFromTop (6);

        auto inputLine = area.removeFromTop (30);
        place (inputLabel, inputLine.removeFromLeft (74));
        place (inputChannelBox, inputLine.removeFromRight (120));
        inputLine.removeFromRight (6);
        place (inputBox, inputLine);
        area.removeFromTop (8);

        if (track.sequencer)
        {
            place (routeHeading, area.removeFromTop (22));
            place (routeHint, area.removeFromTop (48));
            if (routeRows.empty())
                area.removeFromTop (rowH);
            for (auto& row : routeRows)
            {
                auto r = area.removeFromTop (rowH).reduced (0, 2);
                place (row->use, r.removeFromLeft (r.getWidth() / 2));
                place (row->channel, r);
            }
            if (routeRows.empty())
            {
                // nothing to route to yet
                routeHint.setText ("Add another MIDI track (+ MIDI or + Drums) to send these notes to.", juce::dontSendNotification);
            }
        }
        area.removeFromTop (sectionGap);

        // hardware MIDI outputs
        place (externalHeading, area.removeFromTop (22));
        place (externalHint, area.removeFromTop (48));
        for (auto& row : externalRows)
        {
            auto r = area.removeFromTop (rowH).reduced (0, 2);
            const int third = r.getWidth() / 3;
            place (row->use, r.removeFromLeft (third + 20));
            place (row->from, r.removeFromLeft ((r.getWidth() - 4) / 2));
            r.removeFromLeft (4);
            place (row->to, r);
        }
        area.removeFromTop (sectionGap);

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
