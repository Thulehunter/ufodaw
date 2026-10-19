#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "Theme.h"

// The clip launcher: a grid with a column per track and a row ("scene") per idea. Click a cell to start its clip; it repeats until another cell of
// the track is started or the track is stopped, and everything starts on the next beat or bar so it stays in time. The button at the start of a
// row starts the whole row. Right-click a cell to fill it (an audio file, a pattern, or the clip that is selected in the arrangement), rename it,
// clear it, or move it to the arrangement to edit it there. Clips in the grid are not on the timeline: they play on their own while the transport runs.
class ClipLauncherPanel : public juce::Component, private juce::Timer
{
public:
    struct Hooks
    {
        std::function<std::pair<Track*, int>()> selectedClip;   // the clip that is selected in the arrangement (track, clip id); null track = none
        std::function<void()> changed;                           // the cells changed (the project has unsaved changes)
        std::function<void()> timelineChanged;                   // a clip was moved onto the timeline
        std::function<double()> playheadSeconds;
    };

    ClipLauncherPanel (Engine& e, Hooks h) : engine (e), hooks (std::move (h)), grid (*this)
    {
        quantumBox.addItem ("Start on the next bar", 1);
        quantumBox.addItem ("Start on the next beat", 2);
        quantumBox.addItem ("Start at once", 3);
        quantumBox.setSelectedId (engine.launchQuantum.load() == (int) Engine::LaunchQuantum::Bar ? 1
                                  : engine.launchQuantum.load() == (int) Engine::LaunchQuantum::Beat ? 2 : 3, juce::dontSendNotification);
        quantumBox.onChange = [this]
        {
            const int id = quantumBox.getSelectedId();
            engine.launchQuantum = id == 1 ? (int) Engine::LaunchQuantum::Bar : id == 2 ? (int) Engine::LaunchQuantum::Beat : (int) Engine::LaunchQuantum::None;
        };
        addAndMakeVisible (quantumBox);

        stopAll.setTooltip ("Ends every clip at the next beat or bar");
        stopAll.onClick = [this] { engine.stopAllSlots(); };
        addAndMakeVisible (stopAll);

        addScene.onClick = [this] { engine.setNumScenes (engine.getNumScenes() + 1); refreshSlots(); layoutGrid(); };
        addAndMakeVisible (addScene);
        removeScene.setTooltip ("Removes the last row (only when it is empty)");
        removeScene.onClick = [this] { engine.setNumScenes (engine.getNumScenes() - 1); refreshSlots(); layoutGrid(); };
        addAndMakeVisible (removeScene);

        help.setText ("Click a cell to start it. Right-click a cell to fill it. The transport starts by itself.", juce::dontSendNotification);
        help.setColour (juce::Label::textColourId, Theme::textFaint);
        help.setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (help);

        viewport.setViewedComponent (&grid, false);
        viewport.setScrollBarsShown (true, true);
        addAndMakeVisible (viewport);

        refreshSlots();
        layoutGrid();
        setSize (860, 430);
        startTimerHz (20);
    }

    ~ClipLauncherPanel() override { stopTimer(); }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::background); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto top = r.removeFromTop (28);
        stopAll.setBounds (top.removeFromLeft (90));
        top.removeFromLeft (8);
        quantumBox.setBounds (top.removeFromLeft (190));
        top.removeFromLeft (8);
        addScene.setBounds (top.removeFromLeft (90));
        top.removeFromLeft (4);
        removeScene.setBounds (top.removeFromLeft (100));
        top.removeFromLeft (12);
        help.setBounds (top);
        r.removeFromTop (6);
        viewport.setBounds (r);
        layoutGrid();
    }

