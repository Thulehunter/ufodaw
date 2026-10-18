#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "Theme.h"

// The mixer: one channel strip per track (name, output, sends, level meter, volume fader, pan, mute, solo) and the master on the right.
// It works on the same values as the track headers, so the two always agree; every change is one undo step.
// Output = where the track plays (the master or a bus); Sends = how much of it also goes to each bus (reverb, delay...). Pre-fader sends
// are set in the track header's Out button.
class MixerPanel : public juce::Component, private juce::Timer
{
public:
    static constexpr int stripWidth = 86, stripHeight = 392, masterWidth = 96;
    static constexpr int maxSendRows = 4, sendRowHeight = 32;

    MixerPanel (Engine& e, std::function<void()> onBeginEdit,
                std::function<void (Track&, juce::Component&)> onFullRouting = nullptr)
        : engine (e), beginEdit (std::move (onBeginEdit)), fullRouting (std::move (onFullRouting))
    {
        rebuild();
        startTimerHz (30);
    }

    ~MixerPanel() override { stopTimer(); }

    // After the tracks were replaced (an undo, a new project): the strips point at the new ones
    void rebuild()
    {
        strips.clear();
        shownTracks.clear();
        buses = engine.getBuses();
        for (int i = 0; i < engine.getNumTracks(); ++i)
            addStrip (*engine.getTrack (i), false);
        addStrip (engine.getMaster(), true);
        for (auto& s : strips)
            shownTracks.push_back (s->track);
        busSignature = busNames();
        const int nShown = juce::jmin (maxSendRows, (int) buses.size());
        routingHeight = buses.empty() ? 0 : 28 + 4 + nShown * sendRowHeight + ((int) buses.size() > maxSendRows ? 26 : 0);
        setSize (juce::jmax (stripWidth * 2, (int) strips.size() * stripWidth + (masterWidth - stripWidth) + 16), stripHeight + routingHeight);
        resized();
        repaint();
    }

    // Forget the tracks (they are about to be replaced)
    void detach() { strips.clear(); shownTracks.clear(); }

    void openFullRouting (Track& t, juce::Component& anchor) { if (fullRouting) fullRouting (t, anchor); }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::background); }

    void resized() override
    {
        int x = 8;
        for (auto& s : strips)
        {
            const int w = s->isMaster ? masterWidth : stripWidth;
            s->setBounds (x, 8, w - 4, getHeight() - 16);
            x += w;
        }
    }

