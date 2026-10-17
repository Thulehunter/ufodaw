#include "MainComponent.h"
#include "PianoRoll.h"
#include "DrumSequencer.h"
#include "FxPanel.h"
#include "PluginPanel.h"
#include <algorithm>
#include <map>

//==============================================================================
// Left-hand controls for one track: name, volume, pan, mute, solo, level meter.
class TrackHeader : public juce::Component
{
public:
    TrackHeader (Track& t, std::function<void (Track*)> onRemove, std::function<void()> onBeginEdit,
                 std::function<void (Track&, juce::Component&)> onOpenPlugins,
                 std::function<void (Track&, juce::Component&)> onOpenAutomation,
                 std::function<double()> onPosition,
                 std::function<void (Track&)> onOpenEditor)
        : track (t), remove (std::move (onRemove)), beginEdit (std::move (onBeginEdit)), openPlugins (std::move (onOpenPlugins)),
          openAutomation (std::move (onOpenAutomation)), position (std::move (onPosition)), openEditor (std::move (onOpenEditor))
    {
        nameLabel.setInterceptsMouseClicks (false, false);   // so double-clicking the name reaches the header
        nameLabel.setText (track.name, juce::dontSendNotification);
        nameLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        addAndMakeVisible (nameLabel);

        volume.setSliderStyle (juce::Slider::LinearHorizontal);
        volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volume.setRange (0.0, 1.5, 0.001);
        volume.setValue (track.gain, juce::dontSendNotification);
        volume.onDragStart = [this] { beginEdit(); };
        volume.onValueChange = [this] { track.gain = (float) volume.getValue(); };
        addAndMakeVisible (volume);

        pan.setSliderStyle (juce::Slider::LinearHorizontal);
        pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        pan.setRange (-1.0, 1.0, 0.01);
        pan.setValue (track.pan, juce::dontSendNotification);
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.onDragStart = [this] { beginEdit(); };
        pan.onValueChange = [this] { track.pan = (float) pan.getValue(); };
        addAndMakeVisible (pan);

        muteButton.setClickingTogglesState (true);
        muteButton.setToggleState (track.mute, juce::dontSendNotification);
        muteButton.setColour (juce::TextButton::buttonOnColourId, Theme::redBright);
        muteButton.onClick = [this] { beginEdit(); track.mute = muteButton.getToggleState(); };
        addAndMakeVisible (muteButton);

        soloButton.setClickingTogglesState (true);
        soloButton.setToggleState (track.solo, juce::dontSendNotification);
        soloButton.setColour (juce::TextButton::buttonOnColourId, Theme::yellow);
        soloButton.onClick = [this] { beginEdit(); track.solo = soloButton.getToggleState(); };
        addAndMakeVisible (soloButton);

        deleteButton.onClick = [this] { remove (&track); };
        addAndMakeVisible (deleteButton);

        fxButton.setTooltip ("Effects: EQ, compressor, delay, reverb");
        fxButton.onClick = [this]
        {
            juce::CallOutBox::launchAsynchronously (std::make_unique<FxPanel> (track, beginEdit), fxButton.getScreenBounds(), nullptr);
        };
        addAndMakeVisible (fxButton);

        vstButton.setTooltip ("VST3 plugins: instrument and effects");
        vstButton.onClick = [this] { openPlugins (track, vstButton); };
        addAndMakeVisible (vstButton);

        autoButton.setTooltip ("Automation of volume and pan");
        autoButton.onClick = [this] { openAutomation (track, autoButton); };
        addAndMakeVisible (autoButton);
    }

    Track& getTrack() { return track; }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.y < 30)   // the name row
            openEditor (track);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);

        // colour stripe by track type: orange = audio, red = MIDI synth, yellow = drums
        g.setColour (Theme::trackColour (track.type == Track::Type::Midi, track.kit != nullptr));
        g.fillRect (0, 0, 5, getHeight() - 1);

        auto m = juce::Rectangle<int> (getWidth() - 8, 6, 4, getHeight() - 12);
        g.setColour (juce::Colours::black);
        g.fillRect (m);
        const float lvl = juce::jlimit (0.0f, 1.0f, shown);
        g.setColour (lvl > 0.9f ? Theme::redBright : Theme::yellow);
        g.fillRect (m.withTop (m.getBottom() - (int) (lvl * (float) m.getHeight())));

        g.setColour (Theme::border);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    void resized() override
    {
        auto c = getLocalBounds().withTrimmedRight (14).reduced (8, 6);
        auto top = c.removeFromTop (20);
        deleteButton.setBounds (top.removeFromRight (22));
        top.removeFromRight (4);
        fxButton.setBounds (top.removeFromRight (32));
        top.removeFromRight (4);
        vstButton.setBounds (top.removeFromRight (38));
        top.removeFromRight (4);
        autoButton.setBounds (top.removeFromRight (26));
        nameLabel.setBounds (top);
        c.removeFromTop (4);
        auto buttons = c.removeFromRight (56);
        muteButton.setBounds (buttons.removeFromTop (22));
        buttons.removeFromTop (4);
        soloButton.setBounds (buttons.removeFromTop (22));
        volume.setBounds (c.removeFromTop (24));
        pan.setBounds (c.removeFromTop (24));
    }

    void tick()
    {
        const float l = track.level;
        shown = l > shown ? l : shown * 0.85f;

        const bool fxActive = track.fx.anyOn();
        if (fxActive != fxShownActive)
        {
            fxShownActive = fxActive;
            fxButton.setColour (juce::TextButton::buttonColourId, fxActive ? Theme::orangeDim : Theme::buttonOff);
            fxButton.repaint();
        }

        // Automated controls follow the envelope at the playhead and can't be dragged by hand.
        const bool volAuto = track.autoVolume.active && ! track.autoVolume.points.empty();
        const bool panAuto = track.autoPan.active && ! track.autoPan.points.empty();
        volume.setEnabled (! volAuto);
        pan.setEnabled (! panAuto);
        if (volAuto && ! volume.isMouseButtonDown())
            volume.setValue (evalAutomation (track.autoVolume.points, position(), track.gain), juce::dontSendNotification);
        if (panAuto && ! pan.isMouseButtonDown())
            pan.setValue (evalAutomation (track.autoPan.points, position(), track.pan), juce::dontSendNotification);

        bool otherAuto = false;
        for (auto& lane : track.autoLanes)
            otherAuto = otherAuto || (lane->active && ! lane->points.empty());
        const int autoState = track.autoEditId.isNotEmpty() ? 2 : (volAuto || panAuto || otherAuto) ? 1 : 0;
        if (autoState != autoShownState)
        {
            autoShownState = autoState;
            autoButton.setColour (juce::TextButton::buttonColourId, autoState == 2 ? Theme::orange
                                                                  : autoState == 1 ? Theme::orangeDim
                                                                                   : Theme::buttonOff);
            autoButton.repaint();
        }

        const bool hasPlugins = track.instrument != nullptr || ! track.inserts.empty();
        if (hasPlugins != vstShownActive)
        {
            vstShownActive = hasPlugins;
            vstButton.setColour (juce::TextButton::buttonColourId, hasPlugins ? Theme::red : Theme::buttonOff);
            vstButton.repaint();
        }
        repaint();
    }

private:
    Track& track;
    bool fxShownActive = false;
    bool vstShownActive = false;
    int autoShownState = -1;
    std::function<void (Track*)> remove;
    std::function<void()> beginEdit;
    std::function<void (Track&, juce::Component&)> openPlugins;
    std::function<void (Track&, juce::Component&)> openAutomation;
    std::function<double()> position;
    std::function<void (Track&)> openEditor;
    float shown = 0.0f;

    juce::Label nameLabel;
    juce::Slider volume, pan;
    juce::TextButton muteButton { "M" }, soloButton { "S" }, deleteButton { "x" }, fxButton { "FX" }, vstButton { "VST" }, autoButton { "A" };
};

//==============================================================================
// Bar/beat ruler above the lanes. Click or drag to move the playhead.
class TimeRuler : public juce::Component
{
public:
    explicit TimeRuler (MainComponent& m) : main (m) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        const double pps = main.getPixelsPerSecond();
        const double bar = main.barSeconds(), beat = main.beatSeconds();
        const double scroll = main.getScrollX();

        auto& engine = main.getEngine();
        const double loopA = engine.loopStart, loopB = engine.loopEnd;
        if (loopB > loopA)
        {
            const int x1 = (int) (loopA * pps - scroll), x2 = (int) (loopB * pps - scroll);
            const bool on = engine.loopEnabled;
            g.setColour ((on ? Theme::orange : Theme::textFaint).withAlpha (on ? 0.35f : 0.22f));
            g.fillRect (x1, 0, x2 - x1, getHeight());
            g.setColour (on ? Theme::orange : Theme::textFaint);
            g.fillRect (x1, getHeight() - 3, x2 - x1, 3);
        }

        const int labelEvery = juce::jmax (1, (int) std::ceil (60.0 / (bar * pps)));
        const int firstBar = (int) std::floor (scroll / (bar * pps));

        g.setFont (11.0f);
        for (int b = firstBar;; ++b)
        {
            const int x = (int) (b * bar * pps - scroll);
            if (x > getWidth()) break;

            g.setColour (Theme::text.withAlpha (0.4f));
            g.drawVerticalLine (x, (float) getHeight() * 0.45f, (float) getHeight());
            if (b % labelEvery == 0)
            {
                g.setColour (Theme::text.withAlpha (0.75f));
                g.drawText (juce::String (b + 1), x + 3, 1, 50, 14, juce::Justification::left);
            }

            if (beat * pps >= 14.0)
                for (int k = 1; k < 4; ++k)
                {
                    g.setColour (Theme::text.withAlpha (0.2f));
                    g.drawVerticalLine (x + (int) (k * beat * pps), (float) getHeight() * 0.7f, (float) getHeight());
                }
        }
        g.setColour (Theme::border);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    // left button: playhead; right button: drag out the loop region
    void mouseDown (const juce::MouseEvent& e) override
    {
        loopDragging = e.mods.isPopupMenu();
        if (loopDragging)
            loopAnchor = main.snapTime (timeAt (e.x));
        else
            move (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (loopDragging)
        {
            const double t = main.snapTime (timeAt (e.x));
            main.setLoopRegion (juce::jmin (loopAnchor, t), juce::jmax (loopAnchor, t));
        }
        else
        {
            move (e);
        }
    }

    void mouseUp (const juce::MouseEvent&) override { loopDragging = false; }

private:
    double timeAt (int x) const { return juce::jmax (0.0, (x + main.getScrollX()) / main.getPixelsPerSecond()); }
    void move (const juce::MouseEvent& e) { main.setPlayhead (timeAt (e.x)); }

    bool loopDragging = false;
    double loopAnchor = 0.0;

    MainComponent& main;
};

//==============================================================================
// One track's timeline row: draws and edits its clips.
class TrackLane : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    TrackLane (MainComponent& m, Track& t) : main (m), track (t) {}

