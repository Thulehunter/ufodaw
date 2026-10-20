#include "PianoRoll.h"
#include "MainComponent.h"
#include "Scales.h"
#include "SynthPresets.h"

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

    // Every key carries its name (C4, D#4, ...) and lights up while its note sounds: played live, or playing in the clip.
    void paint (juce::Graphics& g) override
    {
        const auto area = g.getClipBounds();
        const auto sounding = roll.activePitches();
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        for (int p = maxPitch; p >= minPitch; --p)
        {
            const juce::Rectangle<int> r (0, pitchToY (p), getWidth(), rowH);
            if (! r.intersects (area))
                continue;

            const bool black = isBlackKey (p);
            const bool lit = p == held || sounding.test ((size_t) p);
            g.setColour (lit ? Theme::yellow : black ? Theme::keyBlack : Theme::keyWhite);
            g.fillRect (r.withTrimmedBottom (1));
            if (auto* tr = roll.getTrack(); tr != nullptr && tr->scaleType > 0 && Scales::contains (p, tr->scaleRoot, tr->scaleType))   // a key of the scale
            {
                g.setColour (Scales::isRoot (p, tr->scaleRoot) ? Theme::redBright : Theme::orange);
                g.fillEllipse ((float) r.getX() + 5.0f, (float) r.getCentreY() - 3.0f, 6.0f, 6.0f);
            }

            g.setColour (lit ? Theme::noteOutline : black ? Theme::textDim : (p % 12 == 0 ? Theme::noteOutline : Theme::textFaint));
            g.drawText (noteName (p), r.withTrimmedRight (6), juce::Justification::centredRight);
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
        group.clear();
        repaint();
    }

    bool deleteSelected()
    {
        if (! load())
            return false;
        if (group.size() > 1)   // several notes: all of them go
        {
            main.beginEdit();
            std::vector<int> indices = group;
            std::sort (indices.begin(), indices.end(), std::greater<int>());
            for (int i : indices)
                if (i >= 0 && i < (int) clip.notes.size())
                    clip.notes.erase (clip.notes.begin() + i);
            group.clear();
            selected = -1;
            commit();
            repaint();
            return true;
        }
        if (selected < 0 || selected >= (int) clip.notes.size())
            return false;
        main.beginEdit();
        clip.notes.erase (clip.notes.begin() + selected);
        selected = -1;
        group.clear();
        commit();
        return true;
    }

    int getSelected() const { return selected; }
    bool isNoteSelected (int i) const { return i == selected || std::find (group.begin(), group.end(), i) != group.end(); }
    bool hasGroup() const { return group.size() > 1; }

    bool copyGroup (bool cut)
    {
        if (! load() || group.size() < 2)
            return false;
        std::vector<Note> notes;
        double first = 1.0e18;
        for (int i : group)
            if (i >= 0 && i < (int) clip.notes.size())
            {
                notes.push_back (clip.notes[(size_t) i]);
                first = juce::jmin (first, clip.notes[(size_t) i].start);
            }
        if (notes.empty())
            return false;
        for (auto& n : notes)
            n.start -= first;
        RollPanel::noteClipboard = std::move (notes);
        if (cut)
            deleteSelected();
        return true;
    }

    // notes from the clipboard (or a copy of the selected ones) put in at a time counted from the start of the clip; they become the selection
    bool putNotes (const std::vector<Note>& notes, double at)
    {
        if (! load() || notes.empty())
            return false;
        main.beginEdit();
        group.clear();
        for (auto n : notes)
        {
            n.start += at;
            if (n.start >= clip.length - 1.0e-6)
                continue;
            n.length = juce::jmin (n.length, clip.length - n.start);
            clip.notes.push_back (n);
            group.push_back ((int) clip.notes.size() - 1);
        }
        if (group.empty())
        {
            main.cancelEdit();
            return false;
        }
        selected = group.back();
        commit();
        repaint();
        return true;
    }

    bool pasteGroup (double atSeconds)
    {
        if (! load() || RollPanel::noteClipboard.empty())
            return false;
        return putNotes (RollPanel::noteClipboard, juce::jlimit (0.0, clip.length, atSeconds - clip.start));
    }

    bool duplicateGroup()
    {
        if (! load() || group.size() < 2)
            return false;
        std::vector<Note> notes;
        double first = 1.0e18, last = 0.0;
        for (int i : group)
            if (i >= 0 && i < (int) clip.notes.size())
            {
                notes.push_back (clip.notes[(size_t) i]);
                first = juce::jmin (first, clip.notes[(size_t) i].start);
                last = juce::jmax (last, clip.notes[(size_t) i].start + clip.notes[(size_t) i].length);
            }
        for (auto& n : notes)
            n.start -= first;
        return putNotes (notes, last);   // (right after the group)
    }

    bool selectEverything()
    {
        if (! load() || clip.notes.empty())
            return false;
        group.clear();
        for (int i = 0; i < (int) clip.notes.size(); ++i)
            group.push_back (i);
        selected = (int) clip.notes.size() - 1;
        repaint();
        roll.repaintVelocity();
        return true;
    }

    void selectNote (int index)
    {
        selected = index;
        repaint();
    }

    // Lowest and highest pitch of the clip's notes (a default range around middle C when it has none).
    void pitchRange (int& lo, int& hi)
    {
        lo = 60;
        hi = 72;
        if (load() && ! clip.notes.empty())
        {
            lo = 127;
            hi = 0;
            for (auto& n : clip.notes)
            {
                lo = juce::jmin (lo, n.pitch);
                hi = juce::jmax (hi, n.pitch);
            }
        }
    }

    // Pitches that sound right now because the playhead is inside a note.
    std::bitset<128> soundingInClip()
    {
        std::bitset<128> set;
        if (! main.getEngine().isPlaying() || ! load())
            return set;
        const double pos = main.getEngine().getPositionSeconds() - clip.start;
        for (auto& n : clip.notes)
            if (pos >= n.start && pos < n.start + n.length && juce::isPositiveAndBelow (n.pitch, 128))
                set.set ((size_t) n.pitch);
        return set;
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = g.getClipBounds();
        g.fillAll (Theme::gridBg);
        if (! load())
            return;

        const double pps = main.getPixelsPerSecond();

        for (int p = maxPitch; p >= minPitch; --p)
        {
            const juce::Rectangle<int> r (0, pitchToY (p), getWidth(), rowH);
            if (! r.intersects (area))
                continue;
            g.setColour (isBlackKey (p) ? Theme::rowDark : Theme::rowLight);
            g.fillRect (r);
            if (track != nullptr && track->scaleType > 0)   // the key: rows outside the scale are dimmed, the root is tinted
            {
                if (! Scales::contains (p, track->scaleRoot, track->scaleType))
                {
                    g.setColour (juce::Colours::black.withAlpha (0.30f));
                    g.fillRect (r);
                }
                else if (Scales::isRoot (p, track->scaleRoot))
                {
                    g.setColour (Theme::orange.withAlpha (0.12f));
                    g.fillRect (r);
                }
            }
            g.setColour (p % 12 == 0 ? Theme::gridLineStrong : Theme::gridLine);
            g.drawHorizontalLine (r.getBottom() - 1, 0.0f, (float) getWidth());
        }

        // vertical grid from the tempo map: bars, beats and 16ths (when there is room)
        const auto& map = main.getTempoMap();
        const double leftT = xToTime (area.getX()), rightT = xToTime (area.getRight());
        const double beatPx = map.beatSecondsAt (leftT) * pps;
        map.forEachGridLine (leftT, rightT, 4, [&] (double t, int kind, int)
        {
            if (kind == 0 && beatPx / 4.0 < 9.0) return;
            if (kind == 1 && beatPx < 9.0) return;
            g.setColour (Theme::text.withAlpha (kind == 2 ? 0.16f : kind == 1 ? 0.08f : 0.035f));
            g.drawVerticalLine (timeToX (t), 0.0f, (float) getHeight());
        });

        // dim everything outside the clip
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRect (0, 0, juce::jmax (0, timeToX (clip.start)), getHeight());
        const int endX = timeToX (clip.end());
        g.fillRect (endX, 0, getWidth() - endX, getHeight());

        const double playingPos = main.getEngine().getPositionSeconds() - clip.start;   // where the playhead is inside this clip
        for (int i = 0; i < (int) clip.notes.size(); ++i)
        {
            const auto r = noteRect (clip.notes[(size_t) i]);
            if (! r.intersects (area))
                continue;
            const auto& note = clip.notes[(size_t) i];
            const bool sel = isNoteSelected (i);
            const bool playingNow = playingPos >= note.start && playingPos < note.start + note.length && main.getEngine().isPlaying();
            g.setColour (playingNow ? Theme::yellow
                                    : sel ? Theme::noteSel
                                          : Theme::midiClipSel.withMultipliedBrightness (0.6f + 0.5f * note.velocity));
            g.fillRect (r);
            g.setColour (sel ? Theme::text : Theme::noteOutline);
            g.drawRect (r, 1);

            if (r.getWidth() > 12)   // the two ends can be dragged to make the note shorter or longer: a small grip shows where
            {
                const int grip = edgeWidth (r);
                g.setColour (juce::Colours::white.withAlpha (sel ? 0.35f : 0.18f));
                g.fillRect (r.getX() + 1, r.getY() + 2, 2, r.getHeight() - 4);
                g.fillRect (r.getRight() - 3, r.getY() + 2, 2, r.getHeight() - 4);
                if (i == hoverNote && hoverEdge != 0)   // the end under the mouse lights up
                {
                    g.setColour (juce::Colours::white.withAlpha (0.35f));
                    g.fillRect (hoverEdge < 0 ? r.getX() : r.getRight() - grip, r.getY(), grip, r.getHeight());
                }
            }

            if (r.getWidth() >= 26)   // wide enough: name the note on the note itself
            {
                g.setColour (playingNow || sel ? Theme::noteOutline : Theme::text);
                g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
                g.drawText (noteName (note.pitch), r.reduced (3, 0), juce::Justification::centredLeft);
            }
        }

        const int px = timeToX (main.getEngine().getPositionSeconds());
        g.setColour (Theme::playhead);
        g.drawVerticalLine (px, 0.0f, (float) getHeight());

        if (marqueeOn)   // the selection rectangle
        {
            g.setColour (Theme::yellow.withAlpha (0.14f));
            g.fillRect (marqueeRect);
            g.setColour (Theme::yellow.withAlpha (0.8f));
            g.drawRect (marqueeRect, 1);
        }
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
        marqueeOn = false;

        if (e.mods.isShiftDown())   // Shift: click a note to add it to / take it out of the selection, drag on an empty spot for a rectangle
        {
            if (hit >= 0)
            {
                if (group.empty() && selected >= 0)
                    group.push_back (selected);
                const auto found = std::find (group.begin(), group.end(), hit);
                if (found != group.end())
                    group.erase (found);
                else
                    group.push_back (hit);
                selected = group.empty() ? -1 : group.back();
            }
            else
            {
                if (group.empty() && selected >= 0)
                    group.push_back (selected);
                marqueeBase = group;
                marqueeStart = e.getPosition();
                marqueeOn = true;
                marqueeRect = { marqueeStart.x, marqueeStart.y, 0, 0 };
                mode = Mode::Marquee;
            }
            roll.repaintVelocity();
            repaint();
            return;
        }

        if (hit >= 0)
        {
            const bool inGroup = group.size() > 1 && std::find (group.begin(), group.end(), hit) != group.end();
            if (! inGroup)
                group.clear();   // (a note outside the selection: it is the only selected one now)
            selected = hit;
            const auto& n = clip.notes[(size_t) hit];
            const auto r = noteRect (n);
            const int edge = r.getWidth() > 12 ? edgeAt (r, e.x) : 0;
            mode = edge > 0 ? Mode::Resize : edge < 0 && ! inGroup ? Mode::ResizeLeft : Mode::Move;
            grabTime = xToTime (e.x) - (clip.start + n.start);
            groupOrig.clear();
            if (inGroup)
                for (int i : group)
                    if (i >= 0 && i < (int) clip.notes.size())
                        groupOrig.push_back ({ i, clip.notes[(size_t) i] });
            preview (n.pitch);
        }
        else
        {
            group.clear();
            const double t = rawOrSnapped (xToTime (e.x), e) - clip.start;
            if (t < 0.0 || t >= clip.length)
                return;

            Note n;
            n.pitch = yToPitch (e.y);
            if (track->scaleSnap && track->scaleType > 0)
                n.pitch = Scales::nearest (n.pitch, track->scaleRoot, track->scaleType);
            n.start = t;
            n.length = juce::jmin (main.getTempoMap().beatSecondsAt (clip.start + t), clip.length - t);
            main.beginEdit();
            gestureEdited = true;   // adding the note and stretching it afterwards is one undo step
            if (track->scaleChord > 0)   // a chord: the other notes of it go in as well (the first one is the note that is stretched)
            {
                const auto notes = Scales::chord (n.pitch, track->scaleRoot, track->scaleType, track->scaleChord);
                for (size_t k = 1; k < notes.size(); ++k)
                {
                    Note extra = n;
                    extra.pitch = notes[k];
                    clip.notes.push_back (extra);
                }
            }
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
        if (mode == Mode::Marquee)
        {
            marqueeRect = juce::Rectangle<int>::leftTopRightBottom (juce::jmin (marqueeStart.x, e.x), juce::jmin (marqueeStart.y, e.y),
                                                                     juce::jmax (marqueeStart.x, e.x), juce::jmax (marqueeStart.y, e.y));
            group = marqueeBase;
            for (int i = 0; i < (int) clip.notes.size(); ++i)
                if (noteRect (clip.notes[(size_t) i]).intersects (marqueeRect) && std::find (group.begin(), group.end(), i) == group.end())
                    group.push_back (i);
            selected = group.empty() ? -1 : group.back();
            roll.repaintVelocity();
            repaint();
            return;
        }

        if (mode == Mode::None || selected < 0 || selected >= (int) clip.notes.size())
            return;

        auto& n = clip.notes[(size_t) selected];
        const double minLen = main.getTempoMap().beatSecondsAt (clip.start + n.start) / 4.0;

        // work out the new values first so the undo snapshot is only taken when something changes
        double newStart = n.start, newLength = n.length;
        int newPitch = n.pitch;
        if (mode == Mode::Resize)
        {
            const double end = rawOrSnapped (xToTime (e.x), e) - clip.start;
            newLength = juce::jmax (minLen, end - n.start);
        }
        else if (mode == Mode::ResizeLeft)   // the start moves, the end stays where it is
        {
            const double end = n.start + n.length;
            newStart = juce::jlimit (0.0, end - minLen, rawOrSnapped (xToTime (e.x), e) - clip.start);
            newLength = end - newStart;
        }
        else
        {
            const double start = rawOrSnapped (xToTime (e.x) - grabTime, e) - clip.start;
            newStart = juce::jlimit (0.0, juce::jmax (0.0, clip.length - minLen), start);
            newPitch = yToPitch (e.y);
            if (track->scaleSnap && track->scaleType > 0)
                newPitch = Scales::nearest (newPitch, track->scaleRoot, track->scaleType);
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

        if (group.size() > 1 && ! groupOrig.empty() && (mode == Mode::Move || mode == Mode::Resize))   // the whole selection follows
        {
            const Note* primary = nullptr;
            for (auto& g : groupOrig)
                if (g.first == selected)
                    primary = &g.second;
            if (primary != nullptr)
            {
                double dStart = newStart - primary->start, dLength = newLength - primary->length;
                int dPitch = newPitch - primary->pitch;
                double minStart = 1.0e18, maxEnd = 0.0;
                int minPitch = 127, maxPitch = 0;
                for (auto& g : groupOrig)
                {
                    minStart = juce::jmin (minStart, g.second.start);
                    maxEnd = juce::jmax (maxEnd, g.second.start + g.second.length);
                    minPitch = juce::jmin (minPitch, g.second.pitch);
                    maxPitch = juce::jmax (maxPitch, g.second.pitch);
                }
                if (mode == Mode::Move)
                {
                    dStart = juce::jlimit (-minStart, juce::jmax (-minStart, clip.length - maxEnd), dStart);
                    dPitch = juce::jlimit (-minPitch, 127 - maxPitch, dPitch);
                    dLength = 0.0;
                }
                else
                {
                    dStart = 0.0;
                    dPitch = 0;
                }
                for (auto& g : groupOrig)
                {
                    auto& m = clip.notes[(size_t) g.first];
                    m.start = g.second.start + dStart;
                    m.pitch = g.second.pitch + dPitch;
                    m.length = juce::jmax (0.01, g.second.length + dLength);
                }
                commit();
                repaint();
                roll.repaintVelocity();
                return;
            }
        }

        n.start = newStart;
        n.length = newLength;
        n.pitch = newPitch;
        commit();
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (mode == Mode::Marquee)
        {
            marqueeOn = false;
            if (group.size() == 1)   // (one note is just that note)
                group.clear();
            repaint();
        }
        mode = Mode::None;
        mouseMove (e);
    }

    // the mouse pointer shows what a drag does: the ends of a note stretch it, the middle moves it
    void mouseMove (const juce::MouseEvent& e) override
    {
        int newHover = -1, newEdge = 0;
        if (track != nullptr && load())
        {
            newHover = hitNote (e.getPosition());
            if (newHover >= 0)
            {
                const auto r = noteRect (clip.notes[(size_t) newHover]);
                newEdge = r.getWidth() > 12 ? edgeAt (r, e.x) : 0;
            }
        }
        setMouseCursor (newEdge != 0 ? juce::MouseCursor::LeftRightResizeCursor
                                     : newHover >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        if (newHover != hoverNote || newEdge != hoverEdge)
        {
            hoverNote = newHover;
            hoverEdge = newEdge;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoverNote = -1;
        hoverEdge = 0;
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown())
            main.zoomBy (w.deltaY > 0 ? 1.15 : 1.0 / 1.15, e.x);
        else
            Component::mouseWheelMove (e, w);
    }

private:
    enum class Mode { None, Move, Resize, ResizeLeft, Marquee };
    std::vector<int> group;                                  // (every selected note when there are several; selected is the last of them)
    std::vector<std::pair<int, Note>> groupOrig;             // (those notes as they were when a drag began)
    std::vector<int> marqueeBase;
    juce::Point<int> marqueeStart;
    juce::Rectangle<int> marqueeRect;
    bool marqueeOn = false;
    int hoverNote = -1, hoverEdge = 0;   // (the note under the mouse, and which end of it: -1 start, 1 end, 0 the middle)

    // the width of the grab zone at each end of a note: wide enough to hit, never more than a third of a short note
    static int edgeWidth (juce::Rectangle<int> r) { return juce::jlimit (4, 9, r.getWidth() / 3); }
    static int edgeAt (juce::Rectangle<int> r, int x)
    {
        const int w = edgeWidth (r);
        return x >= r.getRight() - w ? 1 : x < r.getX() + w ? -1 : 0;
    }

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

    // -1: velocity bars; otherwise the lane shows one controller of the clip (a CC number, or CcEvent::kPitchBend / kAftertouch)
    void setMode (int newMode)
    {
        mode = newMode;
        ccActive = -1;
        repaint();
    }
    int getMode() const { return mode; }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::rowDark);
        if (! load())
            return;

        if (mode >= 0)
        {
            paintCc (g);
            return;
        }

        g.setColour (Theme::text.withAlpha (0.06f));
        for (float f : { 0.25f, 0.5f, 0.75f })
            g.drawHorizontalLine ((int) valueToY (f), 0.0f, (float) getWidth());

        for (int i = 0; i < (int) clip.notes.size(); ++i)
        {
            const auto& n = clip.notes[(size_t) i];
            const int x = barX (n);
            const float top = valueToY (n.velocity);
            g.setColour (roll.grid->isNoteSelected (i) ? Theme::noteSel : Theme::midiClipSel.withMultipliedBrightness (0.6f + 0.5f * n.velocity));
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

        if (mode >= 0)
        {
            ccMouseDown (e);
            return;
        }

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
        if (mode >= 0)
        {
            ccMouseDrag (e);
            return;
        }
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

    void mouseUp (const juce::MouseEvent&) override { active = -1; ccActive = -1; }

private:
    //----------------------------------------------------------------------------- controller lane
    // The events of the shown controller are drawn as a stepped line (a controller keeps its value until the next event) with a
    // dot per event. Click an empty spot to add one, drag a dot to move it, right-click a dot to delete it.
    int ccX (const CcEvent& e) const { return (int) std::round ((clip.start + e.time) * main.getPixelsPerSecond()) - main.getScrollX() + 1; }
    float ccY (float v) const { return 4.0f + (1.0f - juce::jlimit (0.0f, 1.0f, v)) * (float) (getHeight() - 8); }
    float ccValueAt (int y) const { return juce::jlimit (0.0f, 1.0f, 1.0f - ((float) y - 4.0f) / (float) (getHeight() - 8)); }

    void paintCc (juce::Graphics& g)
    {
        g.setColour (Theme::text.withAlpha (0.06f));
        for (float f : { 0.25f, 0.5f, 0.75f })
            g.drawHorizontalLine ((int) ccY (f), 0.0f, (float) getWidth());
        if (mode == CcEvent::kPitchBend)   // the centre of the wheel
        {
            g.setColour (Theme::text.withAlpha (0.2f));
            g.drawHorizontalLine ((int) ccY (0.5f), 0.0f, (float) getWidth());
        }

        std::vector<const CcEvent*> events;
        for (auto& e : clip.ccs)
            if (e.controller == mode)
                events.push_back (&e);

        if (events.empty())
        {
            g.setColour (Theme::textFaint);
            g.setFont (juce::FontOptions (12.5f));
            g.drawText ("Nothing recorded for this controller. Click to add a point, drag it to move, right-click to delete.",
                        juce::Rectangle<int> (12, 0, getWidth() - 24, getHeight()), juce::Justification::centredLeft);
            return;
        }

        juce::Path line;
        for (size_t i = 0; i < events.size(); ++i)
        {
            const float x = (float) ccX (*events[i]), y = ccY (events[i]->value);
            if (i == 0)
                line.startNewSubPath ((float) ccX (*events[0]), y);
            else
                line.lineTo (x, ccY (events[i - 1]->value));   // holds the old value until here...
            line.lineTo (x, y);                                // ...then jumps
        }
        line.lineTo ((float) getWidth(), ccY (events.back()->value));
        g.setColour (Theme::yellow.withAlpha (0.8f));
        g.strokePath (line, juce::PathStrokeType (1.5f));

        for (auto* e : events)
        {
            g.setColour (Theme::yellow);
            g.fillEllipse ((float) ccX (*e) - 3.5f, ccY (e->value) - 3.5f, 7.0f, 7.0f);
        }
    }

    int ccNearest (const juce::MouseEvent& e) const
    {
        int best = -1;
        float bestDistance = 10.0f;
        for (int i = 0; i < (int) clip.ccs.size(); ++i)
        {
            if (clip.ccs[(size_t) i].controller != mode)
                continue;
            const float dx = (float) (ccX (clip.ccs[(size_t) i]) - e.x), dy = ccY (clip.ccs[(size_t) i].value) - (float) e.y;
            const float d = std::sqrt (dx * dx + dy * dy);
            if (d < bestDistance)
            {
                bestDistance = d;
                best = i;
            }
        }
        return best;
    }

    void ccCommit (std::vector<CcEvent> events)
    {
        if (! gestureEdited)
        {
            main.beginEdit();
            gestureEdited = true;
        }
        main.getEngine().setMidiCcs (*track, clipId, std::move (events));
        roll.repaintGrid();
    }

    void ccMouseDown (const juce::MouseEvent& e)
    {
        ccActive = ccNearest (e);

        if (e.mods.isPopupMenu())   // right-click deletes the dot under the mouse
        {
            if (ccActive >= 0)
            {
                auto events = clip.ccs;
                events.erase (events.begin() + ccActive);
                ccCommit (std::move (events));
            }
            ccActive = -1;
            return;
        }

        if (ccActive >= 0)
            return;

        // an empty spot: a new event there (snapped to the grid unless Alt is held), which can be dragged right away
        const double t = (e.x + main.getScrollX()) / main.getPixelsPerSecond();
        const double snapped = e.mods.isAltDown() ? t : main.snapTime (t);
        CcEvent ev;
        ev.controller = mode;
        ev.time = juce::jlimit (0.0, juce::jmax (0.0, clip.length - 0.001), snapped - clip.start);
        ev.value = ccValueAt (e.y);
        auto events = clip.ccs;
        events.push_back (ev);
        std::stable_sort (events.begin(), events.end(), [] (const CcEvent& a, const CcEvent& b) { return a.time < b.time; });
        ccCommit (events);
        load();
        ccActive = ccFind (ev);
    }

    int ccFind (const CcEvent& ev) const
    {
        for (int i = 0; i < (int) clip.ccs.size(); ++i)
            if (clip.ccs[(size_t) i].controller == ev.controller && clip.ccs[(size_t) i].time == ev.time && clip.ccs[(size_t) i].value == ev.value)
                return i;
        return -1;
    }

    void ccMouseDrag (const juce::MouseEvent& e)
    {
        if (ccActive < 0 || ccActive >= (int) clip.ccs.size() || ! load() || ccActive >= (int) clip.ccs.size())
            return;

        auto events = clip.ccs;
        auto ev = events[(size_t) ccActive];
        const double t = (e.x + main.getScrollX()) / main.getPixelsPerSecond();
        const double snapped = e.mods.isAltDown() ? t : main.snapTime (t);
        ev.time = juce::jlimit (0.0, juce::jmax (0.0, clip.length - 0.001), snapped - clip.start);
        ev.value = ccValueAt (e.y);
        events[(size_t) ccActive] = ev;
        std::stable_sort (events.begin(), events.end(), [] (const CcEvent& a, const CcEvent& b) { return a.time < b.time; });
        ccCommit (events);
        load();
        ccActive = ccFind (ev);
    }

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
    int mode = -1;        // -1 velocity bars, else the controller shown
    int ccActive = -1;    // index in clip.ccs of the event being dragged
};

//==============================================================================
PianoRoll::PianoRoll (MainComponent& m) : main (m)
{
    keys = std::make_unique<Keys> (*this);
    grid = std::make_unique<Grid> (*this);
    velocity = std::make_unique<Velocity> (*this);
    addAndMakeVisible (*velocity);

    // the lane under the notes: velocity, or a controller of the clip (recorded from a mod wheel, pedal, pitch wheel... or drawn)
    laneBox.addItem ("Velocity", 1);
    laneBox.addSeparator();
    const std::pair<int, const char*> common[] = { { 1, "Mod wheel (CC1)" }, { 11, "Expression (CC11)" }, { 7, "Volume (CC7)" }, { 10, "Pan (CC10)" },
                                                    { 64, "Sustain pedal (CC64)" }, { 4, "Foot controller / hi-hat (CC4)" }, { 2, "Breath (CC2)" },
                                                    { 74, "Filter cutoff (CC74)" }, { CcEvent::kPitchBend, "Pitch bend" }, { CcEvent::kAftertouch, "Aftertouch" } };
    for (auto& [controller, name] : common)
        laneBox.addItem (name, 100 + controller);
    laneBox.setSelectedId (1, juce::dontSendNotification);
    laneBox.setTooltip ("What the lane under the notes shows: the velocity of each note, or a controller of this clip");
    laneBox.onChange = [this]
    {
        const int id = laneBox.getSelectedId();
        velocity->setMode (id >= 100 ? id - 100 : -1);
        repaint();
    };
    addAndMakeVisible (laneBox);

    title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    addAndMakeVisible (title);

    waveform.addItemList ({ "Sine", "Saw", "Square", "Triangle" }, 1);
    waveform.onChange = [this] { main.beginEdit(); applySynthParams(); };
    addAndMakeVisible (waveform);

    // sounds for the built-in synth (see SynthPresets.h): every setting of the synth, by category
    presetBox.setTextWhenNothingSelected ("Sounds...");
    {
        int id = 1;
        const char* lastCategory = "";
        for (auto& p : SynthPresets::all())
        {
            if (std::string (lastCategory) != p.category)
            {
                if (id > 1)
                    presetBox.addSeparator();
                presetBox.addSectionHeading (p.category);
                lastCategory = p.category;
            }
            presetBox.addItem (p.name, id++);
        }
    }
    presetBox.setTooltip ("Ready-made sounds for the built-in synth: they set the oscillators, the filter, the envelopes and the LFO (you can adjust them after)");
    presetBox.onChange = [this]
    {
        const int index = presetBox.getSelectedId() - 1;
        presetBox.setSelectedId (0, juce::dontSendNotification);
        if (track == nullptr || index < 0 || index >= (int) SynthPresets::all().size())
            return;
        const auto& pr = SynthPresets::all()[(size_t) index].values;
        main.beginEdit();
        track->synth.apply (pr);
        waveform.setSelectedId (pr.waveform + 1, juce::dontSendNotification);
        attack.setValue (pr.attack, juce::dontSendNotification);
        release.setValue (pr.release, juce::dontSendNotification);
        cutoff.setValue (pr.cutoff, juce::dontSendNotification);
    };
    addAndMakeVisible (presetBox);

    soundButton.setTooltip ("Opens every setting of the synth: oscillators, unison, filter, envelopes, LFO and glide");
    soundButton.onClick = [this] { if (track != nullptr) main.openSynthPanel (*track); };
    addAndMakeVisible (soundButton);

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

    // the key of the clip: notes outside it are dimmed, new notes can snap to it, a click can add a chord
    for (int i = 0; i < 12; ++i)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        rootBox.addItem (names[i], i + 1);
    }
    for (size_t i = 0; i < Scales::defs().size(); ++i)
        scaleBox.addItem (Scales::defs()[i].name, (int) i + 1);
    chordBox.addItem ("Single notes", 1);
    chordBox.addItem ("Triads on click", 2);
    chordBox.addItem ("7th chords on click", 3);
    rootBox.setTooltip ("The root note of the key");
    scaleBox.setTooltip ("The scale of the key: the rows of its notes stay light, the others are dimmed (the root is tinted)");
    chordBox.setTooltip ("A click adds a whole chord, built from the notes of the scale (or a major chord when there is no scale)");
    snapToggle.setTooltip ("New notes and moved notes jump to the nearest note of the scale");
    fitScaleButton.setTooltip ("Moves every note of this clip that is outside the scale to the nearest note of it");
    scaleLabel.setText ("Key", juce::dontSendNotification);
    scaleLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    scaleLabel.setColour (juce::Label::textColourId, Theme::textDim);
    auto scaleChanged = [this]
    {
        if (track == nullptr)
            return;
        main.beginEdit();
        track->scaleRoot = juce::jmax (0, rootBox.getSelectedId() - 1);
        track->scaleType = juce::jmax (0, scaleBox.getSelectedId() - 1);
        track->scaleChord = juce::jmax (0, chordBox.getSelectedId() - 1);
        track->scaleSnap = snapToggle.getToggleState();
        repaintGrid();
    };
    rootBox.onChange = scaleChanged;
    scaleBox.onChange = scaleChanged;
    chordBox.onChange = scaleChanged;
    snapToggle.onClick = scaleChanged;
    fitScaleButton.onClick = [this] { fitNotesToScale(); };
    for (juce::Component* c : std::initializer_list<juce::Component*> { &rootBox, &scaleBox, &chordBox, &snapToggle, &fitScaleButton, &scaleLabel })
        addAndMakeVisible (c);

    closeButton.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeButton);

    fitButton.setTooltip ("Scroll so the notes of this clip are in view");
    fitButton.onClick = [this] { scrollToNotes(); };
    addAndMakeVisible (fitButton);

    pluginButton.setTooltip ("Open the VST instrument's own window");
    pluginButton.onClick = [this] { if (track != nullptr) main.openInstrumentEditor (*track); };
    addChildComponent (pluginButton);

    holder.addAndMakeVisible (*keys);
    holder.addAndMakeVisible (*grid);
    viewport.setViewedComponent (&holder, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
    viewport.toBack();   // (it spans the whole width, also the space left of the keys where the key, scale and snap controls are: they must stay on top to be clickable)
}

PianoRoll::~PianoRoll() = default;

void PianoRoll::showClip (Track* t, int id)
{
    const bool changed = t != track || id != clipId;
    track = t;
    clipId = id;
    grid->setClip (t, id);
    velocity->setClip (t, id);
    refreshLaneChoices();
    refreshScaleControls();
    toolbarShowsPlugin = ! (t != nullptr && (t->instrument != nullptr || t->sampler != nullptr));   // force updateToolbar() to apply
    updateToolbar();

    if (t != nullptr)
    {
        title.setText (t->name + "  -  MIDI clip", juce::dontSendNotification);
        waveform.setSelectedId (t->synth.waveform.load() + 1, juce::dontSendNotification);
        attack.setValue (t->synth.attack, juce::dontSendNotification);
        release.setValue (t->synth.release, juce::dontSendNotification);
        cutoff.setValue (t->synth.cutoff, juce::dontSendNotification);

        if (changed)
            scrollToNotes();   // (waits for the panel's layout if it is not sized yet)
    }
    repaint();
}

void PianoRoll::refreshScaleControls()
{
    if (track == nullptr)
        return;
    rootBox.setSelectedId (track->scaleRoot + 1, juce::dontSendNotification);
    scaleBox.setSelectedId (juce::jlimit (0, (int) Scales::defs().size() - 1, track->scaleType) + 1, juce::dontSendNotification);
    chordBox.setSelectedId (juce::jlimit (0, 2, track->scaleChord) + 1, juce::dontSendNotification);
    snapToggle.setToggleState (track->scaleSnap, juce::dontSendNotification);
}

// Every note of the clip that is outside the scale goes to the nearest note of it (one undo step)
void PianoRoll::fitNotesToScale()
{
    MidiClip clip;
    if (track == nullptr || track->scaleType <= 0 || ! main.getEngine().getMidiClip (*track, clipId, clip))
        return;
    bool any = false;
    for (auto& n : clip.notes)
        any = any || ! Scales::contains (n.pitch, track->scaleRoot, track->scaleType);
    if (! any)
        return;
    main.beginEdit();
    for (auto& n : clip.notes)
        n.pitch = Scales::nearest (n.pitch, track->scaleRoot, track->scaleType);
    main.getEngine().setMidiNotes (*track, clipId, std::move (clip.notes));
    repaintGrid();
}

void PianoRoll::refreshLaneChoices()
{
    MidiClip clip;
    if (track == nullptr || ! main.getEngine().getMidiClip (*track, clipId, clip))
        return;

    for (auto& cc : clip.ccs)
        if (laneBox.indexOfItemId (100 + cc.controller) < 0)   // a controller that is not in the usual list: "CC 21"
            laneBox.addItem ("CC " + juce::String (cc.controller), 100 + cc.controller);
}

std::bitset<128> PianoRoll::activePitches()
{
    auto set = grid->soundingInClip();
    for (int p = 0; p < 128; ++p)
        if (main.isLiveNoteDown (p))
            set.set ((size_t) p);
    return set;
}

// Puts the clip's notes in the middle of the view (or its highest notes at the top when they don't all fit).
void PianoRoll::scrollToNotes()
{
    const int viewH = viewport.getMaximumVisibleHeight();
    if (viewH <= 0)
    {
        pendingScroll = true;
        return;
    }

    int lo = 60, hi = 72;
    grid->pitchRange (lo, hi);
    const int spanPx = (hi - lo + 1) * rowH;
    int y = spanPx >= viewH - 2 * rowH ? pitchToY (hi) - rowH
                                       : pitchToY (hi) - (viewH - spanPx) / 2;
    viewport.setViewPosition (0, juce::jlimit (0, juce::jmax (0, totalHeight - viewH), y));
    pendingScroll = false;
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

bool PianoRoll::copyNotes (bool cut) { return grid->copyGroup (cut); }
bool PianoRoll::pasteNotes (double atSeconds) { return grid->pasteGroup (atSeconds); }
bool PianoRoll::duplicateNotes() { return grid->duplicateGroup(); }
bool PianoRoll::selectAllNotes() { return grid->selectEverything(); }
void PianoRoll::repaintVelocity() { if (velocity != nullptr) velocity->repaint(); }

bool PianoRoll::deleteSelectedNote() { return grid->deleteSelected(); }

void PianoRoll::updateToolbar()
{
    const bool plugin = track != nullptr && (track->instrument != nullptr || track->sampler != nullptr);   // (the sampler has a window of its own, too)
    pluginButton.setButtonText (track != nullptr && track->sampler != nullptr ? "Sampler window" : "Plugin window");
    if (plugin == toolbarShowsPlugin)
        return;
    toolbarShowsPlugin = plugin;

    pluginButton.setVisible (plugin);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &waveform, &presetBox, &soundButton, &attackLabel, &attack, &releaseLabel, &release, &cutoffLabel, &cutoff })
        c->setVisible (! plugin);
}

void PianoRoll::repaintGrid()
{
    updateToolbar();   // cheap; keeps the toolbar right when a plugin is loaded or removed while the roll is open
    grid->repaint();
    keys->repaint();   // keys light up while their notes sound
    velocity->repaint();
}

void PianoRoll::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
    g.setColour (Theme::panel);
    g.fillRect (0, 0, getWidth(), toolbarHeight);
    g.setColour (Theme::border);
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());
    g.drawHorizontalLine (toolbarHeight - 1, 0.0f, (float) getWidth());

    // how-to in the empty space to the left of the keys (under the controls of the key)
    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawFittedText ("Click an empty spot to add a note, then drag right to make it longer. Drag the middle of a note to move it, drag either end to make it shorter or longer, "
                      "right-click to delete. Shift+drag selects several notes (Shift+click adds one); they move, stretch, copy, paste and delete together. Alt = free (no snapping). Ctrl + mouse wheel = zoom.",
                      juce::Rectangle<int> (14, toolbarHeight + 128, MainComponent::controlsWidth - keyWidth - 24, 110),
                      juce::Justification::topLeft, 8);

    // label for the velocity lane at the bottom
    const juce::Rectangle<int> labelArea (0, getHeight() - velocityHeight, MainComponent::controlsWidth, velocityHeight);
    g.setColour (Theme::panel);
    g.fillRect (labelArea);
    g.setColour (Theme::text.withAlpha (0.6f));
    g.setFont (11.5f);
    g.drawText (velocity->getMode() < 0 ? "drag the bars" : "click: add, drag: move, right-click: delete",
                labelArea.withTrimmedTop (36).reduced (12, 0), juce::Justification::centredLeft);
    g.setColour (Theme::border);
    g.drawHorizontalLine (getHeight() - velocityHeight, 0.0f, (float) getWidth());
}