private:
    struct PanSlider : public juce::Slider
    {
        double snapValue (double attempted, DragMode) override
        {
            return std::abs (attempted) < 0.03 ? 0.0 : attempted;   // (a notch at the centre)
        }
    };

    struct Strip : public juce::Component
    {
        Strip (Track& t, bool master, MixerPanel& o) : track (&t), isMaster (master), owner (o)
        {
            if (! master && ! o.buses.empty())
                buildRouting();
            name.setJustificationType (juce::Justification::centred);
            name.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            name.setColour (juce::Label::textColourId, master ? Theme::yellow : Theme::text);
            name.setText (master ? juce::String ("MASTER") : t.name, juce::dontSendNotification);
            if (! master)
            {
                name.setEditable (false, true, true);   // double-click renames the track
                name.setTooltip ("Double-click to rename");
                name.onTextChange = [this]
                {
                    auto text = name.getText().trim();
                    if (text.isEmpty() || text == track->name)
                    {
                        name.setText (track->name, juce::dontSendNotification);
                        return;
                    }
                    owner.beginEdit();
                    track->name = text;
                };
            }
            addAndMakeVisible (name);

            fader.setSliderStyle (juce::Slider::LinearVertical);
            fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            fader.setRange (0.0, 1.5, 0.001);
            fader.setSkewFactorFromMidPoint (0.7);
            fader.setDoubleClickReturnValue (true, 0.8);
            fader.setValue (t.gain, juce::dontSendNotification);
            fader.setPopupDisplayEnabled (true, true, nullptr);
            fader.textFromValueFunction = [] (double v) { return v < 0.001 ? juce::String ("-inf dB") : juce::String (20.0 * std::log10 (v), 1) + " dB"; };
            fader.onDragStart = [this] { owner.beginEdit(); };
            fader.onValueChange = [this]
            {
                if (! fader.isMouseButtonDown())
                    owner.beginEdit();   // (typed or scrolled: one step per change)
                track->gain = (float) fader.getValue();
            };
            addAndMakeVisible (fader);

            if (! master)
            {
                pan.setSliderStyle (juce::Slider::LinearHorizontal);
                pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
                pan.setRange (-1.0, 1.0, 0.01);
                pan.setDoubleClickReturnValue (true, 0.0);
                pan.setValue (t.pan, juce::dontSendNotification);
                pan.setPopupDisplayEnabled (true, true, nullptr);
                pan.textFromValueFunction = [] (double v)
                {
                    const int percent = (int) std::round (std::abs (v) * 100.0);
                    return percent == 0 ? juce::String ("Center") : (v < 0 ? "L " : "R ") + juce::String (percent) + "%";
                };
                pan.onDragStart = [this] { owner.beginEdit(); };
                pan.onValueChange = [this]
                {
                    if (! pan.isMouseButtonDown())
                        owner.beginEdit();
                    track->pan = (float) pan.getValue();
                };
                addAndMakeVisible (pan);

                mute.setClickingTogglesState (true);
                mute.setColour (juce::TextButton::buttonOnColourId, Theme::redBright);
                mute.setToggleState (t.mute, juce::dontSendNotification);
                mute.onClick = [this] { owner.beginEdit(); track->mute = mute.getToggleState(); };
                addAndMakeVisible (mute);

                solo.setClickingTogglesState (true);
                solo.setColour (juce::TextButton::buttonOnColourId, Theme::yellow);
                solo.setToggleState (t.solo, juce::dontSendNotification);
                solo.onClick = [this] { owner.beginEdit(); track->solo = solo.getToggleState(); };
                addAndMakeVisible (solo);
            }
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (4, 4);
            name.setBounds (r.removeFromTop (22));
            if (! isMaster && outBox.isVisible())
            {
                r.removeFromTop (4);
                outBox.setBounds (r.removeFromTop (24));
                for (auto& row : sendRows)
                {
                    auto line = r.removeFromTop (sendRowHeight);
                    row->name.setBounds (line.removeFromTop (14));
                    row->level.setBounds (line.reduced (0, 1));
                }
                if (moreButton.isVisible())
                    moreButton.setBounds (r.removeFromTop (24).reduced (0, 1));
                r.removeFromTop (4);
            }
            if (! isMaster)
            {
                auto buttons = r.removeFromBottom (26);
                mute.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (2, 0));
                solo.setBounds (buttons.reduced (2, 0));
                r.removeFromBottom (4);
                pan.setBounds (r.removeFromBottom (22));
                r.removeFromBottom (4);
            }
            dbLabel = r.removeFromBottom (16);
            meterArea = r.removeFromRight (14).reduced (0, 6);
            r.removeFromRight (6);
            fader.setBounds (r.reduced (0, 6));
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (Theme::panel);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
            g.setColour (isMaster ? Theme::yellow : Theme::trackColour (track->type == Track::Type::Midi, track->kit != nullptr));
            g.fillRoundedRectangle (4.0f, 4.0f, (float) getWidth() - 8.0f, 3.0f, 1.5f);

            // the meter: green-less warm scale, red at the top; the bar of the loudest recent level
            g.setColour (juce::Colours::black);
            g.fillRoundedRectangle (meterArea.toFloat(), 2.0f);
            const float lvl = juce::jlimit (0.0f, 1.0f, shown);
            const int h = (int) (lvl * (float) meterArea.getHeight());
            g.setColour (lvl > 0.9f ? Theme::redBright : Theme::yellow);
            g.fillRect (meterArea.withTop (meterArea.getBottom() - h));
            if (track->clipped.load())
            {
                g.setColour (Theme::redBright);
                g.fillRect (meterArea.getX(), meterArea.getY() - 5, meterArea.getWidth(), 4);
            }

            g.setColour (Theme::textDim);
            g.setFont (11.0f);
            const double gain = fader.getValue();
            g.drawText (gain < 0.001 ? juce::String ("-inf dB") : juce::String (20.0 * std::log10 (gain), 1) + " dB", dbLabel, juce::Justification::centred);
        }

        // Output and sends: a box for where the track plays and a small slider per bus (up to four; "More sends..." opens the full panel)
        void buildRouting()
        {
            for (auto* b : owner.buses)
                if (b != track)
                    outs.push_back (b);

            outBox.addItem ("Master", 1);   // (short names: the strip is narrow)
            for (size_t i = 0; i < outs.size(); ++i)
                outBox.addItem (outs[i]->name, (int) i + 2);
            outBox.setTooltip ("Output: where this track plays - the master, or a bus (a group, or an effects return)");
            outBox.onChange = [this]
            {
                const int id = outBox.getSelectedId();
                Track* wanted = id >= 2 && (size_t) id - 2 < outs.size() ? outs[(size_t) id - 2] : nullptr;
                if (wanted == track->output)
                    return;
                owner.beginEdit();
                owner.engine.setTrackOutput (*track, wanted);
            };
            addAndMakeVisible (outBox);

            for (size_t i = 0; i < outs.size() && (int) i < maxSendRows; ++i)
            {
                auto row = std::make_unique<SendRow>();
                row->bus = outs[i];
                row->name.setText ("Send " + outs[i]->name, juce::dontSendNotification);
                row->name.setFont (juce::FontOptions (10.5f));
                row->name.setColour (juce::Label::textColourId, Theme::textDim);
                row->name.setMinimumHorizontalScale (0.7f);
                row->name.setInterceptsMouseClicks (false, false);
                addAndMakeVisible (row->name);

                row->level.setSliderStyle (juce::Slider::LinearHorizontal);
                row->level.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
                row->level.setRange (0.0, 1.5, 0.01);
                row->level.setDoubleClickReturnValue (true, 0.0);
                row->level.setPopupDisplayEnabled (true, true, nullptr);
                row->level.textFromValueFunction = [] (double v) { return v < 0.005 ? juce::String ("off") : juce::String (20.0 * std::log10 (v), 1) + " dB"; };
                row->level.setTooltip ("How much of this track also goes to the bus (double-click: off)");
                auto* r = row.get();
                float level = 0.0f;
                bool pre = false;
                owner.engine.getSend (*track, *r->bus, level, pre);
                row->level.setValue (level, juce::dontSendNotification);
                row->level.onDragStart = [this] { owner.beginEdit(); };
                row->level.onValueChange = [this, r]
                {
                    if (! r->level.isMouseButtonDown())
                        owner.beginEdit();
                    float lv = 0.0f;
                    bool pr = false;
                    owner.engine.getSend (*track, *r->bus, lv, pr);   // (the pre / post choice stays as set in the track header)
                    owner.engine.setSend (*track, *r->bus, (float) r->level.getValue(), pr);
                };
                addAndMakeVisible (row->level);
                sendRows.push_back (std::move (row));
            }

            if ((int) outs.size() > maxSendRows)
            {
                moreButton.setButtonText ("More sends...");
                moreButton.onClick = [this] { owner.openFullRouting (*track, moreButton); };
                addAndMakeVisible (moreButton);
            }
        }

        // Message thread, ~30 times a second: the meter falls smoothly, the controls follow the track
        void tick()
        {
            if (outBox.isVisible())   // (undo, or the track header, changed the routing)
            {
                int id = 1;
                for (size_t i = 0; i < outs.size(); ++i)
                    if (track->output == outs[i])
                        id = (int) i + 2;
                if (outBox.getSelectedId() != id)
                    outBox.setSelectedId (id, juce::dontSendNotification);
                for (auto& row : sendRows)
                    if (! row->level.isMouseButtonDown())
                    {
                        float lv = 0.0f;
                        bool pr = false;
                        owner.engine.getSend (*track, *row->bus, lv, pr);
                        if (std::abs (row->level.getValue() - (double) lv) > 0.004)
                            row->level.setValue (lv, juce::dontSendNotification);
                    }
            }
            const float l = track->level.load();
            shown = l > shown ? l : shown * 0.85f;
            if (! fader.isMouseButtonDown() && std::abs (fader.getValue() - (double) track->gain.load()) > 0.0005)
                fader.setValue (track->gain.load(), juce::dontSendNotification);
            if (! isMaster)
            {
                if (! pan.isMouseButtonDown() && std::abs (pan.getValue() - (double) track->pan.load()) > 0.0005)
                    pan.setValue (track->pan.load(), juce::dontSendNotification);
                if (mute.getToggleState() != track->mute.load())
                    mute.setToggleState (track->mute.load(), juce::dontSendNotification);
                if (solo.getToggleState() != track->solo.load())
                    solo.setToggleState (track->solo.load(), juce::dontSendNotification);
                if (! name.isBeingEdited() && name.getText() != track->name)
                    name.setText (track->name, juce::dontSendNotification);
            }
            repaint();
        }

        struct SendRow
        {
            Track* bus = nullptr;
            juce::Label name;
            juce::Slider level;
        };

        Track* track;
        bool isMaster;
        MixerPanel& owner;
        std::vector<Track*> outs;   // the buses this track can play into (not itself)
        juce::ComboBox outBox;
        std::vector<std::unique_ptr<SendRow>> sendRows;
        juce::TextButton moreButton;
        juce::Label name;
        juce::Slider fader;
        PanSlider pan;
        juce::TextButton mute { "M" }, solo { "S" };
        juce::Rectangle<int> dbLabel, meterArea;
        float shown = 0.0f;
    };

    void addStrip (Track& t, bool master)
    {
        auto strip = std::make_unique<Strip> (t, master, *this);
        addAndMakeVisible (*strip);
        strips.push_back (std::move (strip));
    }

    void timerCallback() override
    {
        // the strips follow the tracks: added, removed or replaced tracks rebuild them (compared by address only)
        bool same = shownTracks.size() == (size_t) engine.getNumTracks() + 1 && busSignature == busNames();
        for (int i = 0; same && i < engine.getNumTracks(); ++i)
            same = shownTracks[(size_t) i] == engine.getTrack (i);
        if (! same)
        {
            rebuild();
            return;
        }
        for (auto& s : strips)
            s->tick();
    }

    juce::String busNames() const
    {
        juce::String s;
        for (auto* b : engine.getBuses())
            s << b->name << "|";
        return s;
    }

    Engine& engine;
    std::function<void()> beginEdit;
    std::vector<Track*> buses;
    juce::String busSignature;
    int routingHeight = 0;
    std::vector<std::unique_ptr<Strip>> strips;
    std::vector<Track*> shownTracks;
    std::function<void (Track&, juce::Component&)> fullRouting;
};

