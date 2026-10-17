#include "PianoRoll.h"
#include "MainComponent.h"

namespace
{
    constexpr int rowH = 14;
    constexpr int minPitch = 24;    // C1
    constexpr int maxPitch = 108;   // C8
    constexpr int totalHeight = (maxPitch - minPitch + 1) * rowH;

    int pitchToY (int pitch) { return (maxPitch - pitch) * rowH; }
    int yToPitch (int y)     { return juce::jlimit (minPitch, maxPitch, maxPitch - y / rowH); }

    bool isBlackKey (int pitch)
    {
        const int m = pitch % 12;
        return m == 1 || m == 3 || m == 6 || m == 8 || m == 10;
    }

    juce::String noteName (int pitch)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[pitch % 12]) + juce::String (pitch / 12 - 1);
    }
}

//==============================================================================
class PianoRoll::Keys : public juce::Component
{
public:
    explicit Keys (PianoRoll& r) : roll (r) {}

    void paint (juce::Graphics& g) override
    {
        const auto area = g.getClipBounds();
        g.setFont (10.0f);
        for (int p = maxPitch; p >= minPitch; --p)
        {
            const juce::Rectangle<int> r (0, pitchToY (p), getWidth(), rowH);
            if (! r.intersects (area))
                continue;

            const bool black = isBlackKey (p);
            g.setColour (p == held ? juce::Colour (0xff3fb57f) : black ? juce::Colour (0xff25272d) : juce::Colour (0xffe6e8ec));
            g.fillRect (r.withTrimmedBottom (1));
            if (p % 12 == 0)
            {
                g.setColour (juce::Colour (0xff444a55));
                g.drawText (noteName (p), r.withTrimmedLeft (getWidth() / 2), juce::Justification::centredRight);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override { press (yToPitch (e.y)); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int p = yToPitch (e.y);
        if (p != held) press (p);
    }
    void mouseUp (const juce::MouseEvent&) override { release(); }

private:
    void press (int pitch)
    {
        release();
        held = pitch;
        if (auto* t = roll.getTrack())
            roll.main.getEngine().previewNote (*t, pitch, true);
        repaint();
    }

    void release()
    {
        if (held >= 0)
            if (auto* t = roll.getTrack())
                roll.main.getEngine().previewNote (*t, held, false);
        held = -1;
        repaint();
    }

    PianoRoll& roll;
    int held = -1;
};

//==============================================================================
class PianoRoll::Grid : public juce::Component, private juce::Timer
{
public:
    explicit Grid (PianoRoll& r) : roll (r), main (r.main) {}
    ~Grid() override { stopPreview(); }

    void setClip (Track* t, int id)
    {
        stopPreview();
        track = t;
        clipId = id;
        selected = -1;
        repaint();
    }

    bool deleteSelected()
    {
        if (! load() || selected < 0 || selected >= (int) clip.notes.size())
            return false;
        main.beginEdit();
        clip.notes.erase (clip.notes.begin() + selected);
        selected = -1;
        commit();
        return true;
    }

    int getSelected() const { return selected; }

    void selectNote (int index)
    {
        selected = index;
        repaint();
    }

    int highestPitch()
    {
        int hi = 78;
        if (load())
            for (auto& n : clip.notes)
                hi = juce::jmax (hi, n.pitch);
        return hi;
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = g.getClipBounds();
        g.fillAll (juce::Colour (0xff1a1c21));
        if (! load())
            return;

        const double pps = main.getPixelsPerSecond();

        for (int p = maxPitch; p >= minPitch; --p)
        {
            const juce::Rectangle<int> r (0, pitchToY (p), getWidth(), rowH);
            if (! r.intersects (area))
                continue;
            g.setColour (isBlackKey (p) ? juce::Colour (0xff16181c) : juce::Colour (0xff1f2228));
            g.fillRect (r);
            g.setColour (p % 12 == 0 ? juce::Colour (0xff3a3f4a) : juce::Colour (0xff272a31));
            g.drawHorizontalLine (r.getBottom() - 1, 0.0f, (float) getWidth());
        }

        // vertical grid: bars, beats and 16ths (when there is room)
        const double step = main.beatSeconds() / 4.0;
        const double firstT = std::floor (xToTime (area.getX()) / step) * step;
        for (double t = firstT;; t += step)
        {
            const int x = timeToX (t);
            if (x > area.getRight()) break;
            const auto index = (long long) std::llround (t / step);
            const bool isBar = index % 16 == 0, isBeat = index % 4 == 0;
            if (! isBar && ! isBeat && step * pps < 9.0) continue;
            if (! isBar && isBeat && main.beatSeconds() * pps < 9.0) continue;
            g.setColour (juce::Colours::white.withAlpha (isBar ? 0.16f : isBeat ? 0.08f : 0.035f));
            g.drawVerticalLine (x, 0.0f, (float) getHeight());
        }

        // dim everything outside the clip
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRect (0, 0, juce::jmax (0, timeToX (clip.start)), getHeight());
        const int endX = timeToX (clip.end());
        g.fillRect (endX, 0, getWidth() - endX, getHeight());

        for (int i = 0; i < (int) clip.notes.size(); ++i)
        {
            const auto r = noteRect (clip.notes[(size_t) i]);
            if (! r.intersects (area))
                continue;
            const bool sel = i == selected;
            g.setColour (sel ? juce::Colour (0xffa8f0c8) : juce::Colour (0xff3fb57f).withMultipliedBrightness (0.6f + 0.5f * clip.notes[(size_t) i].velocity));
            g.fillRect (r);
            g.setColour (sel ? juce::Colours::white : juce::Colour (0xff0e1f17));
            g.drawRect (r, 1);
        }

        const int px = timeToX (main.getEngine().getPositionSeconds());
        g.setColour (juce::Colours::orange);
        g.drawVerticalLine (px, 0.0f, (float) getHeight());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        main.grabKeyboardFocus();
        if (! load())
            return;

        const int hit = hitNote (e.getPosition());

        if (e.mods.isPopupMenu())   // right click deletes
        {
            if (hit >= 0)
            {
                selected = hit;
                deleteSelected();
            }
            return;
        }

        mode = Mode::None;
        gestureEdited = false;
        if (hit >= 0)
        {
            selected = hit;
            const auto& n = clip.notes[(size_t) hit];
            const auto r = noteRect (n);
            mode = (e.x >= r.getRight() - 6 && r.getWidth() > 12) ? Mode::Resize : Mode::Move;
            grabTime = xToTime (e.x) - (clip.start + n.start);
            preview (n.pitch);
        }
        else
        {
            const double t = rawOrSnapped (xToTime (e.x), e) - clip.start;
            if (t < 0.0 || t >= clip.length)
                return;

            Note n;
            n.pitch = yToPitch (e.y);
            n.start = t;
            n.length = juce::jmin (main.beatSeconds(), clip.length - t);
            main.beginEdit();
            gestureEdited = true;   // adding the note and stretching it afterwards is one undo step
            clip.notes.push_back (n);
            selected = (int) clip.notes.size() - 1;
            mode = Mode::Resize;   // dragging right right after clicking stretches the new note
            commit();
            preview (n.pitch);
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (mode == Mode::None || selected < 0 || selected >= (int) clip.notes.size())
            return;

        auto& n = clip.notes[(size_t) selected];
        const double minLen = main.beatSeconds() / 4.0;

        // work out the new values first so the undo snapshot is only taken when something changes
        double newStart = n.start, newLength = n.length;
        int newPitch = n.pitch;
        if (mode == Mode::Resize)
        {
            const double end = rawOrSnapped (xToTime (e.x), e) - clip.start;
            newLength = juce::jmax (minLen, end - n.start);
        }
        else
        {
            const double start = rawOrSnapped (xToTime (e.x) - grabTime, e) - clip.start;
            newStart = juce::jlimit (0.0, juce::jmax (0.0, clip.length - minLen), start);
            newPitch = yToPitch (e.y);
        }

        const bool changed = std::abs (newStart - n.start) > 1.0e-9 || std::abs (newLength - n.length) > 1.0e-9
                             || newPitch != n.pitch;
        if (! changed)
            return;
        if (! gestureEdited)
        {
            main.beginEdit();
            gestureEdited = true;
        }

        if (newPitch != n.pitch)
            preview (newPitch);
        n.start = newStart;
        n.length = newLength;
        n.pitch = newPitch;
        commit();
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        mode = Mode::None;
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown())
            main.zoomBy (w.deltaY > 0 ? 1.15 : 1.0 / 1.15, e.x);
        else
            Component::mouseWheelMove (e, w);
    }

private:
    enum class Mode { None, Move, Resize };

    bool load() { return track != nullptr && main.getEngine().getMidiClip (*track, clipId, clip); }
    void commit() { main.getEngine().setMidiNotes (*track, clipId, clip.notes); }

    double xToTime (int x) const { return (x + main.getScrollX()) / main.getPixelsPerSecond(); }
    int timeToX (double t) const { return (int) std::round (t * main.getPixelsPerSecond()) - main.getScrollX(); }
    double rawOrSnapped (double t, const juce::MouseEvent& e) const { return e.mods.isAltDown() ? t : main.snapTime (t); }

    juce::Rectangle<int> noteRect (const Note& n) const
    {
        const int x = timeToX (clip.start + n.start);
        const int w = juce::jmax (3, (int) std::round (n.length * main.getPixelsPerSecond()));
        return { x, pitchToY (n.pitch) + 1, w, rowH - 1 };
    }

    int hitNote (juce::Point<int> p) const
    {
        for (int i = (int) clip.notes.size() - 1; i >= 0; --i)
            if (noteRect (clip.notes[(size_t) i]).contains (p))
                return i;
        return -1;
    }

    void preview (int pitch)
    {
        stopPreview();
        main.getEngine().previewNote (*track, pitch, true);
        previewPitch = pitch;
        startTimer (400);
    }

    void stopPreview()
    {
        if (previewPitch >= 0 && track != nullptr)
            main.getEngine().previewNote (*track, previewPitch, false);
        previewPitch = -1;
        stopTimer();
    }

    void timerCallback() override { stopPreview(); }

    PianoRoll& roll;
    MainComponent& main;
    Track* track = nullptr;
    int clipId = -1;
    MidiClip clip;
    int selected = -1;
    Mode mode = Mode::None;
    bool gestureEdited = false;   // true once this mouse gesture has taken its undo snapshot
    double grabTime = 0.0;
    int previewPitch = -1;
};

//==============================================================================
// Velocity bars under the grid, one per note. Drag a bar up or down to change that note's velocity;
// dragging sideways across several bars paints them all.
class PianoRoll::Velocity : public juce::Component
{
public:
    explicit Velocity (PianoRoll& r) : roll (r), main (r.main) {}

    void setClip (Track* t, int id)
    {
        track = t;
        clipId = id;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff16181c));
        if (! load())
            return;

        g.setColour (juce::Colours::white.withAlpha (0.06f));
        for (float f : { 0.25f, 0.5f, 0.75f })
            g.drawHorizontalLine ((int) valueToY (f), 0.0f, (float) getWidth());

        const int selected = roll.grid->getSelected();
        for (int i = 0; i < (int) clip.notes.size(); ++i)
        {
            const auto& n = clip.notes[(size_t) i];
            const int x = barX (n);
            const float top = valueToY (n.velocity);
            g.setColour (i == selected ? juce::Colour (0xffa8f0c8) : juce::Colour (0xff3fb57f).withMultipliedBrightness (0.6f + 0.5f * n.velocity));
            g.fillRect ((float) x - 2.0f, top, 5.0f, (float) getHeight() - top);
            g.fillEllipse ((float) x - 4.0f, top - 4.0f, 8.0f, 8.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        main.grabKeyboardFocus();
        gestureEdited = false;
        active = -1;
        if (! load())
            return;

        float best = 8.0f;   // pick the bar closest to the click, within a few pixels
        for (int i = 0; i < (int) clip.notes.size(); ++i)
        {
            const float d = (float) std::abs (barX (clip.notes[(size_t) i]) - e.x);
            if (d < best)
            {
                best = d;
                active = i;
            }
        }
        if (active < 0)
            return;

        lastX = e.x;
        roll.grid->selectNote (active);
        setVelocity ({ active }, yToValue (e.y));
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (active < 0 || ! load())
            return;

        const float v = yToValue (e.y);
        std::vector<int> touched { active };
        const int lo = juce::jmin (lastX, e.x), hi = juce::jmax (lastX, e.x);
        for (int i = 0; i < (int) clip.notes.size(); ++i)
        {
            const int x = barX (clip.notes[(size_t) i]);
            if (i != active && x >= lo && x <= hi)
                touched.push_back (i);
        }
        lastX = e.x;
        setVelocity (touched, v);
    }

    void mouseUp (const juce::MouseEvent&) override { active = -1; }

private:
    bool load() { return track != nullptr && main.getEngine().getMidiClip (*track, clipId, clip); }
    int barX (const Note& n) const
    {
        return (int) std::round ((clip.start + n.start) * main.getPixelsPerSecond()) - main.getScrollX() + 1;
    }
    float valueToY (float v) const { return 4.0f + (1.0f - v) * (float) (getHeight() - 8); }
    float yToValue (int y) const { return juce::jlimit (0.05f, 1.0f, 1.0f - ((float) y - 4.0f) / (float) (getHeight() - 8)); }

    void setVelocity (const std::vector<int>& indices, float v)
    {
        bool changed = false;
        for (int i : indices)
            changed = changed || std::abs (clip.notes[(size_t) i].velocity - v) > 1.0e-4f;
        if (! changed)
            return;

        if (! gestureEdited)
        {
            main.beginEdit();
            gestureEdited = true;
        }
        for (int i : indices)
            clip.notes[(size_t) i].velocity = v;
        main.getEngine().setMidiNotes (*track, clipId, clip.notes);
        roll.repaintGrid();
    }

    PianoRoll& roll;
    MainComponent& main;
    Track* track = nullptr;
    int clipId = -1;
    MidiClip clip;
    int active = -1;
    int lastX = 0;
    bool gestureEdited = false;
};

//==============================================================================
PianoRoll::PianoRoll (MainComponent& m) : main (m)
{
    keys = std::make_unique<Keys> (*this);
    grid = std::make_unique<Grid> (*this);
    velocity = std::make_unique<Velocity> (*this);
    addAndMakeVisible (*velocity);

    title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    addAndMakeVisible (title);

    waveform.addItemList ({ "Sine", "Saw", "Square", "Triangle" }, 1);
    waveform.onChange = [this] { main.beginEdit(); applySynthParams(); };
    addAndMakeVisible (waveform);

    auto setupSlider = [this] (juce::Slider& s, double lo, double hi, double mid)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        s.setRange (lo, hi);
        s.setSkewFactorFromMidPoint (mid);
        s.onDragStart = [this] { main.beginEdit(); };
        s.onValueChange = [this] { applySynthParams(); };
        addAndMakeVisible (s);
    };
    setupSlider (attack, 0.001, 2.0, 0.1);
    setupSlider (release, 0.01, 3.0, 0.3);
    setupSlider (cutoff, 100.0, 18000.0, 2000.0);
    for (auto* l : { &attackLabel, &releaseLabel, &cutoffLabel })
        addAndMakeVisible (l);

    closeButton.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeButton);

    holder.addAndMakeVisible (*keys);
    holder.addAndMakeVisible (*grid);
    viewport.setViewedComponent (&holder, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
}

PianoRoll::~PianoRoll() = default;

void PianoRoll::showClip (Track* t, int id)
{
    const bool changed = t != track || id != clipId;
    track = t;
    clipId = id;
    grid->setClip (t, id);
    velocity->setClip (t, id);

    if (t != nullptr)
    {
        title.setText (t->name + "  -  MIDI clip", juce::dontSendNotification);
        waveform.setSelectedId (t->synth.waveform.load() + 1, juce::dontSendNotification);
        attack.setValue (t->synth.attack, juce::dontSendNotification);
        release.setValue (t->synth.release, juce::dontSendNotification);
        cutoff.setValue (t->synth.cutoff, juce::dontSendNotification);

        if (changed)
            viewport.setViewPosition (0, juce::jmax (0, pitchToY (grid->highestPitch() + 3)));
    }
    repaint();
}

void PianoRoll::applySynthParams()
{
    if (track == nullptr)
        return;
    track->synth.waveform = juce::jmax (0, waveform.getSelectedId() - 1);
    track->synth.attack = (float) attack.getValue();
    track->synth.release = (float) release.getValue();
    track->synth.cutoff = (float) cutoff.getValue();
}

bool PianoRoll::stillValid() const
{
    return track != nullptr && main.getEngine().indexOfTrack (track) >= 0 && main.getEngine().isMidiClip (*track, clipId);
}

bool PianoRoll::deleteSelectedNote() { return grid->deleteSelected(); }

void PianoRoll::repaintGrid()
{
    grid->repaint();
    velocity->repaint();
}

void PianoRoll::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1b1d22));
    g.setColour (juce::Colour (0xff24272e));
    g.fillRect (0, 0, getWidth(), toolbarHeight);
    g.setColour (juce::Colour (0xff0b0c0e));
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());
    g.drawHorizontalLine (toolbarHeight - 1, 0.0f, (float) getWidth());

    // label for the velocity lane at the bottom
    const juce::Rectangle<int> labelArea (0, getHeight() - velocityHeight, MainComponent::controlsWidth, velocityHeight);
    g.setColour (juce::Colour (0xff24272e));
    g.fillRect (labelArea);
    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.setFont (12.0f);
    g.drawText ("Velocity (drag the bars)", labelArea.reduced (12, 0), juce::Justification::centredLeft);
    g.setColour (juce::Colour (0xff0b0c0e));
    g.drawHorizontalLine (getHeight() - velocityHeight, 0.0f, (float) getWidth());
}