    Track& getTrack() { return track; }

    void paint (juce::Graphics& g) override
    {
        const double pps = main.getPixelsPerSecond();
        const auto area = g.getClipBounds();

        g.fillAll (Theme::laneBg);

        // grid
        const double bar = main.barSeconds(), beat = main.beatSeconds();
        const int firstBar = juce::jmax (0, (int) std::floor (area.getX() / (bar * pps)));
        for (int b = firstBar;; ++b)
        {
            const int x = (int) (b * bar * pps);
            if (x > area.getRight()) break;
            g.setColour (Theme::text.withAlpha (0.10f));
            g.drawVerticalLine (x, 0.0f, (float) getHeight());
            if (beat * pps >= 14.0)
            {
                g.setColour (Theme::text.withAlpha (0.04f));
                for (int k = 1; k < 4; ++k)
                    g.drawVerticalLine (x + (int) (k * beat * pps), 0.0f, (float) getHeight());
            }
        }

        // clips
        const auto clips = main.getEngine().getClips (track);
        for (auto it = cache.begin(); it != cache.end();)
            it = std::any_of (clips.begin(), clips.end(), [&] (auto& c) { return c.id == it->first; }) ? std::next (it) : cache.erase (it);

        for (auto& c : clips)
        {
            const auto r = clipRect (c, pps);
            if (! r.intersects (area))
                continue;

            const bool sel = main.isSelected (&track, c.id);
            g.setColour (sel ? Theme::audioClipSel : Theme::audioClip);
            g.fillRoundedRectangle (r.toFloat(), 4.0f);

            const auto& peaks = peaksFor (c, pps);
            const float mid = (float) r.getCentreY();
            const float half = (float) r.getHeight() * 0.5f - 3.0f;
            g.setColour (Theme::audioWave);
            for (int x = juce::jmax (r.getX(), area.getX()); x < juce::jmin (r.getRight(), area.getRight()); ++x)
            {
                const int idx = x - r.getX();
                if (idx >= 0 && idx < (int) peaks.size())
                    g.drawVerticalLine (x, mid - peaks[(size_t) idx] * half, mid + peaks[(size_t) idx] * half);
            }

            g.setColour (sel ? Theme::text : Theme::text.withAlpha (0.35f));
            g.drawRoundedRectangle (r.toFloat(), 4.0f, sel ? 2.0f : 1.0f);

            g.saveState();
            g.reduceClipRegion (r.reduced (2));
            g.setColour (Theme::text.withAlpha (0.85f));
            g.setFont (11.0f);
            g.drawText (c.sourceFile.getFileNameWithoutExtension(), r.getX() + 5, r.getY() + 2, 400, 13, juce::Justification::left);
            g.restoreState();
        }

        // MIDI clips, with a miniature of their notes
        for (auto& m : main.getEngine().getMidiClips (track))
        {
            const auto r = rectFor (m.start, m.length, pps);
            if (! r.intersects (area))
                continue;

            const bool sel = main.isSelected (&track, m.id);
            const bool drums = track.kit != nullptr;   // drum clips are yellow, synth/MIDI clips red
            g.setColour (sel ? (drums ? Theme::drumClipSel : Theme::midiClipSel) : (drums ? Theme::drumClip : Theme::midiClip));
            g.fillRoundedRectangle (r.toFloat(), 4.0f);

            int lo = 127, hi = 0;
            for (auto& n : m.notes)
            {
                lo = juce::jmin (lo, n.pitch);
                hi = juce::jmax (hi, n.pitch);
            }
            if (m.notes.empty()) { lo = 60; hi = 71; }
            const int span = juce::jmax (12, hi - lo + 1);
            const float rowPx = (float) (r.getHeight() - 18) / (float) span;

            g.saveState();
            g.reduceClipRegion (r.reduced (1));
            g.setColour (Theme::midiNote);
            for (auto& n : m.notes)
            {
                const float nx = (float) r.getX() + (float) (n.start * pps);
                const float nw = juce::jmax (2.0f, (float) (n.length * pps));
                const float ny = (float) r.getY() + 15.0f + (float) (hi - n.pitch) * rowPx;
                g.fillRect (nx, ny, nw, juce::jmax (2.0f, rowPx - 1.0f));
            }
            g.setColour (Theme::text.withAlpha (0.85f));
            g.setFont (11.0f);
            g.drawText ("MIDI", r.getX() + 5, r.getY() + 2, 100, 13, juce::Justification::left);
            g.restoreState();

            g.setColour (sel ? Theme::text : Theme::text.withAlpha (0.35f));
            g.drawRoundedRectangle (r.toFloat(), 4.0f, sel ? 2.0f : 1.0f);
        }

        paintAutomation (g, pps);

        g.setColour (Theme::border);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        main.grabKeyboardFocus();
        if (track.autoEditId.isNotEmpty())
        {
            autoMouseDown (e);
            return;
        }

        const double pps = main.getPixelsPerSecond();
        const double t = e.x / pps;

        dragId = -1;
        resizing = false;
        for (auto& h : hits())   // topmost first
        {
            const auto r = rectFor (h.start, h.length, pps);
            if (! r.contains (e.getPosition()))
                continue;

            dragId = h.id;
            dragTrack = &track;
            dragStart = h.start;
            dragLength = h.length;
            edited = false;
            grabOffset = t - h.start;
            resizing = h.midi && r.getWidth() > 16 && e.x >= r.getRight() - 6;   // right edge of a MIDI clip
            main.selectClip (&track, dragId);
            return;
        }

        main.selectClip (nullptr, -1);
        main.setPlayhead (t);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (track.type != Track::Type::Midi || track.autoEditId.isNotEmpty())
            return;

        const double pps = main.getPixelsPerSecond();
        for (auto& h : hits())
            if (rectFor (h.start, h.length, pps).contains (e.getPosition()))
            {
                if (h.midi && track.instrument != nullptr)   // double-click on a clip: open the instrument's own window
                    main.openInstrumentEditor (track);
                return;
            }

        // empty spot on a MIDI track: new two-bar clip at the bar under the cursor
        const double bar = main.barSeconds();
        const double start = std::floor ((e.x / pps) / bar) * bar;
        main.beginEdit();
        const int id = main.getEngine().addMidiClip (track, start, bar * 2.0);
        if (id >= 0)
        {
            main.selectClip (&track, id);
            main.clipsChanged();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (track.autoEditId.isNotEmpty())
        {
            autoMouseDrag (e);
            return;
        }

        const double t = juce::jmax (0.0, e.x / main.getPixelsPerSecond());
        if (dragId < 0)
        {
            main.setPlayhead (t);   // scrub
            return;
        }

        if (resizing)
        {
            const double end = e.mods.isAltDown() ? t : main.snapTime (t);
            const double newLength = juce::jmax (main.beatSeconds(), end - dragStart);
            if (! edited)
            {
                if (std::abs (newLength - dragLength) < 1.0e-6)
                    return;
                main.beginEdit();
                edited = true;
            }
            main.getEngine().setMidiClipLength (*dragTrack, dragId, newLength);
            main.clipsChanged();
            return;
        }

        // dragging up or down carries the clip to another track of the same kind
        if (auto* target = main.trackAtY (e.getEventRelativeTo (&main.getLaneHolder()).y);
            target != nullptr && target != dragTrack && target->type == dragTrack->type)
        {
            if (! edited)
            {
                main.beginEdit();
                edited = true;
            }
            if (main.getEngine().moveClipToTrack (*dragTrack, *target, dragId))
            {
                dragTrack = target;
                main.selectClip (dragTrack, dragId);
            }
        }

        double newStart = t - grabOffset;
        if (! e.mods.isAltDown())
            newStart = main.snapTime (newStart);
        if (! edited)
        {
            if (std::abs (newStart - dragStart) < 1.0e-6)
                return;
            main.beginEdit();
            edited = true;
        }
        main.getEngine().moveClip (*dragTrack, dragId, newStart);
        main.clipsChanged();
    }

    void mouseUp (const juce::MouseEvent&) override { dragId = -1; autoDrag = -1; }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown())
            main.zoomBy (w.deltaY > 0 ? 1.15 : 1.0 / 1.15, e.getEventRelativeTo (&main.getLaneViewport()).x);
        else
            Component::mouseWheelMove (e, w);
    }

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int x, int) override
    {
        main.importFiles (files, &track, juce::jmax (0.0, main.snapTime (x / main.getPixelsPerSecond())));
    }