class MixerWindow : public juce::DocumentWindow
{
public:
    MixerWindow (Engine& e, std::function<void()> onBeginEdit, std::function<void()> onClosed,
                 std::function<void (Track&, juce::Component&)> onFullRouting = nullptr)
        : juce::DocumentWindow ("Mixer", Theme::background, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          closed (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);
        panel = new MixerPanel (e, std::move (onBeginEdit), std::move (onFullRouting));
        viewport.setViewedComponent (panel, false);
        viewport.setScrollBarsShown (true, true);
        setContentNonOwned (&viewport, true);
        setResizable (true, true);
        setResizeLimits (260, MixerPanel::stripHeight + 60, 4000, 1400);
        centreWithSize (juce::jmin (panel->getWidth() + 24, 1200), juce::jmin (panel->getHeight() + 70, 900));
        setVisible (true);
    }

    ~MixerWindow() override { viewport.setViewedComponent (nullptr, false); delete panel; }

    MixerPanel& getPanel() { return *panel; }
    void closeButtonPressed() override { if (closed) closed(); }

    // Ctrl+Z / Ctrl+Y work here too: the owner decides
    std::function<bool (const juce::KeyPress&)> onKey;
    bool keyPressed (const juce::KeyPress& k) override { return (onKey && onKey (k)) || juce::DocumentWindow::keyPressed (k); }

private:
    juce::Viewport viewport;
    MixerPanel* panel = nullptr;
    std::function<void()> closed;
};