void PianoRoll::resized()
{
    auto bar = getLocalBounds().removeFromTop (toolbarHeight).reduced (8, 5);
    closeButton.setBounds (bar.removeFromRight (26));
    title.setBounds (bar.removeFromLeft (MainComponent::controlsWidth - 20));
    waveform.setBounds (bar.removeFromLeft (100));
    bar.removeFromLeft (14);
    attackLabel.setBounds (bar.removeFromLeft (46));
    attack.setBounds (bar.removeFromLeft (110));
    bar.removeFromLeft (10);
    releaseLabel.setBounds (bar.removeFromLeft (52));
    release.setBounds (bar.removeFromLeft (110));
    bar.removeFromLeft (10);
    cutoffLabel.setBounds (bar.removeFromLeft (46));
    cutoff.setBounds (bar.removeFromLeft (130));

    viewport.setBounds (0, toolbarHeight, getWidth(), juce::jmax (0, getHeight() - toolbarHeight - velocityHeight));
    velocity->setBounds (MainComponent::controlsWidth, getHeight() - velocityHeight,
                         juce::jmax (0, getWidth() - MainComponent::controlsWidth), velocityHeight);
    const int w = viewport.getMaximumVisibleWidth();
    holder.setSize (w, totalHeight);
    keys->setBounds (MainComponent::controlsWidth - keyWidth, 0, keyWidth, totalHeight);
    grid->setBounds (MainComponent::controlsWidth, 0, juce::jmax (0, w - MainComponent::controlsWidth), totalHeight);
}