private:
    struct Peaks
    {
        double pps = 0.0, offset = -1.0, length = -1.0;
        std::vector<float> values;
    };

    //==========================================================================
    // Automation envelope editing (when the track's "A" menu picked a parameter to edit)

    static constexpr int autoMargin = 10;

    float valueToY (float v, const AutoInfo& info) const
    {
        const float span = juce::jmax (1.0e-9f, info.hi - info.lo);
        return (float) autoMargin + (1.0f - (v - info.lo) / span) * (float) (getHeight() - 2 * autoMargin);
    }

    float yToValue (int y, const AutoInfo& info) const
    {
        const float f = 1.0f - ((float) y - (float) autoMargin) / (float) (getHeight() - 2 * autoMargin);
        return juce::jlimit (info.lo, info.hi, info.lo + f * (info.hi - info.lo));
    }

    // Range and name of the envelope being edited; looked up again only when the choice changes.
    const AutoInfo& editInfo()
    {
        if (track.autoEditId != cachedInfoId)
        {
            cachedInfoId = track.autoEditId;
            cachedInfo = main.getEngine().getAutomationInfo (track, cachedInfoId);
        }
        return cachedInfo;
    }

    void paintAutomation (juce::Graphics& g, double pps)
    {
        const auto editing = track.autoEditId;
        auto& engine = main.getEngine();

        if (editing.isNotEmpty())
        {
            g.setColour (juce::Colours::black.withAlpha (0.45f));   // dim the clips: they can't be edited right now
            g.fillRect (getLocalBounds());
        }

        auto ids = engine.getAutomationIds (track);   // envelopes that have points
        if (editing.isNotEmpty() && std::find (ids.begin(), ids.end(), editing) == ids.end())
            ids.push_back (editing);

        for (auto& id : ids)
        {
            const bool full = id == editing;
            if (! full && ! engine.isAutomationActive (track, id))
                continue;

            const auto info = engine.getAutomationInfo (track, id);
            if (! info.valid)
                continue;

            const auto pts = engine.getAutomation (track, id);
            const auto colour = id == "volume" ? Theme::orange : id == "pan" ? Theme::yellow
                                                                                        : Theme::redBright;
            g.setColour (colour.withAlpha (full ? 1.0f : 0.5f));

            if (pts.empty())
            {
                // nothing yet: show where the parameter currently sits and how to start
                const float y = valueToY (info.current, info);
                const float dashes[] = { 5.0f, 4.0f };
                g.drawDashedLine (juce::Line<float> (0.0f, y, (float) getWidth(), y), dashes, 2, 1.0f);
                g.setFont (12.0f);
                g.drawText (info.name + " automation: click to add points, right-click a point to remove it",
                            8, 4, 700, 14, juce::Justification::left);
                continue;
            }

            juce::Path path;
            path.startNewSubPath (0.0f, valueToY (pts.front().value, info));
            for (auto& p : pts)
                path.lineTo ((float) (p.time * pps), valueToY (p.value, info));
            path.lineTo ((float) getWidth(), valueToY (pts.back().value, info));
            g.strokePath (path, juce::PathStrokeType (full ? 2.0f : 1.0f));

            if (full)
            {
                g.setFont (12.0f);
                g.drawText (info.name, 8, 4, 700, 14, juce::Justification::left);
                for (int i = 0; i < (int) pts.size(); ++i)
                {
                    const float x = (float) (pts[(size_t) i].time * pps), y = valueToY (pts[(size_t) i].value, info);
                    g.setColour (i == autoDrag ? Theme::text : colour);
                    g.fillEllipse (x - 4.5f, y - 4.5f, 9.0f, 9.0f);
                    g.setColour (juce::Colours::black);
                    g.drawEllipse (x - 4.5f, y - 4.5f, 9.0f, 9.0f, 1.0f);
                }
            }
        }
    }

    int autoHit (const std::vector<AutoPoint>& pts, juce::Point<int> p, const AutoInfo& info) const
    {
        const double pps = main.getPixelsPerSecond();
        for (int i = 0; i < (int) pts.size(); ++i)
            if (std::abs (p.x - pts[(size_t) i].time * pps) <= 7.0 && std::abs ((float) p.y - valueToY (pts[(size_t) i].value, info)) <= 7.0f)
                return i;
        return -1;
    }

    double autoTime (const juce::MouseEvent& e) const
    {
        const double t = juce::jmax (0.0, e.x / main.getPixelsPerSecond());
        return e.mods.isAltDown() ? t : main.snapTime (t);
    }

    void autoMouseDown (const juce::MouseEvent& e)
    {
        auto& engine = main.getEngine();
        const auto param = track.autoEditId;
        const auto info = editInfo();
        autoEdited = false;
        autoDrag = -1;
        if (! info.valid)
            return;
        autoPts = engine.getAutomation (track, param);
        const int hit = autoHit (autoPts, e.getPosition(), info);

        if (e.mods.isPopupMenu())   // right click removes a point
        {
            if (hit >= 0)
            {
                main.beginEdit();
                autoPts.erase (autoPts.begin() + hit);
                engine.setAutomation (track, param, autoPts);
                main.clipsChanged();
            }
            return;
        }

        if (hit >= 0)
        {
            autoDrag = hit;
            repaint();
            return;
        }

        // empty spot: new point, which can be dragged straight away
        main.beginEdit();
        autoEdited = true;
        const AutoPoint np { autoTime (e), yToValue (e.y, info) };
        autoPts.push_back (np);
        std::stable_sort (autoPts.begin(), autoPts.end(), [] (const AutoPoint& a, const AutoPoint& b) { return a.time < b.time; });
        for (int i = 0; i < (int) autoPts.size(); ++i)
            if (autoPts[(size_t) i].time == np.time && autoPts[(size_t) i].value == np.value)
                autoDrag = i;
        engine.setAutomation (track, param, autoPts);
        main.clipsChanged();
    }

    void autoMouseDrag (const juce::MouseEvent& e)
    {
        if (autoDrag < 0 || autoDrag >= (int) autoPts.size())
            return;

        const size_t i = (size_t) autoDrag;
        const double minT = i > 0 ? autoPts[i - 1].time + 0.001 : 0.0;
        const double maxT = i + 1 < autoPts.size() ? autoPts[i + 1].time - 0.001 : 1.0e9;
        const double t = juce::jlimit (minT, juce::jmax (minT, maxT), autoTime (e));
        const float v = yToValue (e.y, editInfo());

        if (! autoEdited)
        {
            if (std::abs (t - autoPts[i].time) < 1.0e-9 && std::abs (v - autoPts[i].value) < 1.0e-6f)
                return;
            main.beginEdit();
            autoEdited = true;
        }
        autoPts[i].time = t;
        autoPts[i].value = v;
        main.getEngine().setAutomation (track, track.autoEditId, autoPts);
        main.clipsChanged();
    }

    static juce::Rectangle<int> rectFor (double start, double length, double pps)
    {
        return { (int) std::round (start * pps), 3, juce::jmax (2, (int) std::round (length * pps)), MainComponent::stripHeight - 7 };
    }

    static juce::Rectangle<int> clipRect (const Clip& c, double pps) { return rectFor (c.start, c.length, pps); }

    struct Hit { int id; double start, length; bool midi; };

    // Audio and MIDI clips of this track, topmost last-drawn first.
    std::vector<Hit> hits() const
    {
        std::vector<Hit> out;
        for (auto& c : main.getEngine().getClips (track))
            out.push_back ({ c.id, c.start, c.length, false });
        for (auto& m : main.getEngine().getMidiClips (track))
            out.push_back ({ m.id, m.start, m.length, true });
        std::reverse (out.begin(), out.end());
        return out;
    }

    // Waveform columns for one clip at the current zoom, cached until the zoom or the clip's region changes.
    const std::vector<float>& peaksFor (const Clip& c, double pps)
    {
        auto& p = cache[c.id];
        if (p.pps != pps || p.offset != c.offset || p.length != c.length)
        {
            p.pps = pps;
            p.offset = c.offset;
            p.length = c.length;

            const int w = juce::jmax (2, (int) std::round (c.length * pps));
            p.values.assign ((size_t) w, 0.0f);

            const int n = c.audio->getNumSamples();
            const double spp = c.length * c.fileRate / w;
            const double base = c.offset * c.fileRate;
            for (int ch = 0; ch < c.audio->getNumChannels(); ++ch)
            {
                const auto* d = c.audio->getReadPointer (ch);
                for (int i = 0; i < w; ++i)
                {
                    const int a = juce::jmax (0, (int) (base + i * spp));
                    const int b = juce::jmin (n, (int) (base + (i + 1) * spp) + 1);
                    float m = 0.0f;
                    for (int s = a; s < b; ++s)
                        m = juce::jmax (m, std::abs (d[s]));
                    p.values[(size_t) i] = juce::jmax (p.values[(size_t) i], m);
                }
            }
        }
        return p.values;
    }

    MainComponent& main;
    Track& track;
    std::map<int, Peaks> cache;
    std::vector<AutoPoint> autoPts;   // working copy while an envelope point is being edited
    juce::String cachedInfoId;
    AutoInfo cachedInfo;
    int autoDrag = -1;
    bool autoEdited = false;

    int dragId = -1;
    Track* dragTrack = nullptr;   // the track the dragged clip currently lives on
    bool resizing = false;
    bool edited = false;   // true once this drag has taken its undo snapshot
    double grabOffset = 0.0;
    double dragStart = 0.0;
    double dragLength = 0.0;
};

//==============================================================================
MainComponent::MainComponent()
{
    // These must exist before setSize() triggers resized()
    ruler = std::make_unique<TimeRuler> (*this);
    pianoRoll = std::make_unique<PianoRoll> (*this);
    drumRoll = std::make_unique<DrumSequencer> (*this);
    roll = pianoRoll.get();
    pianoRoll->onClose = [this] { hideRoll(); };
    drumRoll->onClose = [this] { hideRoll(); };
    addChildComponent (*pianoRoll);
    addChildComponent (*drumRoll);

    setWantsKeyboardFocus (true);

    deviceManager.initialiseWithDefaultDevices (2, 2);
    player.setSource (&engine);
    deviceManager.addAudioCallback (&player);

    deviceManager.addMidiInputDeviceCallback ({}, &midiForwarder);   // all enabled MIDI inputs
    midiListConnection = juce::MidiDeviceListConnection::make ([this] { enableAllMidiInputs(); });   // hot-plugging
    statusLabel.setFont (juce::FontOptions (12.0f));
    statusLabel.setColour (juce::Label::textColourId, Theme::text.withAlpha (0.6f));
    addAndMakeVisible (statusLabel);

    keysButton.setTooltip ("Play the armed MIDI track with the computer keyboard: A W S E D F T G Y H U J K O L P. "
                           "Z / X change octave. While on, those letters are not shortcuts.");
    keysButton.setWantsKeyboardFocus (false);
    keysButton.onClick = [this] { grabKeyboardFocus(); };
    addAndMakeVisible (keysButton);

    pluginsButton.setTooltip ("Scan for and manage VST3 plugins");
    pluginsButton.setWantsKeyboardFocus (false);
    pluginsButton.onClick = [this] { pluginHost.showManager(); };
    addAndMakeVisible (pluginsButton);
    enableAllMidiInputs();

    for (juce::Component* b : std::initializer_list<juce::Component*> {
             &rewindButton, &playButton, &pauseButton, &stopButton, &recordButton, &addButton, &addMidiButton,
             &openButton, &saveButton, &saveAsButton, &exportButton, &undoButton, &redoButton, &zoomOutButton, &zoomInButton })
        addAndMakeVisible (b);
    loopButton.setClickingTogglesState (true);
    loopButton.setColour (juce::TextButton::buttonOnColourId, Theme::orange);
    loopButton.setTooltip ("Loop playback (L). Right-drag on the ruler sets the loop; Shift+L loops the selected clip.");
    loopButton.setWantsKeyboardFocus (false);
    loopButton.onClick = [this] { toggleLoop(); grabKeyboardFocus(); };
    addAndMakeVisible (loopButton);
    undoButton.onClick = [this] { undo(); };
    redoButton.onClick = [this] { redo(); };
    updateUndoButtons();
    addAndMakeVisible (metroButton);
    addAndMakeVisible (snapButton);
    addAndMakeVisible (bpmLabel);
    addAndMakeVisible (bpmSlider);
    addAndMakeVisible (masterSlider);
    addAndMakeVisible (timeLabel);

    rewindButton.onClick = [this] { engine.setPositionSeconds (0.0); playStartSeconds = 0.0; };
    playButton.onClick   = [this] { startPlayback(); };
    pauseButton.onClick  = [this] { pausePlayback(); };
    stopButton.onClick   = [this] { stopPlayback(); };
    recordButton.onClick = [this] { toggleRecording(); };
    addButton.onClick = [this] { addTrackDialog(); };
    addMidiButton.onClick = [this] { addMidiTrack (false); };
    addDrumButton.onClick = [this] { addMidiTrack (true); };
    addAndMakeVisible (addDrumButton);
    metroButton.onClick = [this] { engine.metronome = metroButton.getToggleState(); };
    snapButton.setToggleState (true, juce::dontSendNotification);
    openButton.onClick   = [this] { openProjectDialog(); };
    saveButton.onClick   = [this] { if (currentProject != juce::File()) saveProject (currentProject); else saveProjectAs(); };
    saveAsButton.onClick = [this] { saveProjectAs(); };
    exportButton.onClick = [this] { exportDialog(); };
    zoomOutButton.onClick = [this] { zoomBy (1.0 / 1.4, laneViewport.getMaximumVisibleWidth() / 2); };
    zoomInButton.onClick  = [this] { zoomBy (1.4, laneViewport.getMaximumVisibleWidth() / 2); };

    bpmSlider.setRange (40.0, 240.0, 1.0);
    bpmSlider.setValue (120.0, juce::dontSendNotification);
    bpmSlider.onDragStart = [this] { beginEdit(); };
    bpmSlider.onValueChange = [this] { engine.bpm = (float) bpmSlider.getValue(); clipsChanged(); };

    masterSlider.setRange (0.0, 1.2, 0.001);
    masterSlider.setValue (engine.masterGain, juce::dontSendNotification);
    masterSlider.onDragStart = [this] { beginEdit(); };
    masterSlider.onValueChange = [this] { engine.masterGain = (float) masterSlider.getValue(); };

    timeLabel.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 24.0f, juce::Font::plain));
    timeLabel.setJustificationType (juce::Justification::centred);
    timeLabel.setColour (juce::Label::textColourId, Theme::yellow);
    timeLabel.setColour (juce::Label::backgroundColourId, Theme::laneBg);
    timeLabel.setColour (juce::Label::outlineColourId, Theme::border);
    timeLabel.setTooltip ("Playhead position (minutes:seconds)");

    // hover hints for everything in the top bar
    addButton.setTooltip ("Add an audio track from a file (you can also drag audio files into the window)");
    addMidiButton.setTooltip ("Add a synth track: play notes, or draw them in the piano roll");
    addDrumButton.setTooltip ("Add a drum track: click steps in a grid, or load your own samples per pad");
    openButton.setTooltip ("Open a project (Ctrl+O)");
    saveButton.setTooltip ("Save the project (Ctrl+S)");
    saveAsButton.setTooltip ("Save the project under a new name");
    exportButton.setTooltip ("Render the whole project to a WAV file");
    undoButton.setTooltip ("Undo (Ctrl+Z)");
    redoButton.setTooltip ("Redo (Ctrl+Y)");
    metroButton.setTooltip ("Click track while playing");
    snapButton.setTooltip ("Snap moves and notes to a 16th-note grid (hold Alt while dragging to ignore it)");
    zoomInButton.setTooltip ("Zoom in (Ctrl + mouse wheel)");
    zoomOutButton.setTooltip ("Zoom out (Ctrl + mouse wheel)");
    bpmSlider.setTooltip ("Tempo in beats per minute (drag, or double-click to type)");
    masterSlider.setTooltip ("Master volume");

    laneViewport.setViewedComponent (&laneHolder, false);
    laneViewport.setScrollBarsShown (true, true);
    laneViewport.onScroll = [this]
    {
        headerViewport.setViewPosition (0, laneViewport.getViewPositionY());
        ruler->repaint();
        roll->repaintGrid();
        repaint();
    };
    addAndMakeVisible (laneViewport);

    headerViewport.setViewedComponent (&headerHolder, false);
    headerViewport.setScrollBarsShown (false, false);
    addAndMakeVisible (headerViewport);
    addAndMakeVisible (*ruler);

    setSize (1400, 640);   // last: resized() touches all the children above
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    pluginHost.closeAllEditors();   // editor windows must go before the plugins they show
    midiListConnection = {};
    deviceManager.removeMidiInputDeviceCallback ({}, &midiForwarder);
    deviceManager.removeAudioCallback (&player);
    player.setSource (nullptr);
}