private:
    static constexpr int cellW = 118, cellH = 38, headerH = 46, sceneW = 44;

    struct Column
    {
        Track* track;
        std::vector<LaunchSlot> slots;
        Engine::SlotState state;
    };

    class Grid : public juce::Component
    {
    public:
        explicit Grid (ClipLauncherPanel& o) : owner (o) {}

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Theme::laneBg);
            const int scenes = owner.engine.getNumScenes();
            const double blink = 0.5 + 0.5 * std::sin (juce::Time::getMillisecondCounterHiRes() * 0.014);

            for (int s = 0; s < scenes; ++s)   // the scene buttons
            {
                const auto r = juce::Rectangle<int> (2, headerH + s * cellH + 2, sceneW - 4, cellH - 4);
                g.setColour (Theme::buttonBase);
                g.fillRoundedRectangle (r.toFloat(), 4.0f);
                g.setColour (Theme::text);
                g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
                g.drawText (juce::String (s + 1) + juce::String (juce::CharPointer_UTF8 (" \xe2\x96\xb6")), r, juce::Justification::centred);
            }

            for (int c = 0; c < (int) owner.columns.size(); ++c)
            {
                auto& col = owner.columns[(size_t) c];
                auto& t = *col.track;
                const auto colour = t.colour != 0 ? juce::Colour ((juce::uint32) t.colour)
                                                  : Theme::trackColour (t.type == Track::Type::Midi, t.kit != nullptr);
                const int x = sceneW + c * cellW;

                // the header: the name and a stop button
                g.setColour (Theme::rowDark);
                g.fillRect (x, 0, cellW - 2, headerH - 2);
                g.setColour (colour);
                g.fillRect (x, 0, cellW - 2, 3);
                g.setColour (Theme::text);
                g.setFont (juce::FontOptions (12.5f));
                g.drawText (t.name, x + 6, 5, cellW - 40, 20, juce::Justification::centredLeft, true);
                const auto stopR = juce::Rectangle<int> (x + cellW - 30, 4, 24, 20);
                g.setColour (col.state.playing >= 0 || col.state.queued >= 0 ? Theme::buttonBase : Theme::buttonOff);
                g.fillRoundedRectangle (stopR.toFloat(), 3.0f);
                g.setColour (Theme::text);
                g.fillRect (stopR.withSizeKeepingCentre (8, 8));
                g.setColour (Theme::textFaint);
                g.setFont (juce::FontOptions (11.0f));
                g.drawText (t.type == Track::Type::Midi ? (t.kit != nullptr ? "drums" : "MIDI") : "audio", x + 6, 26, cellW - 12, 14, juce::Justification::centredLeft);

                for (int s = 0; s < scenes; ++s)
                {
                    const auto r = juce::Rectangle<int> (x, headerH + s * cellH, cellW - 2, cellH - 2);
                    const bool filled = s < (int) col.slots.size() && col.slots[(size_t) s].used;
                    const bool playing = col.state.playing == s, queued = col.state.queued == s;
                    if (! filled)
                    {
                        g.setColour (Theme::rowDark.withAlpha (0.6f));
                        g.fillRoundedRectangle (r.toFloat().reduced (1.0f), 4.0f);
                        continue;
                    }
                    g.setColour (playing ? colour : colour.withMultipliedBrightness (0.55f).withMultipliedSaturation (0.8f));
                    g.fillRoundedRectangle (r.toFloat().reduced (1.0f), 4.0f);
                    if (playing)   // the position in the loop
                    {
                        g.setColour (juce::Colours::white.withAlpha (0.35f));
                        g.fillRect (r.getX() + 1, r.getBottom() - 5, (int) ((r.getWidth() - 2) * col.state.loopPosition), 3);
                    }
                    if (queued)
                    {
                        g.setColour (juce::Colours::white.withAlpha ((float) (0.3 + 0.7 * blink)));
                        g.drawRoundedRectangle (r.toFloat().reduced (1.5f), 4.0f, 2.0f);
                    }
                    g.setColour (playing ? juce::Colours::black.withAlpha (0.8f) : Theme::text);
                    g.setFont (juce::FontOptions (12.5f));
                    g.drawText (juce::String (juce::CharPointer_UTF8 (playing ? "\xe2\x96\xb6 " : "")) + col.slots[(size_t) s].name,
                                r.reduced (8, 0).withTrimmedBottom (4), juce::Justification::centredLeft, true);
                    if (playing && col.state.stopQueued)
                    {
                        g.setColour (juce::Colours::black.withAlpha ((float) (0.4 + 0.5 * blink)));
                        g.fillRect (r.getRight() - 14, r.getY() + 8, 6, 6);
                    }
                }
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const auto p = e.getPosition();
            const int scenes = owner.engine.getNumScenes();
            if (p.x < sceneW)
            {
                const int s = (p.y - headerH) / cellH;
                if (p.y >= headerH && s < scenes)
                    owner.engine.launchScene (s);
                return;
            }
            const int c = (p.x - sceneW) / cellW;
            if (c < 0 || c >= (int) owner.columns.size())
                return;
            auto& t = *owner.columns[(size_t) c].track;
            if (p.y < headerH)
            {
                if (p.x - sceneW - c * cellW > cellW - 34)
                    owner.engine.stopTrackSlots (t);
                return;
            }
            const int s = (p.y - headerH) / cellH;
            if (s >= scenes)
                return;
            const bool filled = s < (int) owner.columns[(size_t) c].slots.size() && owner.columns[(size_t) c].slots[(size_t) s].used;
            if (e.mods.isPopupMenu() || ! filled)
                owner.cellMenu (t, s, filled);
            else
                owner.engine.launchSlot (t, s);
        }

    private:
        ClipLauncherPanel& owner;
    };

    void cellMenu (Track& t, int scene, bool filled)
    {
        juce::PopupMenu m;
        const auto selected = hooks.selectedClip ? hooks.selectedClip() : std::make_pair<Track*, int> (nullptr, -1);
        const bool selectedHere = selected.first == &t && selected.second >= 0;
        if (filled)
        {
            m.addItem (1, "Rename...");
            m.addItem (2, "Move to the arrangement (to edit it there)");
            m.addItem (3, "Clear");
            m.addSeparator();
        }
        if (t.type == Track::Type::Audio)
            m.addItem (4, "Load an audio file...");
        else
            m.addItem (5, "New 4-bar pattern");
        m.addItem (6, "Use the clip that is selected in the arrangement", selectedHere);
        m.addItem (7, "Use the selected clip and take it off the arrangement", selectedHere);

        juce::Component::SafePointer<ClipLauncherPanel> safe (this);
        Track* tp = &t;
        m.showMenuAsync (juce::PopupMenu::Options(), [safe, tp, scene, selected] (int result)
        {
            if (safe == nullptr || result == 0)
                return;
            auto& self = *safe;
            auto& eng = self.engine;
            bool changed = true;
            if (result == 1)
                self.askName (*tp, scene);
            else if (result == 2)
            {
                const double at = self.hooks.playheadSeconds ? self.hooks.playheadSeconds() : 0.0;
                if (eng.moveSlotToTimeline (*tp, scene, at) && self.hooks.timelineChanged)
                    self.hooks.timelineChanged();
            }
            else if (result == 3)
                eng.clearSlot (*tp, scene);
            else if (result == 4)
            {
                self.chooser = std::make_unique<juce::FileChooser> ("Choose an audio file", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                                    "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
                self.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe, tp, scene] (const juce::FileChooser& fc)
                {
                    if (safe == nullptr || ! fc.getResult().existsAsFile())
                        return;
                    if (safe->engine.setSlotFromFile (*tp, scene, fc.getResult()))
                        safe->afterChange();
                });
                changed = false;
            }
            else if (result == 5)
            {
                const double bar = self.engine.getTempoMap().barSecondsAt (0.0);
                eng.newSlotMidi (*tp, scene, 4.0 * bar);
            }
            else if (result == 6 || result == 7)
            {
                if (eng.setSlotFromClip (*tp, scene, selected.second, result == 7) && result == 7 && self.hooks.timelineChanged)
                    self.hooks.timelineChanged();
            }
            if (changed)
                self.afterChange();
        });
    }

    void askName (Track& t, int scene)
    {
        auto* w = new juce::AlertWindow ("Rename", "Name of this clip:", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", "", "");
        w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<ClipLauncherPanel> safe (this);
        Track* tp = &t;
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, tp, scene] (int result)
        {
            const auto text = w->getTextEditorContents ("name").trim();
            delete w;
            if (result == 1 && text.isNotEmpty() && safe != nullptr)
            {
                safe->engine.renameSlot (*tp, scene, text);
                safe->afterChange();
            }
        }), false);
    }

    void afterChange()
    {
        refreshSlots();
        if (hooks.changed)
            hooks.changed();
        grid.repaint();
    }

    // The columns (the tracks that can hold clips) with a snapshot of their cells.
    void refreshSlots()
    {
        columns.clear();
        for (int i = 0; i < engine.getNumTracks(); ++i)
            if (auto* t = engine.getTrack (i); t != nullptr && ! t->bus)
                columns.push_back ({ t, engine.getSlots (*t), engine.getSlotState (*t) });
    }

    void layoutGrid()
    {
        grid.setSize (juce::jmax (viewport.getWidth() - 14, sceneW + (int) columns.size() * cellW + 4),
                      juce::jmax (viewport.getHeight() - 14, headerH + engine.getNumScenes() * cellH + 4));
    }

    void timerCallback() override
    {
        // the tracks or the number of rows may have changed (another tab, a new track): look again now and then
        const int tracksNow = engine.getNumTracks();
        const bool structural = tracksNow != lastTracks || engine.getNumScenes() != lastScenes || ++tick % 10 == 0;
        if (structural)
        {
            lastTracks = tracksNow;
            lastScenes = engine.getNumScenes();
            refreshSlots();
            layoutGrid();
        }
        else
            for (auto& c : columns)
                c.state = engine.getSlotState (*c.track);
        grid.repaint();
    }

    Engine& engine;
    Hooks hooks;
    std::vector<Column> columns;
    Grid grid;
    juce::Viewport viewport;
    juce::TextButton stopAll { "Stop all" }, addScene { "+ Row" }, removeScene { "- Last row" };
    juce::ComboBox quantumBox;
    juce::Label help;
    std::unique_ptr<juce::FileChooser> chooser;
    int lastTracks = -1, lastScenes = -1, tick = 0;
};

class ClipLauncherWindow : public juce::DocumentWindow
{
public:
    ClipLauncherWindow (ClipLauncherPanel* panel, std::function<void()> onClosed)
        : juce::DocumentWindow ("Clip launcher", Theme::background, juce::DocumentWindow::closeButton), closed (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (panel, true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { if (closed) closed(); }

private:
    std::function<void()> closed;
};