void PianoRoll::resized()
{
    auto bar = getLocalBounds().removeFromTop (toolbarHeight).reduced (8, 5);
    closeButton.setBounds (bar.removeFromRight (26));
    bar.removeFromRight (6);
    fitButton.setBounds (bar.removeFromRight (96));
    title.setBounds (bar.removeFromLeft (MainComponent::controlsWidth - 20));
    const auto waveArea = bar.removeFromLeft (100);
    waveform.setBounds (waveArea);
    pluginButton.setBounds (waveArea.withWidth (130));
    bar.removeFromLeft (14);
    attackLabel.setBounds (bar.removeFromLeft (46));
    attack.setBounds (bar.removeFromLeft (110));
    bar.removeFromLeft (10);
    releaseLabel.setBounds (bar.removeFromLeft (52));
    release.setBounds (bar.removeFromLeft (110));
    bar.removeFromLeft (10);
    cutoffLabel.setBounds (bar.removeFromLeft (46));
    cutoff.setBounds (bar.removeFromLeft (130));
    bar.removeFromLeft (14);
    presetBox.setBounds (bar.removeFromLeft (150));
    bar.removeFromLeft (6);
    soundButton.setBounds (bar.removeFromLeft (110));

    {   // the key, in the empty space to the left of the keys
        const int x = 12, w = MainComponent::controlsWidth - keyWidth - 24;
        int y = toolbarHeight + 10;
        scaleLabel.setBounds (x, y, 40, 22);
        rootBox.setBounds (x + 40, y, 54, 24);
        scaleBox.setBounds (x + 100, y, w - 100, 24);
        y += 30;
        chordBox.setBounds (x, y, w, 24);
        y += 30;
        snapToggle.setBounds (x, y, 112, 24);
        fitScaleButton.setBounds (x + 118, y, w - 118, 24);
    }
    laneBox.setBounds (12, getHeight() - velocityHeight + 8, MainComponent::controlsWidth - 24, 24);
    viewport.setBounds (0, toolbarHeight, getWidth(), juce::jmax (0, getHeight() - toolbarHeight - velocityHeight));
    velocity->setBounds (MainComponent::controlsWidth, getHeight() - velocityHeight,
                         juce::jmax (0, getWidth() - MainComponent::controlsWidth), velocityHeight);
    const int w = viewport.getMaximumVisibleWidth();
    holder.setSize (w, totalHeight);
    keys->setBounds (MainComponent::controlsWidth - keyWidth, 0, keyWidth, totalHeight);
    grid->setBounds (MainComponent::controlsWidth, 0, juce::jmax (0, w - MainComponent::controlsWidth), totalHeight);

    if (pendingScroll)   // a clip was opened before the panel had its size
        scrollToNotes();
}