//==============================================================================
double MainComponent::beatSeconds() const { return 60.0 / juce::jmax (20.0f, engine.bpm.load()); }

double MainComponent::snapTime (double seconds) const
{
    if (! snapButton.getToggleState())
        return seconds;
    const double step = beatSeconds() / 4.0;   // 16th notes
    return std::round (seconds / step) * step;
}

void MainComponent::selectClip (Track* t, int id)
{
    selTrack = t;
    selClip = id;

    if (t != nullptr && id >= 0 && engine.isMidiClip (*t, id))
        showRoll (t, id);
    else if (rollVisible)
        roll->showClip (roll->getTrack(), roll->getClipId());   // also drops the selected note

    updateArmedTrack();
    repaint();
}

void MainComponent::enableAllMidiInputs()
{
    juce::StringArray names;
    for (auto& d : juce::MidiInput::getAvailableDevices())
    {
        deviceManager.setMidiInputDeviceEnabled (d.identifier, true);
        if (deviceManager.isMidiInputDeviceEnabled (d.identifier))
            names.add (d.name);
    }
    midiInputText = names.joinIntoString (", ");
    updateArmedTrack();
}

// The track that live notes (and MIDI recording) go to.
void MainComponent::updateArmedTrack()
{
    Track* t = nullptr;
    if (selTrack != nullptr)
    {
        if (selTrack->type == Track::Type::Midi)
            t = selTrack;
    }
    else if (rollVisible && roll->getTrack() != nullptr)
    {
        t = roll->getTrack();
    }
    armedTrack = t;

    statusLabel.setText ("MIDI in: " + (midiInputText.isEmpty() ? juce::String ("none") : midiInputText)
                             + "    Armed: " + (t != nullptr ? t->name : juce::String ("none (select a MIDI clip)")),
                         juce::dontSendNotification);
}

void MainComponent::MidiForwarder::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m)
{
    if (m.isNoteOn())       owner.liveNote (m.getNoteNumber(), true, m.getFloatVelocity());
    else if (m.isNoteOff()) owner.liveNote (m.getNoteNumber(), false, 0.0f);
}

// May run on the MIDI thread: plays the note on the armed synth and, while recording, remembers it.
void MainComponent::liveNote (int pitch, bool on, float velocity)
{
    auto* t = armedTrack.load();
    if (t == nullptr)
        return;

    engine.previewNote (*t, pitch, on, velocity);
    if (! midiRecording)
        return;

    const double now = engine.getPositionSeconds();
    const juce::ScopedLock sl (midiRecLock);
    if (on)
    {
        heldNotes.push_back ({ pitch, now, velocity });
        return;
    }
    for (auto it = heldNotes.begin(); it != heldNotes.end(); ++it)
        if (it->pitch == pitch)
        {
            Note n;
            n.pitch = pitch;
            n.start = it->start;
            n.length = juce::jmax (0.02, now - it->start);
            n.velocity = it->velocity;
            recordedNotes.push_back (n);
            heldNotes.erase (it);
            return;
        }
}

void MainComponent::showRoll (Track* t, int clipId)
{
    // drum tracks get the step sequencer, every other MIDI track the piano roll
    RollPanel* wanted = (t != nullptr && t->kit != nullptr) ? static_cast<RollPanel*> (drumRoll.get())
                                                           : static_cast<RollPanel*> (pianoRoll.get());
    const bool wasVisible = rollVisible;
    const bool panelChanged = wanted != roll;
    roll = wanted;
    rollVisible = true;
    roll->showClip (t, clipId);
    if (! wasVisible || panelChanged)
        layoutContent();

    // the panel takes space from the lanes: make sure the clip's own track stays in view
    for (int i = 0; i < lanes.size(); ++i)
        if (&lanes[i]->getTrack() == t)
        {
            auto pos = laneViewport.getViewPosition();
            const int top = i * stripHeight, bottom = top + stripHeight, visible = laneViewport.getMaximumVisibleHeight();
            if (top < pos.y)                   pos.y = top;
            else if (bottom > pos.y + visible) pos.y = juce::jmax (0, bottom - visible);
            laneViewport.setViewPosition (pos);
        }
}

void MainComponent::hideRoll()
{
    if (! rollVisible)
        return;
    rollVisible = false;
    layoutContent();
    updateArmedTrack();
}

void MainComponent::addMidiTrack (bool drums)
{
    int count = 1;
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (auto* other = engine.getTrack (i); other->type == Track::Type::Midi && (other->kit != nullptr) == drums)
            ++count;

    beginEdit();
    auto* t = drums ? engine.addDrumTrack ("Drums " + juce::String (count))
                    : engine.addTrack ("Synth " + juce::String (count), Track::Type::Midi);
    const double bar = barSeconds();
    const double start = std::floor (engine.getPositionSeconds() / bar) * bar;
    const int id = engine.addMidiClip (*t, start, bar * 2.0);
    addTrackUI (*t);
    selectClip (t, id);   // opens the piano roll
    clipsChanged();
}

Track* MainComponent::trackAtY (int y) const
{
    if (y < 0)
        return nullptr;
    const int index = y / stripHeight;
    return index < lanes.size() ? &lanes[index]->getTrack() : nullptr;
}

void MainComponent::setLoopRegion (double a, double b)
{
    if (b - a < beatSeconds() * 0.5)
        return;
    engine.loopStart = a;
    engine.loopEnd = b;
    engine.loopEnabled = true;
    loopButton.setToggleState (true, juce::dontSendNotification);
    ruler->repaint();
    repaint();
}

void MainComponent::openInstrumentEditor (Track& t)
{
    if (t.instrument != nullptr)
        pluginHost.showEditor (*t.instrument);
    else if (! t.inserts.empty())
        pluginHost.showEditor (*t.inserts.front());
    else
        for (auto* header : headers)   // no plugin yet: show the panel where one can be chosen
            if (&header->getTrack() == &t)
                openPlugins (t, *header);
}

void MainComponent::openPlugins (Track& t, juce::Component& anchor)
{
    juce::CallOutBox::launchAsynchronously (std::make_unique<PluginPanel> (t, engine, pluginHost, [this] { beginEdit(); }),
                                            anchor.getScreenBounds(), nullptr);
}

// The "A" menu of a track header: which envelope to edit in the lane, and housekeeping.
void MainComponent::openAutomation (Track& t, juce::Component& anchor)
{
    auto* track = &t;
    auto actions = std::make_shared<std::vector<std::function<void()>>>();   // menu item id - 1 -> what to do
    auto add = [&] (juce::PopupMenu& m, const juce::String& text, bool enabled, bool ticked, std::function<void()> action)
    {
        actions->push_back (std::move (action));
        m.addItem ((int) actions->size(), text, enabled, ticked);
    };
    auto edit = [this, track] (const juce::String& id)
    {
        return [this, track, id] { track->autoEditId = id; };
    };

    juce::PopupMenu menu;

    // what can be edited: volume, pan, then the built-in effects/synth and every hosted plugin's parameters
    add (menu, "Volume", true, t.autoEditId == "volume", edit ("volume"));
    add (menu, "Pan", true, t.autoEditId == "pan", edit ("pan"));

    std::map<juce::String, juce::PopupMenu> groups;
    std::vector<juce::String> groupOrder;
    for (auto& target : engine.listAutomationTargets (t))
    {
        if (groups.find (target.group) == groups.end())
            groupOrder.push_back (target.group);
        add (groups[target.group], target.name, true, t.autoEditId == target.id, edit (target.id));
    }
    menu.addSeparator();
    for (auto& group : groupOrder)
        menu.addSubMenu ("Automate: " + group, groups[group]);

    // what already has envelopes: switch it on/off, clear it
    const auto existing = engine.getAutomationIds (t);
    if (! existing.empty())
    {
        menu.addSeparator();
        for (auto& id : existing)
        {
            const auto info = engine.getAutomationInfo (t, id);
            juce::PopupMenu sub;
            add (sub, "Edit", true, t.autoEditId == id, edit (id));
            add (sub, "Active", true, engine.isAutomationActive (t, id), [this, track, id]
            {
                beginEdit();
                engine.setAutomationActive (*track, id, ! engine.isAutomationActive (*track, id));
            });
            add (sub, "Clear", true, false, [this, track, id]
            {
                beginEdit();
                engine.setAutomation (*track, id, {});
                if (track->autoEditId == id)
                    track->autoEditId = {};
            });
            menu.addSubMenu ("Envelope: " + (info.valid ? info.name : id), sub);
        }
    }

    if (t.autoEditId.isNotEmpty())
    {
        menu.addSeparator();
        add (menu, "Stop editing automation", true, false, [track] { track->autoEditId = {}; });
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor), [this, track, actions] (int result)
    {
        if (result <= 0 || result > (int) actions->size() || engine.indexOfTrack (track) < 0)
            return;
        (*actions)[(size_t) result - 1]();
        clipsChanged();
    });
}

// Plugin instrument + inserts of a track, for the project file.
juce::var MainComponent::pluginsToVar (const Track& t) const
{
    auto* o = new juce::DynamicObject();
    if (t.instrument != nullptr)
        o->setProperty ("instrument", pluginHost.slotToVar (*t.instrument));

    juce::Array<juce::var> list;
    for (auto& p : t.inserts)
        list.add (pluginHost.slotToVar (*p));
    o->setProperty ("inserts", list);

    juce::Array<juce::var> midiFxList;
    for (auto& p : t.midiFx)
        midiFxList.add (pluginHost.slotToVar (*p));
    o->setProperty ("midiFx", midiFxList);
    return juce::var (o);
}

void MainComponent::pluginsFromVar (Track& t, const juce::var& trackVar, juce::StringArray& problems)
{
    const auto plugins = trackVar["plugins"];
    if (! plugins.isObject())
        return;

    juce::String error;
    if (plugins.hasProperty ("instrument") && t.type == Track::Type::Midi)
    {
        if (auto slot = pluginHost.slotFromVar (plugins["instrument"], engine, error))
            engine.setInstrument (t, slot);
        else
            problems.add (t.name + " (instrument): " + error);
    }

    if (auto* inserts = plugins["inserts"].getArray())
        for (auto& v : *inserts)
        {
            if (auto slot = pluginHost.slotFromVar (v, engine, error))
                engine.addInsert (t, slot);
            else
                problems.add (t.name + ": " + error);
        }

    if (auto* midiFx = plugins["midiFx"].getArray())
        for (auto& v : *midiFx)
        {
            if (auto slot = pluginHost.slotFromVar (v, engine, error))
                engine.addMidiFx (t, slot);
            else
                problems.add (t.name + " (MIDI effect): " + error);
        }
}

void MainComponent::toggleLoop()
{
    const bool on = loopButton.getToggleState();
    if (on && engine.loopEnd <= engine.loopStart)   // no region yet: use the project, at least 4 bars
    {
        const double bar = barSeconds();
        engine.loopStart = 0.0;
        engine.loopEnd = juce::jmax (4.0, std::ceil (engine.getTotalLengthSeconds() / bar)) * bar;
    }
    engine.loopEnabled = on;
    ruler->repaint();
    repaint();
}

void MainComponent::loopToSelection()
{
    if (selTrack == nullptr || selClip < 0)
        return;
    Clip a;
    MidiClip m;
    if (engine.getClip (*selTrack, selClip, a))
        setLoopRegion (a.start, a.end());
    else if (engine.getMidiClip (*selTrack, selClip, m))
        setLoopRegion (m.start, m.end());
}

void MainComponent::setPlayhead (double seconds)
{
    engine.setPositionSeconds (seconds);
    repaint();
}

void MainComponent::zoomBy (double factor, int anchorX)
{
    const double old = pixelsPerSecond;
    const double next = juce::jlimit (8.0, 800.0, old * factor);
    if (next == old)
        return;

    const double anchorTime = (laneViewport.getViewPositionX() + anchorX) / old;
    pixelsPerSecond = next;
    layoutContent();
    laneViewport.setViewPosition (juce::jmax (0, (int) (anchorTime * next - anchorX)), laneViewport.getViewPositionY());
    ruler->repaint();
    roll->repaintGrid();
    repaint();
}

void MainComponent::clipsChanged()
{
    layoutContent();
    ruler->repaint();
    roll->repaintGrid();
    repaint();
}

//==============================================================================
// Undo/redo: before each change a snapshot of the project is pushed; undo swaps the live
// project with the latest snapshot (and keeps the state it replaced for redo).

void MainComponent::beginEdit()
{
    undoStack.push_back (engine.captureState());
    if (undoStack.size() > 100)
        undoStack.erase (undoStack.begin());
    redoStack.clear();
    updateUndoButtons();
}

void MainComponent::cancelEdit()
{
    if (! undoStack.empty())
        undoStack.pop_back();
    updateUndoButtons();
}

void MainComponent::updateUndoButtons()
{
    undoButton.setEnabled (! undoStack.empty());
    redoButton.setEnabled (! redoStack.empty());
}

void MainComponent::undo()
{
    if (undoStack.empty())
        return;
    redoStack.push_back (engine.captureState());
    auto state = std::move (undoStack.back());
    undoStack.pop_back();
    restoreSnapshot (state);
}

void MainComponent::redo()
{
    if (redoStack.empty())
        return;
    undoStack.push_back (engine.captureState());
    auto state = std::move (redoStack.back());
    redoStack.pop_back();
    restoreSnapshot (state);
}

void MainComponent::restoreSnapshot (const ProjectState& state)
{
    // Remember what was selected so it can be selected again (and the piano roll reopened).
    const int selTrackIndex = selTrack != nullptr ? engine.indexOfTrack (selTrack) : -1;
    const int selClipId = selClip;

    hideRoll();
    selTrack = nullptr;
    selClip = -1;
    headers.clear();   // they reference the Track objects that restoreState() replaces
    lanes.clear();

    engine.restoreState (state);

    for (int i = 0; i < engine.getNumTracks(); ++i)
        addTrackUI (*engine.getTrack (i));

    bpmSlider.setValue (engine.bpm, juce::dontSendNotification);
    masterSlider.setValue (engine.masterGain, juce::dontSendNotification);

    if (selTrackIndex >= 0)
        if (auto* t = engine.getTrack (selTrackIndex))
        {
            Clip a;
            MidiClip m;
            if (engine.getClip (*t, selClipId, a) || engine.getMidiClip (*t, selClipId, m))
                selectClip (t, selClipId);
        }

    updateUndoButtons();
    updateArmedTrack();   // the old Track pointer is gone
    clipsChanged();
}

//==============================================================================
void MainComponent::addTrackUI (Track& t)
{
    auto* header = headers.add (new TrackHeader (t, [this] (Track* tr)
    {
        // Deferred: this runs inside a button owned by the header being deleted.
        juce::MessageManager::callAsync ([this, tr] { removeTrackUI (tr); });
    }, [this] { beginEdit(); },
       [this] (Track& tr, juce::Component& anchor) { openPlugins (tr, anchor); },
       [this] (Track& tr, juce::Component& anchor) { openAutomation (tr, anchor); },
       [this] { return engine.getPositionSeconds(); },
       [this] (Track& tr) { openInstrumentEditor (tr); }));
    headerHolder.addAndMakeVisible (header);

    auto* lane = lanes.add (new TrackLane (*this, t));
    laneHolder.addAndMakeVisible (lane);
    layoutContent();
}

void MainComponent::removeTrackUI (Track* t)
{
    for (int i = 0; i < lanes.size(); ++i)
        if (&lanes[i]->getTrack() == t)
        {
            beginEdit();
            if (selTrack == t)
                selectClip (nullptr, -1);
            if (roll->getTrack() == t)
                hideRoll();
            if (t->instrument != nullptr)
                pluginHost.closeEditorsFor (t->instrument.get());
            for (auto& p : t->inserts)
                pluginHost.closeEditorsFor (p.get());
            for (auto& p : t->midiFx)
                pluginHost.closeEditorsFor (p.get());
            headers.remove (i);
            lanes.remove (i);
            engine.removeTrack (t);
            layoutContent();
            return;
        }
}

void MainComponent::clearTracksUI()
{
    pluginHost.closeAllEditors();
    hideRoll();
    selectClip (nullptr, -1);
    headers.clear();
    lanes.clear();
    engine.removeAllTracks();
    layoutContent();
}

void MainComponent::layoutContent()
{
    if (getWidth() <= 0)
        return;

    const int y0 = topBarHeight + rulerHeight;
    const int laneW = getWidth() - controlsWidth;
    const int rollH = rollVisible ? juce::jmin (rollHeight, getHeight() / 2) : 0;
    laneViewport.setBounds (controlsWidth, y0, laneW, getHeight() - y0 - bottomBarHeight - rollH);
    for (juce::Component* panel : std::initializer_list<juce::Component*> { pianoRoll.get(), drumRoll.get() })
    {
        panel->setBounds (0, getHeight() - bottomBarHeight - rollH, getWidth(), rollH);
        panel->setVisible (rollVisible && panel == roll);
    }
    ruler->setBounds (controlsWidth, topBarHeight, laneW, rulerHeight);

    const int visibleW = laneViewport.getMaximumVisibleWidth();
    const int visibleH = laneViewport.getMaximumVisibleHeight();
    headerViewport.setBounds (0, y0, controlsWidth, visibleH);

    const int contentH = juce::jmax (lanes.size() * stripHeight, visibleH);
    const double seconds = engine.getTotalLengthSeconds() + 10.0;
    const int contentW = juce::jmax (visibleW, (int) (seconds * pixelsPerSecond));

    for (int i = 0; i < lanes.size(); ++i)
    {
        lanes[i]->setBounds (0, i * stripHeight, contentW, stripHeight);
        headers[i]->setBounds (0, i * stripHeight, controlsWidth, stripHeight);
    }
    laneHolder.setSize (contentW, contentH);
    headerHolder.setSize (controlsWidth, contentH);
}

void MainComponent::resized()
{
    // top bar, in groups: add tracks | time and tempo | project files | undo / redo | master volume
    topSeparators.clear();
    auto top = getLocalBounds().removeFromTop (topBarHeight - 3).reduced (10, 9);
    auto separator = [&] { top.removeFromLeft (10); topSeparators.push_back (top.getX()); top.removeFromLeft (11); };

    addButton.setBounds (top.removeFromLeft (80));
    top.removeFromLeft (6);
    addMidiButton.setBounds (top.removeFromLeft (70));
    top.removeFromLeft (6);
    addDrumButton.setBounds (top.removeFromLeft (76));
    separator();
    timeLabel.setBounds (top.removeFromLeft (150));
    top.removeFromLeft (14);
    bpmLabel.setBounds (top.removeFromLeft (36));
    bpmSlider.setBounds (top.removeFromLeft (70));
    top.removeFromLeft (10);
    metroButton.setBounds (top.removeFromLeft (110));
    separator();
    openButton.setBounds (top.removeFromLeft (56));
    top.removeFromLeft (4);
    saveButton.setBounds (top.removeFromLeft (56));
    top.removeFromLeft (4);
    saveAsButton.setBounds (top.removeFromLeft (64));
    top.removeFromLeft (4);
    exportButton.setBounds (top.removeFromLeft (90));
    separator();
    undoButton.setBounds (top.removeFromLeft (50));
    top.removeFromLeft (4);
    redoButton.setBounds (top.removeFromLeft (50));
    masterSlider.setBounds (top.removeFromRight (150));

    // bottom bar: MIDI/armed status on the left, computer-keys toggle on the right
    statusLabel.setBounds (10, getHeight() - bottomBarHeight + 8, juce::jmax (100, getWidth() / 2 - 150), bottomBarHeight - 16);
    keysButton.setBounds (getWidth() - 100, getHeight() - bottomBarHeight + 18, 90, 24);
    pluginsButton.setBounds (getWidth() - 100 - 96, getHeight() - bottomBarHeight + 16, 90, 28);

    // transport: centred along the bottom
    constexpr int tb = 44, gap = 10;
    const int rowW = 5 * tb + 4 * gap;
    int bx = (getWidth() - rowW) / 2;
    const int by = getHeight() - bottomBarHeight + (bottomBarHeight - tb) / 2;
    for (auto* b : std::initializer_list<juce::Component*> { &rewindButton, &playButton, &pauseButton, &stopButton, &recordButton })
    {
        b->setBounds (bx, by, tb, tb);
        bx += tb + gap;
    }
    loopButton.setBounds (bx + 6, by + 8, 60, tb - 16);

    // snap + zoom live in the corner above the track headers
    auto corner = juce::Rectangle<int> (0, topBarHeight, controlsWidth, rulerHeight).reduced (6, 2);
    snapButton.setBounds (corner.removeFromLeft (70));
    zoomInButton.setBounds (corner.removeFromRight (30));
    corner.removeFromRight (4);
    zoomOutButton.setBounds (corner.removeFromRight (30));

    layoutContent();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);

    // top bar with a red - orange - yellow strip along its lower edge
    g.setColour (Theme::panel);
    g.fillRect (0, 0, getWidth(), topBarHeight);
    juce::ColourGradient strip (Theme::red, 0.0f, 0.0f, Theme::yellow, (float) getWidth(), 0.0f, false);
    strip.addColour (0.5, Theme::orange);
    g.setGradientFill (strip);
    g.fillRect (0, topBarHeight - 3, getWidth(), 3);
    g.setColour (Theme::border);
    for (int x : topSeparators)
        g.fillRect (x, 12, 1, topBarHeight - 3 - 24);

    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("MASTER", masterSlider.getX() - 58, 10, 54, 14, juce::Justification::centredRight);

    // bottom bar
    g.setColour (Theme::panel);
    g.fillRect (0, getHeight() - bottomBarHeight, getWidth(), bottomBarHeight);
    g.setColour (Theme::border);
    g.drawHorizontalLine (getHeight() - bottomBarHeight, 0.0f, (float) getWidth());

    auto m = juce::Rectangle<int> (getWidth() - 158, 8, 150, 4);
    g.setColour (Theme::border);
    g.fillRect (m);
    g.setColour (masterShown > 0.95f ? Theme::redBright : Theme::yellow);
    g.fillRect (m.withWidth ((int) (juce::jlimit (0.0f, 1.0f, masterShown) * (float) m.getWidth())));
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    // loop region, as a faint wash over the lanes
    if (engine.loopEnabled && engine.loopEnd > engine.loopStart)
    {
        const auto lanesArea = laneViewport.getBounds();
        const int x1 = juce::jmax (lanesArea.getX(), lanesArea.getX() + (int) (engine.loopStart * pixelsPerSecond) - getScrollX());
        const int x2 = juce::jmin (lanesArea.getRight(), lanesArea.getX() + (int) (engine.loopEnd * pixelsPerSecond) - getScrollX());
        if (x2 > x1)
        {
            g.setColour (Theme::orange.withAlpha (0.07f));
            g.fillRect (x1, lanesArea.getY(), x2 - x1, lanesArea.getHeight());
        }
    }

    const int x = laneViewport.getX() + (int) (engine.getPositionSeconds() * pixelsPerSecond) - getScrollX();
    if (x >= laneViewport.getX() && x < laneViewport.getRight())
    {
        g.setColour (Theme::playhead);
        g.fillRect (x - 1, ruler->getY(), 2, laneViewport.getBottom() - ruler->getY());
        juce::Path head;   // small flag on the ruler so the playhead is easy to spot and grab
        head.addTriangle ((float) x - 6.0f, (float) ruler->getY() + 1.0f, (float) x + 6.0f, (float) ruler->getY() + 1.0f,
                          (float) x, (float) ruler->getY() + 11.0f);
        g.fillPath (head);
    }

    if (engine.getNumTracks() == 0)   // empty project: say what to do
    {
        auto area = juce::Rectangle<int> (0, topBarHeight + rulerHeight, getWidth(), getHeight() - topBarHeight - rulerHeight - bottomBarHeight);
        auto block = area.withSizeKeepingCentre (560, 230);
        g.setColour (Theme::background.withAlpha (0.85f));
        g.fillRoundedRectangle (block.toFloat().expanded (24.0f, 16.0f), 14.0f);

        g.setColour (Theme::yellow);
        g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
        g.drawText ("Start making music", block.removeFromTop (40), juce::Justification::centred);
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (15.0f));
        g.drawText ("Drag audio files into this window, or add a track with the buttons at the top left:",
                    block.removeFromTop (28), juce::Justification::centred);
        block.removeFromTop (8);

        struct Chip { const char* button; const char* what; juce::Colour colour; };
        const Chip chips[] = { { "+ Track", "audio track - from a wav / mp3 / flac file", Theme::orange },
                               { "+ MIDI",  "synth track - play or draw notes in the piano roll", Theme::redBright },
                               { "+ Drums", "drum machine - click steps in a grid", Theme::yellow } };
        for (auto& c : chips)
        {
            auto row = block.removeFromTop (26);
            g.setColour (c.colour);
            g.fillEllipse ((float) row.getX() + 40.0f, (float) row.getCentreY() - 5.0f, 10.0f, 10.0f);
            g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            g.drawText (c.button, row.withTrimmedLeft (60).withWidth (80), juce::Justification::centredLeft);
            g.setColour (Theme::textDim);
            g.setFont (juce::FontOptions (15.0f));
            g.drawText (c.what, row.withTrimmedLeft (140), juce::Justification::centredLeft);
        }
        block.removeFromTop (10);
        g.setColour (Theme::textFaint);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Space = play / pause    R = record    S = split    Ctrl+Z = undo    hover over a button for a hint",
                    block.removeFromTop (22), juce::Justification::centred);
    }
}

//==============================================================================
void MainComponent::splitAtPlayhead()
{
    const double t = engine.getPositionSeconds();
    beginEdit();
    bool changed = false;

    if (selTrack != nullptr && selClip >= 0)
    {
        changed = engine.splitClip (*selTrack, selClip, t) >= 0;
    }
    else
    {
        for (int i = 0; i < engine.getNumTracks(); ++i)
            if (auto* tr = engine.getTrack (i))
            {
                for (auto& c : engine.getClips (*tr))
                    changed = engine.splitClip (*tr, c.id, t) >= 0 || changed;
                for (auto& m : engine.getMidiClips (*tr))
                    changed = engine.splitClip (*tr, m.id, t) >= 0 || changed;
            }
    }

    if (! changed)
        cancelEdit();   // playhead wasn't inside any clip
    clipsChanged();
}

void MainComponent::deleteSelected()
{
    if (rollVisible && roll->deleteSelectedNote())   // a selected note goes before the clip
        return;

    if (selTrack != nullptr && selClip >= 0)
    {
        beginEdit();
        engine.deleteClip (*selTrack, selClip);
        selectClip (nullptr, -1);
        clipsChanged();
    }
}

void MainComponent::duplicateSelected()
{
    if (selTrack != nullptr && selClip >= 0)
    {
        beginEdit();
        const int id = engine.duplicateClip (*selTrack, selClip);
        if (id >= 0)
            selectClip (selTrack, id);
        clipsChanged();
    }
}

namespace
{
    // Computer keyboard as a piano: index in this string == semitones above the base C
    const char pianoKeyChars[] = "AWSEDFTGYHUJKOLP";
}

bool MainComponent::keyStateChanged (bool)
{
    if (! keysButton.getToggleState())
        return false;

    const auto mods = juce::ModifierKeys::currentModifiers;
    const bool blocked = mods.isCommandDown() || mods.isAltDown();
    bool handled = false;

    for (int i = 0; i < 16; ++i)
    {
        const bool down = ! blocked && juce::KeyPress::isKeyCurrentlyDown (pianoKeyChars[i]);
        if (down && ! pianoKeyDown[i])
        {
            pianoKeyDown[i] = true;
            pianoKeyPitch[i] = juce::jlimit (0, 127, 60 + 12 * pianoOctave + i);
            liveNote (pianoKeyPitch[i], true, 0.8f);
            handled = true;
        }
        else if (! down && pianoKeyDown[i])
        {
            pianoKeyDown[i] = false;
            liveNote (pianoKeyPitch[i], false, 0.0f);
            handled = true;
        }
    }
    return handled;
}

void MainComponent::focusLost (FocusChangeType)
{
    for (int i = 0; i < 16; ++i)   // don't leave notes hanging when keys are released elsewhere
        if (pianoKeyDown[i])
        {
            pianoKeyDown[i] = false;
            liveNote (pianoKeyPitch[i], false, 0.0f);
        }
}

bool MainComponent::keyPressed (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    const auto ch = juce::CharacterFunctions::toLowerCase (k.getTextCharacter());

    if (keysButton.getToggleState() && ! mods.isCommandDown() && ! mods.isAltDown())
    {
        const int code = k.getKeyCode();
        if (code == 'Z') { pianoOctave = juce::jmax (-3, pianoOctave - 1); return true; }
        if (code == 'X') { pianoOctave = juce::jmin (3, pianoOctave + 1); return true; }
        for (int i = 0; i < 16; ++i)
            if (code == pianoKeyChars[i])
                return true;   // played in keyStateChanged(); not a shortcut
    }

    if (mods.isCommandDown())
    {
        if (ch == 's' || k.getKeyCode() == 'S') { saveButton.triggerClick(); return true; }
        if (ch == 'o' || k.getKeyCode() == 'O') { openButton.triggerClick(); return true; }
        if (k.getKeyCode() == 'D')              { duplicateSelected(); return true; }
        if (k.getKeyCode() == 'Z')              { if (mods.isShiftDown()) redo(); else undo(); return true; }
        if (k.getKeyCode() == 'Y')              { redo(); return true; }
        return false;
    }

    if (k.getKeyCode() == 'L' && ! mods.isAltDown())
    {
        if (mods.isShiftDown()) loopToSelection(); else loopButton.triggerClick();
        return true;
    }
    if (k == juce::KeyPress::spaceKey)  { if (engine.isPlaying()) pausePlayback(); else startPlayback(); return true; }
    if (k == juce::KeyPress::returnKey) { rewindButton.triggerClick(); return true; }
    if (k.getKeyCode() == 'R' && ! mods.isAnyModifierKeyDown()) { toggleRecording(); return true; }
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) { deleteSelected(); return true; }
    if (k.getKeyCode() == 'S' && ! mods.isAnyModifierKeyDown()) { splitAtPlayhead(); return true; }
    return false;
}

void MainComponent::timerCallback()
{
    const double p = engine.getPositionSeconds();
    const int mins = (int) (p / 60.0);
    timeLabel.setText (juce::String::formatted ("%02d:%06.3f", mins, p - mins * 60.0), juce::dontSendNotification);

    const float ml = engine.masterLevel;
    masterShown = ml > masterShown ? ml : masterShown * 0.85f;

    for (auto* h : headers)
        h->tick();

    if (rollVisible)
    {
        if (roll->stillValid())
            roll->repaintGrid();   // follows the playhead
        else
            hideRoll();
    }

    const bool recording = engine.isRecording() || midiRecording;
    playButton.setLit (engine.isPlaying());
    pauseButton.setLit (! engine.isPlaying() && p > 0.0);
    recordButton.setLit (recording && (juce::Time::getMillisecondCounter() / 500) % 2 == 0);   // blinks

    repaint();
}

//==============================================================================
// Transport. Play/Pause keep the position; Stop returns to where playback started
// (or to the beginning if the transport is already stopped).
void MainComponent::startPlayback()
{
    if (! engine.isPlaying())
    {
        playStartSeconds = engine.getPositionSeconds();
        engine.play();
    }
}

void MainComponent::pausePlayback()
{
    finishRecording();
    engine.stop();
}

void MainComponent::stopPlayback()
{
    finishRecording();
    if (engine.isPlaying())
    {
        engine.stop();
        engine.setPositionSeconds (playStartSeconds);
    }
    else
    {
        engine.setPositionSeconds (0.0);
        playStartSeconds = 0.0;
    }
}

void MainComponent::toggleRecording()
{
    if (engine.isRecording() || midiRecording)
    {
        finishRecording();   // keeps playing, like most DAWs
        return;
    }

    // A MIDI track is armed: record the notes that are played, not the audio input
    if (auto* armed = armedTrack.load())
    {
        {
            const juce::ScopedLock sl (midiRecLock);
            heldNotes.clear();
            recordedNotes.clear();
        }
        recMidiTrack = armed;
        recStartSeconds = engine.getPositionSeconds();
        midiRecording = true;
        startPlayback();
        return;
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    const int inputs = device != nullptr ? device->getActiveInputChannels().countNumberOfSetBits() : 0;
    if (inputs == 0)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "No audio input",
                                                "No input device is available. Check your microphone or interface and "
                                                "Windows' microphone privacy settings.");
        return;
    }

    recordingFile = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                        .getChildFile ("MiniDAW Recordings")
                        .getChildFile ("Recording " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S") + ".wav");

    if (! engine.startRecording (recordingFile, juce::jmin (inputs, 2)))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Cannot record",
                                                "Could not create " + recordingFile.getFullPathName());
        return;
    }

    recStartSeconds = engine.getPositionSeconds();
    startPlayback();   // recording only captures while the transport runs
}

// Turns the notes played during MIDI recording into a clip on the armed track, starting on the bar
// where recording began and lasting a whole number of bars.
void MainComponent::finishMidiRecording()
{
    midiRecording = false;
    const double now = engine.getPositionSeconds();

    std::vector<Note> notes;
    {
        const juce::ScopedLock sl (midiRecLock);
        for (auto& h : heldNotes)   // notes still held when recording stops end here
        {
            Note n;
            n.pitch = h.pitch;
            n.start = h.start;
            n.length = juce::jmax (0.02, now - h.start);
            n.velocity = h.velocity;
            recordedNotes.push_back (n);
        }
        heldNotes.clear();
        notes.swap (recordedNotes);
    }

    auto* t = recMidiTrack;
    recMidiTrack = nullptr;
    if (notes.empty() || t == nullptr || engine.indexOfTrack (t) < 0)
        return;

    const double bar = barSeconds();
    const double clipStart = std::floor (recStartSeconds / bar) * bar;
    double end = 0.0;
    for (auto& n : notes)
    {
        n.start = juce::jmax (0.0, n.start - clipStart);
        end = juce::jmax (end, n.start + n.length);
    }
    const double clipLength = juce::jmax (1.0, std::ceil (end / bar)) * bar;

    beginEdit();
    const int id = engine.addMidiClip (*t, clipStart, clipLength);
    engine.setMidiNotes (*t, id, std::move (notes));
    selectClip (t, id);
    clipsChanged();
}

// Closes the recording file and puts it on a new track at the position where recording began.
void MainComponent::finishRecording()
{
    if (midiRecording)
    {
        finishMidiRecording();
        return;
    }

    if (! engine.isRecording())
        return;

    engine.stopRecording();

    beginEdit();
    auto* t = engine.addTrack (recordingFile.getFileNameWithoutExtension());
    if (engine.addClipFromFile (*t, recordingFile, recStartSeconds) < 0)
    {
        engine.removeTrack (t);
        recordingFile.deleteFile();
        cancelEdit();
        return;
    }
    addTrackUI (*t);
    clipsChanged();
}

//==============================================================================
void MainComponent::addTrackDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose audio files", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems
                              | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              juce::StringArray paths;
                              for (auto& f : fc.getResults())
                                  paths.add (f.getFullPathName());
                              importFiles (paths, nullptr, 0.0);
                          });
}

// With one file and a target track the audio becomes a clip on that track at `time`;
// otherwise every file gets its own new track starting at 0.
void MainComponent::importFiles (const juce::StringArray& paths, Track* target, double time)
{
    juce::StringArray failed;
    const bool intoTrack = target != nullptr && target->type == Track::Type::Audio && paths.size() == 1;
    beginEdit();

    for (auto& p : paths)
    {
        const juce::File f (p);
        if (intoTrack)
        {
            const int id = engine.addClipFromFile (*target, f, time);
            if (id >= 0) selectClip (target, id); else failed.add (p);
        }
        else
        {
            auto* t = engine.addTrack (f.getFileNameWithoutExtension());
            if (engine.addClipFromFile (*t, f, 0.0) < 0)
            {
                engine.removeTrack (t);
                failed.add (p);
            }
            else
            {
                addTrackUI (*t);
            }
        }
    }

    if (failed.size() == paths.size())
        cancelEdit();   // nothing was imported
    clipsChanged();
    if (failed.size() > 0)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Cannot open file(s)", failed.joinIntoString ("\n"));
}

//==============================================================================
void MainComponent::saveProject (const juce::File& file)
{
    const auto baseDir = file.getParentDirectory();

    juce::Array<juce::var> trackList;
    for (int i = 0; i < engine.getNumTracks(); ++i)
    {
        auto* t = engine.getTrack (i);

        juce::Array<juce::var> clipList;
        for (auto& c : engine.getClips (*t))
        {
            auto* co = new juce::DynamicObject();
            co->setProperty ("file", c.sourceFile.getRelativePathFrom (baseDir));
            co->setProperty ("start", c.start);
            co->setProperty ("offset", c.offset);
            co->setProperty ("length", c.length);
            clipList.add (juce::var (co));
        }

        auto* o = new juce::DynamicObject();
        o->setProperty ("name", t->name);
        o->setProperty ("type", t->type == Track::Type::Midi ? "midi" : "audio");
        o->setProperty ("gain", (double) t->gain.load());
        o->setProperty ("pan", (double) t->pan.load());
        o->setProperty ("mute", t->mute.load());
        o->setProperty ("solo", t->solo.load());
        o->setProperty ("clips", clipList);

        if (t->type == Track::Type::Midi)
        {
            auto* s = new juce::DynamicObject();
            s->setProperty ("waveform", t->synth.waveform.load());
            s->setProperty ("attack", (double) t->synth.attack.load());
            s->setProperty ("decay", (double) t->synth.decay.load());
            s->setProperty ("sustain", (double) t->synth.sustain.load());
            s->setProperty ("release", (double) t->synth.release.load());
            s->setProperty ("cutoff", (double) t->synth.cutoff.load());
            o->setProperty ("synth", juce::var (s));

            juce::Array<juce::var> midiList;
            for (auto& m : engine.getMidiClips (*t))
            {
                juce::Array<juce::var> noteList;
                for (auto& n : m.notes)
                {
                    auto* no = new juce::DynamicObject();
                    no->setProperty ("pitch", n.pitch);
                    no->setProperty ("start", n.start);
                    no->setProperty ("length", n.length);
                    no->setProperty ("velocity", (double) n.velocity);
                    noteList.add (juce::var (no));
                }
                auto* mo = new juce::DynamicObject();
                mo->setProperty ("start", m.start);
                mo->setProperty ("length", m.length);
                mo->setProperty ("notes", noteList);
                midiList.add (juce::var (mo));
            }
            o->setProperty ("midiClips", midiList);

            if (t->kit != nullptr)   // only the pads with a user sample need saving; the others are generated
            {
                juce::Array<juce::var> padFiles;
                for (int pad = 0; pad < DrumKit::numPads; ++pad)
                {
                    const auto f = t->kit->getPad (pad).file;
                    padFiles.add (f == juce::File() ? juce::String() : f.getRelativePathFrom (baseDir));
                }
                o->setProperty ("drumPads", padFiles);
            }
        }
        auto* fo = new juce::DynamicObject();
        t->fx.forEachFloat ([fo] (const char* name, std::atomic<float>& v) { fo->setProperty (name, (double) v.load()); });
        t->fx.forEachBool ([fo] (const char* name, std::atomic<bool>& v) { fo->setProperty (name, v.load()); });
        o->setProperty ("fx", juce::var (fo));
        o->setProperty ("plugins", pluginsToVar (*t));
        o->setProperty ("sequencer", t->sequencer);
        juce::Array<juce::var> outs;
        for (auto& route : t->midiOuts)
        {
            auto* ro = new juce::DynamicObject();
            ro->setProperty ("track", engine.indexOfTrack (route.target));
            ro->setProperty ("channel", route.channel);
            outs.add (juce::var (ro));
        }
        o->setProperty ("midiOuts", outs);

        // automation, keyed by envelope id ("volume", "pan", "fx.reverbMix", "plugin:3:12", ...)
        auto* ao = new juce::DynamicObject();
        for (auto& id : engine.getAutomationIds (*t))
        {
            juce::Array<juce::var> points;
            for (auto& p : engine.getAutomation (*t, id))
            {
                juce::Array<juce::var> pair;
                pair.add (p.time);
                pair.add ((double) p.value);
                points.add (juce::var (pair));
            }
            auto* lo = new juce::DynamicObject();
            lo->setProperty ("active", engine.isAutomationActive (*t, id));
            lo->setProperty ("points", points);
            ao->setProperty (juce::Identifier (id), juce::var (lo));
        }
        o->setProperty ("automation", juce::var (ao));

        trackList.add (juce::var (o));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 3);
    root->setProperty ("bpm", (double) engine.bpm.load());
    root->setProperty ("masterGain", (double) engine.masterGain.load());
    root->setProperty ("metronome", engine.metronome.load());
    root->setProperty ("loopEnabled", engine.loopEnabled.load());
    root->setProperty ("loopStart", engine.loopStart.load());
    root->setProperty ("loopEnd", engine.loopEnd.load());
    root->setProperty ("tracks", trackList);

    if (file.replaceWithText (juce::JSON::toString (juce::var (root))))
    {
        currentProject = file;
        if (auto* w = getTopLevelComponent())
            w->setName ("MiniDAW - " + file.getFileNameWithoutExtension());
    }
    else
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Cannot save project", file.getFullPathName());
    }
}

void MainComponent::saveProjectAs()
{
    chooser = std::make_unique<juce::FileChooser> ("Save project", currentProject, "*.mdaw");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f != juce::File())
                                  saveProject (f.withFileExtension ("mdaw"));
                          });
}

void MainComponent::openProjectDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Open project", currentProject, "*.mdaw");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f.existsAsFile())
                                  loadProject (f);
                          });
}

void MainComponent::loadProject (const juce::File& file)
{
    auto root = juce::JSON::parse (file);
    if (! root.isObject() || ! root["tracks"].isArray())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Cannot open project", file.getFullPathName());
        return;
    }

    finishRecording();
    engine.stop();
    engine.setPositionSeconds (0.0);
    playStartSeconds = 0.0;
    clearTracksUI();
    undoStack.clear();   // a freshly opened project starts with a clean history
    redoStack.clear();
    updateUndoButtons();

    engine.bpm = (float) (double) root.getProperty ("bpm", 120.0);
    engine.masterGain = (float) (double) root.getProperty ("masterGain", 0.9);
    engine.metronome = (bool) root.getProperty ("metronome", false);
    bpmSlider.setValue (engine.bpm, juce::dontSendNotification);
    masterSlider.setValue (engine.masterGain, juce::dontSendNotification);
    metroButton.setToggleState (engine.metronome, juce::dontSendNotification);

    engine.loopStart = (double) root.getProperty ("loopStart", 0.0);
    engine.loopEnd = (double) root.getProperty ("loopEnd", 0.0);
    engine.loopEnabled = (bool) root.getProperty ("loopEnabled", false);
    loopButton.setToggleState (engine.loopEnabled, juce::dontSendNotification);

    juce::StringArray missing, pluginProblems;
    const auto baseDir = file.getParentDirectory();

    for (auto& item : *root["tracks"].getArray())
    {
        const bool isMidi = item.getProperty ("type", "audio").toString() == "midi";
        const bool isDrums = isMidi && item.hasProperty ("drumPads");
        const auto trackName = item.getProperty ("name", "Track").toString();
        auto* t = isDrums ? engine.addDrumTrack (trackName)
                          : engine.addTrack (trackName, isMidi ? Track::Type::Midi : Track::Type::Audio);

        if (isDrums)
            if (auto* padFiles = item["drumPads"].getArray())
                for (int pad = 0; pad < padFiles->size() && pad < DrumKit::numPads; ++pad)
                {
                    const auto rel = (*padFiles)[pad].toString();
                    if (rel.isEmpty())
                        continue;
                    const auto sample = baseDir.getChildFile (rel);
                    if (! engine.loadDrumSample (*t, pad, sample))
                        missing.add (sample.getFullPathName());
                }
        t->gain = (float) (double) item.getProperty ("gain", 0.8);
        t->pan = (float) (double) item.getProperty ("pan", 0.0);
        t->mute = (bool) item.getProperty ("mute", false);
        t->solo = (bool) item.getProperty ("solo", false);

        const auto fxv = item["fx"];
        if (fxv.isObject())
        {
            t->fx.forEachFloat ([&fxv] (const char* name, std::atomic<float>& v)
                                { if (fxv.hasProperty (name)) v = (float) (double) fxv.getProperty (name, 0.0); });
            t->fx.forEachBool ([&fxv] (const char* name, std::atomic<bool>& v)
                               { if (fxv.hasProperty (name)) v = (bool) fxv.getProperty (name, false); });
        }

        auto loadClip = [&] (const juce::var& v)
        {
            const auto audioFile = baseDir.getChildFile (v["file"].toString());
            const double start = v.getProperty ("start", 0.0);
            const int id = engine.addClipFromFile (*t, audioFile, start);
            if (id < 0)
            {
                missing.add (audioFile.getFullPathName());
                return;
            }
            Clip c;
            engine.getClip (*t, id, c);
            engine.setClipRegion (*t, id, start,
                                  v.getProperty ("offset", 0.0),
                                  v.getProperty ("length", c.sourceLength()));
        };

        if (isMidi)
        {
            const auto s = item["synth"];
            t->synth.waveform = (int) s.getProperty ("waveform", (int) SynthParams::Saw);
            t->synth.attack = (float) (double) s.getProperty ("attack", 0.005);
            t->synth.decay = (float) (double) s.getProperty ("decay", 0.15);
            t->synth.sustain = (float) (double) s.getProperty ("sustain", 0.7);
            t->synth.release = (float) (double) s.getProperty ("release", 0.25);
            t->synth.cutoff = (float) (double) s.getProperty ("cutoff", 8000.0);

            if (auto* midiArray = item["midiClips"].getArray())
                for (auto& mv : *midiArray)
                {
                    const int id = engine.addMidiClip (*t, mv.getProperty ("start", 0.0), mv.getProperty ("length", 4.0));
                    std::vector<Note> notes;
                    if (auto* noteArray = mv["notes"].getArray())
                        for (auto& nv : *noteArray)
                        {
                            Note n;
                            n.pitch = (int) nv.getProperty ("pitch", 60);
                            n.start = nv.getProperty ("start", 0.0);
                            n.length = nv.getProperty ("length", 0.5);
                            n.velocity = (float) (double) nv.getProperty ("velocity", 0.8);
                            notes.push_back (n);
                        }
                    engine.setMidiNotes (*t, id, std::move (notes));
                }
        }
        else if (auto* clipArray = item["clips"].getArray())
        {
            for (auto& cv : *clipArray)
                loadClip (cv);
        }
        else if (item.hasProperty ("file"))   // version 1: one clip per track
        {
            loadClip (item);
        }

        pluginsFromVar (*t, item, pluginProblems);

        // (plugins are loaded above, so lanes bound to plugin parameters can find them)
        if (auto* automation = item["automation"].getDynamicObject())
            for (auto& entry : automation->getProperties())
            {
                const juce::String id = entry.name.toString();
                std::vector<AutoPoint> points;
                if (auto* arr = entry.value["points"].getArray())
                    for (auto& pv : *arr)
                        if (auto* pair = pv.getArray(); pair != nullptr && pair->size() >= 2)
                            points.push_back ({ (double) (*pair)[0], (float) (double) (*pair)[1] });

                if (engine.setAutomation (*t, id, std::move (points)))
                    engine.setAutomationActive (*t, id, (bool) entry.value.getProperty ("active", true));
            }

        addTrackUI (*t);
    }

    // MIDI routes refer to other tracks by index, so they are set once every track exists
    {
        int index = 0;
        for (auto& item : *root["tracks"].getArray())
        {
            std::vector<MidiRoute> routes;
            if (auto* outs = item["midiOuts"].getArray())
                for (auto& v : *outs)
                {
                    // newer projects: { track, channel }; earlier ones: just the track index
                    const bool isObject = v.isObject();
                    const int trackIndex = isObject ? (int) v["track"] : (int) v;
                    const int channel = isObject ? (int) v["channel"] : 0;
                    if (auto* to = engine.getTrack (trackIndex))
                        routes.push_back ({ to, channel });
                }
            if (item.hasProperty ("midiOut") && (int) item["midiOut"] >= 0)   // projects saved with a single route
                if (auto* to = engine.getTrack ((int) item["midiOut"]))
                    routes.push_back ({ to, 0 });

            if (auto* from = engine.getTrack (index))
            {
                from->sequencer = (bool) item.getProperty ("sequencer", ! routes.empty());
                if (! routes.empty())
                    engine.setMidiOuts (*from, routes);
            }
            ++index;
        }
    }

    currentProject = file;
    if (auto* w = getTopLevelComponent())
        w->setName ("MiniDAW - " + file.getFileNameWithoutExtension());
    clipsChanged();

    if (missing.size() > 0)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Some audio files were not found",
                                                missing.joinIntoString ("\n"));
    if (pluginProblems.size() > 0)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Some plugins could not be loaded",
                                                pluginProblems.joinIntoString ("\n"));
}

void MainComponent::exportDialog()
{
    if (engine.getTotalLengthSeconds() <= 0.0)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                                "Nothing to export", "Add a track first.");
        return;
    }

    chooser = std::make_unique<juce::FileChooser> ("Export WAV", currentProject.withFileExtension ("wav"), "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f == juce::File())
                                  return;

                              finishRecording();
                              engine.stop();
                              f = f.withFileExtension ("wav");
                              const bool ok = engine.renderToFile (f, engine.getTotalLengthSeconds() + engine.getRenderTailSeconds(), false);
                              juce::AlertWindow::showMessageBoxAsync (ok ? juce::MessageBoxIconType::InfoIcon
                                                                         : juce::MessageBoxIconType::WarningIcon,
                                                                      ok ? "Export finished" : "Export failed",
                                                                      f.getFullPathName());
                          });
}
