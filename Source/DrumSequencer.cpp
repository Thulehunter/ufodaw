#include "DrumSequencer.h"
#include "MainComponent.h"

namespace
{
    constexpr int rowH = DrumSequencer::rowHeight;

    // Pad 0 (kick) is drawn at the bottom, like on a drum machine.
    int padToY (int pad) { return (DrumKit::numPads - 1 - pad) * rowH; }
    int yToPad (int y)   { return juce::jlimit (0, DrumKit::numPads - 1, DrumKit::numPads - 1 - y / rowH); }

    // warm hues only: kick is red, crash yellow, the pads in between shade through orange
    juce::Colour padColour (int pad) { return juce::Colour::fromHSV (0.01f + 0.125f * (float) pad / (float) (DrumKit::numPads - 1), 0.72f, 0.97f, 1.0f); }
}

//==============================================================================
// Left column: pad names. Click to hear a pad, right-click to load your own sample.
class DrumSequencer::Pads : public juce::Component
{
public:
    explicit Pads (DrumSequencer& s) : owner (s) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        auto* track = owner.getTrack();
        if (track == nullptr || track->kit == nullptr)
            return;

        for (int pad = 0; pad < DrumKit::numPads; ++pad)
        {
            const auto& info = track->kit->getPad (pad);
            const juce::Rectangle<int> r (0, padToY (pad), getWidth(), rowH);

            g.setColour (padColour (pad).withAlpha (pad == pressed ? 0.9f : 0.25f));
            g.fillRect (r.withTrimmedRight (getWidth() - 8).reduced (0, 1));
            g.setColour (Theme::text.withAlpha (0.9f));
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText (info.name, r.withTrimmedLeft (16).withWidth (110), juce::Justification::centredLeft);

            g.setColour (Theme::text.withAlpha (0.45f));
            g.setFont (10.0f);
            g.drawText (info.file != juce::File() ? info.file.getFileName() : track->kit->hasSample (pad) ? juce::String ("built-in sound") : juce::String ("empty"),
                        r.withTrimmedLeft (126).withTrimmedRight (6), juce::Justification::centredLeft, true);

            g.setColour (Theme::laneBg);
            g.drawHorizontalLine (r.getBottom() - 1, 0.0f, (float) getWidth());
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto* track = owner.getTrack();
        if (track == nullptr || track->kit == nullptr)
            return;

        const int pad = yToPad (e.y);
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            menu.addItem (1, "Load sample...");
            menu.addItem (2, "Use built-in sound", track->kit->getPad (pad).file != juce::File());
            menu.addSeparator();
            menu.addItem (3, "Open drum rack (levels, choke, sampler)...");
            menu.showMenuAsync ({}, [this, pad] (int result)
            {
                if (result == 3 && owner.getTrack() != nullptr) owner.main.openDrumRack (*owner.getTrack(), pad);
                if (result == 1) chooseSample (pad);
                if (result == 2) { owner.main.beginEdit(); owner.main.getEngine().resetDrumPad (*owner.getTrack(), pad); repaint(); }
            });
            return;
        }

        pressed = pad;
        owner.main.getEngine().previewNote (*track, track->kit->getPad (pad).note, true, 0.9f);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        pressed = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (owner.getTrack() != nullptr && owner.getTrack()->kit != nullptr && ! e.mods.isPopupMenu())
            owner.main.openDrumRack (*owner.getTrack(), yToPad (e.y));
    }

private:
    void chooseSample (int pad)
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a drum sample", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this, pad] (const juce::FileChooser& fc)
                              {
                                  auto f = fc.getResult();
                                  if (f == juce::File() || owner.getTrack() == nullptr)
                                      return;
                                  if (owner.main.getEngine().loadDrumSample (*owner.getTrack(), pad, f))
                                      repaint();
                                  else
                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                              "Cannot open sample", f.getFullPathName());
                              });
    }

    DrumSequencer& owner;
    std::unique_ptr<juce::FileChooser> chooser;
    int pressed = -1;
};

//==============================================================================
class DrumSequencer::Grid : public juce::Component
{
public:
    explicit Grid (DrumSequencer& s) : owner (s), main (s.main) {}

    void setClip (Track* t, int id)
    {
        track = t;
        clipId = id;
        group.clear();
        repaint();
    }

    void clearPattern()
    {
        if (! load() || clip.notes.empty())
            return;
        main.beginEdit();
        main.getEngine().setMidiNotes (*track, clipId, {});
        repaint();
    }

    // ---- several hits selected at once (Ctrl+drag a rectangle, Shift+click adds one)
    bool isNoteSelected (int i) const { return std::find (group.begin(), group.end(), i) != group.end(); }

    bool deleteGroup()
    {
        if (! load() || group.size() < 2)
            return false;
        main.beginEdit();
        std::vector<int> indices = group;
        std::sort (indices.begin(), indices.end(), std::greater<int>());
        for (int i : indices)
            if (i >= 0 && i < (int) clip.notes.size())
                clip.notes.erase (clip.notes.begin() + i);
        group.clear();
        main.getEngine().setMidiNotes (*track, clipId, clip.notes);
        repaint();
        return true;
    }

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
            deleteGroup();
        return true;
    }

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
        main.getEngine().setMidiNotes (*track, clipId, clip.notes);
        repaint();
        return true;
    }

    bool pasteGroup (double atSeconds)
    {
        if (! load() || RollPanel::noteClipboard.empty())
            return false;
        // (on a step of the grid)
        const double s = stepSeconds();
        const double at = std::round (juce::jlimit (0.0, clip.length, atSeconds - clip.start) / s) * s;
        return putNotes (RollPanel::noteClipboard, at);
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
        return putNotes (notes, std::ceil (last / stepSeconds() - 1.0e-9) * stepSeconds());   // (right after the group, on a step)
    }

    bool selectEverything()
    {
        if (! load() || clip.notes.empty())
            return false;
        group.clear();
        for (int i = 0; i < (int) clip.notes.size(); ++i)
            group.push_back (i);
        repaint();
        return true;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::gridBg);
        if (! load() || track->kit == nullptr)
            return;

        const auto area = g.getClipBounds();
        const double pps = main.getPixelsPerSecond();
        const double step = stepSeconds();

        for (int pad = 0; pad < DrumKit::numPads; ++pad)
        {
            const juce::Rectangle<int> r (0, padToY (pad), getWidth(), rowH);
            if (! r.intersects (area))
                continue;
            g.setColour (pad % 2 == 0 ? Theme::rowLight : Theme::background);
            g.fillRect (r);
            g.setColour (Theme::laneBg);
            g.drawHorizontalLine (r.getBottom() - 1, 0.0f, (float) getWidth());
        }

        // grid lines of the steps, counted from the start of the clip (a clip starts on a bar); the step length is the one in
        // force at the start of the clip
        const double firstT = std::floor ((xToTime (area.getX()) - clip.start) / step) * step + clip.start;
        for (double t = firstT;; t += step)
        {
            const int x = timeToX (t);
            if (x > area.getRight()) break;
            const auto index = (long long) std::llround ((t - clip.start) / step);
            const bool isBar = index % 16 == 0, isBeat = index % 4 == 0;
            if (! isBeat && step * pps < 8.0) continue;
            g.setColour (Theme::text.withAlpha (isBar ? 0.18f : isBeat ? 0.09f : 0.04f));
            g.drawVerticalLine (x, 0.0f, (float) getHeight());
        }

        // hits (a hit is as long as its length: drag an end to make it shorter or longer, the middle moves it)
        for (int i = 0; i < (int) clip.notes.size(); ++i)
        {
            const auto& n = clip.notes[(size_t) i];
            const int pad = track->kit->padForNote (n.pitch);
            if (pad < 0)
                continue;
            // the height of a hit shows its velocity (Ctrl+drag a hit up or down to change it)
            const int full = rowH - 4;
            const int h = juce::jmax (4, (int) std::round ((float) full * (0.2f + 0.8f * n.velocity)));
            const auto cell = noteRect (n, pad);
            const juce::Rectangle<int> r (cell.getX(), cell.getY() + 2 + (full - h), cell.getWidth(), h);
            if (! r.intersects (area))
                continue;
            g.setColour (padColour (pad).withMultipliedBrightness (0.7f + 0.4f * n.velocity));
            g.fillRoundedRectangle (r.toFloat(), 3.0f);
            if (isNoteSelected (i))
            {
                g.setColour (juce::Colours::white.withAlpha (0.9f));
                g.drawRoundedRectangle (r.toFloat(), 3.0f, 1.5f);
            }
            if (r.getWidth() > 12)   // small grips at both ends
            {
                g.setColour (juce::Colours::white.withAlpha (i == hoverNote ? 0.5f : 0.22f));
                g.fillRect (r.getX() + 2, r.getY() + 2, 2, juce::jmax (2, r.getHeight() - 4));
                g.fillRect (r.getRight() - 4, r.getY() + 2, 2, juce::jmax (2, r.getHeight() - 4));
            }
        }

        // dim everything outside the clip
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRect (0, 0, juce::jmax (0, timeToX (clip.start)), getHeight());
        const int endX = timeToX (clip.end());
        g.fillRect (endX, 0, getWidth() - endX, getHeight());

        g.setColour (Theme::playhead);
        g.drawVerticalLine (timeToX (main.getEngine().getPositionSeconds()), 0.0f, (float) getHeight());

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
        if (! load() || track->kit == nullptr)
            return;

        gestureEdited = false;
        moved = false;
        lastCell = { -1, -1 };
        active = -1;

        const int hit = noteAt (e.getPosition());
        const auto cell = cellAt (e.getPosition());

        if (e.mods.isPopupMenu())   // right click erases (also by dragging over hits)
        {
            mode = Mode::Erase;
            if (hit >= 0)
            {
                main.beginEdit();
                gestureEdited = true;
                clip.notes.erase (clip.notes.begin() + hit);
                main.getEngine().setMidiNotes (*track, clipId, clip.notes);
                repaint();
            }
            return;
        }

        if (e.mods.isShiftDown() && hit >= 0)   // Shift+click: a hit joins the selection, or leaves it
        {
            if (const auto found = std::find (group.begin(), group.end(), hit); found != group.end())
                group.erase (found);
            else
                group.push_back (hit);
            mode = Mode::None;
            repaint();
            return;
        }

        if (hit < 0 && e.mods.isCtrlDown())   // Ctrl+drag on an empty spot: a rectangle that selects the hits inside it (Shift adds)
        {
            marqueeBase = e.mods.isShiftDown() ? group : std::vector<int>();
            group = marqueeBase;
            marqueeStart = e.getPosition();
            marqueeRect = { marqueeStart.x, marqueeStart.y, 0, 0 };
            marqueeOn = true;
            mode = Mode::Marquee;
            repaint();
            return;
        }

        if (hit >= 0)
        {
            const bool inGroup = group.size() > 1 && isNoteSelected (hit);
            if (! inGroup)
                group.clear();
            if (e.mods.isCtrlDown())   // velocity: drag up for harder, down for softer
            {
                mode = Mode::Velocity;
                velIndex = hit;
                velStartY = e.y;
                velStartValue = clip.notes[(size_t) hit].velocity;
                return;
            }

            // the ends of a hit change its length, the middle moves it (a click without a drag erases it, as before)
            active = hit;
            original = clip.notes[(size_t) hit];
            groupOrig.clear();
            if (inGroup)
                for (int i : group)
                    if (i >= 0 && i < (int) clip.notes.size())
                        groupOrig.push_back ({ i, clip.notes[(size_t) i] });
            const auto r = noteRect (original, track->kit->padForNote (original.pitch));
            const int edge = edgeAt (r, e.x);
            mode = edge > 0 ? Mode::ResizeEnd : edge < 0 && ! inGroup ? Mode::ResizeStart : Mode::Move;
            grabTime = xToTime (e.x) - (clip.start + original.start);
            main.getEngine().previewNote (*track, original.pitch, true, original.velocity);
            return;
        }

        if (cell.x < 0)
        {
            mode = Mode::None;
            return;
        }

        group.clear();
        // an empty spot: a new hit; drag right to make it longer (Shift+drag paints one hit per step instead)
        mode = e.mods.isShiftDown() ? Mode::Paint : Mode::Stretch;
        apply (cell);
        if (mode == Mode::Stretch)
        {
            active = findNote (cell.x, cell.y);
            if (active >= 0)
                original = clip.notes[(size_t) active];
            else
                mode = Mode::None;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (mode == Mode::None || ! load())
            return;

        if (mode == Mode::Velocity)
        {
            if (velIndex < 0 || velIndex >= (int) clip.notes.size())
                return;
            const float v = juce::jlimit (0.05f, 1.0f, velStartValue + (float) (velStartY - e.y) / (float) (rowH * 3));
            if (! gestureEdited)
            {
                if (std::abs (v - clip.notes[(size_t) velIndex].velocity) < 1.0e-3f)
                    return;
                main.beginEdit();
                gestureEdited = true;
            }
            clip.notes[(size_t) velIndex].velocity = v;
            main.getEngine().setMidiNotes (*track, clipId, clip.notes);
            repaint();
            return;
        }

        if (mode == Mode::Marquee)
        {
            marqueeRect = juce::Rectangle<int>::leftTopRightBottom (juce::jmin (marqueeStart.x, e.x), juce::jmin (marqueeStart.y, e.y),
                                                                     juce::jmax (marqueeStart.x, e.x), juce::jmax (marqueeStart.y, e.y));
            group = marqueeBase;
            for (int i = 0; i < (int) clip.notes.size(); ++i)
            {
                const int pad = track->kit->padForNote (clip.notes[(size_t) i].pitch);
                if (pad >= 0 && noteRect (clip.notes[(size_t) i], pad).intersects (marqueeRect) && ! isNoteSelected (i))
                    group.push_back (i);
            }
            repaint();
            return;
        }

        if (mode == Mode::Paint || mode == Mode::Erase)
        {
            const auto cell = cellAt (e.getPosition());
            if (cell.x >= 0)
                apply (cell);
            return;
        }

        if (active < 0 || active >= (int) clip.notes.size())
            return;

        const double s = step();
        const bool free = e.mods.isAltDown();
        const double t = xToTime (e.x) - clip.start;   // (seconds from the start of the clip)
        auto onGrid = [&] (double seconds) { return free ? seconds : std::round (seconds / s) * s; };
        const double room = juce::jmax (s, clip.length - original.start);
        double newStart = original.start, newLength = original.length;
        int newPitch = original.pitch;

        if (mode == Mode::Stretch)   // (the step under the pointer is part of the new hit)
            newLength = juce::jmin (room, juce::jmax (s, free ? t - original.start : std::floor ((t - original.start) / s + 1.0) * s));
        else if (mode == Mode::ResizeEnd)
            newLength = juce::jlimit (free ? 0.01 : s, room, onGrid (t) - original.start);
        else if (mode == Mode::ResizeStart)
        {
            const double end = original.start + original.length;
            newStart = juce::jlimit (0.0, end - (free ? 0.01 : s), onGrid (t));
            newLength = end - newStart;
        }
        else   // Move
        {
            newStart = juce::jlimit (0.0, juce::jmax (0.0, clip.length - original.length), onGrid (xToTime (e.x) - grabTime - clip.start));
            const int pad = yToPad (e.y);
            if (pad >= 0)
                newPitch = track->kit->getPad (pad).note;
        }

        auto& n = clip.notes[(size_t) active];
        if (std::abs (newStart - n.start) < 1.0e-9 && std::abs (newLength - n.length) < 1.0e-9 && newPitch == n.pitch)
            return;
        if (group.size() > 1 && ! groupOrig.empty() && (mode == Mode::Move || mode == Mode::ResizeEnd))   // the whole selection follows
        {
            double dStart = newStart - original.start, dLength = newLength - original.length;
            const int dPad = track->kit->padForNote (newPitch) - track->kit->padForNote (original.pitch);
            double minStart = 1.0e18, maxEnd = 0.0;
            int minPad = DrumKit::numPads, maxPad = 0;
            for (auto& g : groupOrig)
            {
                const int pad = track->kit->padForNote (g.second.pitch);
                minStart = juce::jmin (minStart, g.second.start);
                maxEnd = juce::jmax (maxEnd, g.second.start + g.second.length);
                minPad = juce::jmin (minPad, pad);
                maxPad = juce::jmax (maxPad, pad);
            }
            int padShift = juce::jlimit (-minPad, DrumKit::numPads - 1 - maxPad, dPad);
            if (mode == Mode::Move)
            {
                dStart = juce::jlimit (-minStart, juce::jmax (-minStart, clip.length - maxEnd), dStart);
                dLength = 0.0;
            }
            else
            {
                dStart = 0.0;
                padShift = 0;
            }
            if (! gestureEdited)
            {
                main.beginEdit();
                gestureEdited = true;
            }
            moved = true;
            for (auto& g : groupOrig)
            {
                auto& m = clip.notes[(size_t) g.first];
                m.start = g.second.start + dStart;
                m.length = juce::jmax (0.01, g.second.length + dLength);
                const int pad = track->kit->padForNote (g.second.pitch);
                if (pad >= 0)
                    m.pitch = track->kit->getPad (juce::jlimit (0, DrumKit::numPads - 1, pad + padShift)).note;
            }
            main.getEngine().setMidiNotes (*track, clipId, clip.notes);
            repaint();
            return;
        }
        if (std::abs (newStart - original.start) > 1.0e-9 || std::abs (newLength - original.length) > 1.0e-9 || newPitch != original.pitch)
            moved = true;
        if (! gestureEdited)
        {
            main.beginEdit();
            gestureEdited = true;
        }
        if (newPitch != n.pitch)
            main.getEngine().previewNote (*track, newPitch, true, original.velocity);
        n.start = newStart;
        n.length = newLength;
        n.pitch = newPitch;
        main.getEngine().setMidiNotes (*track, clipId, clip.notes);
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (mode == Mode::Marquee)
        {
            marqueeOn = false;
            if (group.size() == 1)
                group.clear();
            repaint();
            mode = Mode::None;
            return;
        }
        // a click (no drag) on a hit of a selected group does not erase it
        if (mode == Mode::Move && ! moved && group.size() > 1 && isNoteSelected (active))
        {
            mode = Mode::None;
            active = -1;
            return;
        }
        // a click on a hit (no drag) erases it, like before
        if (mode == Mode::Move && ! moved && load() && active >= 0 && active < (int) clip.notes.size())
        {
            main.beginEdit();
            clip.notes.erase (clip.notes.begin() + active);
            main.getEngine().setMidiNotes (*track, clipId, clip.notes);
            repaint();
        }
        mode = Mode::None;
        active = -1;
        mouseMove (e);
    }

    // the pointer shows what a drag does: an end of a hit stretches it, the middle moves it
    void mouseMove (const juce::MouseEvent& e) override
    {
        int newHover = -1, newEdge = 0;
        if (load() && track != nullptr && track->kit != nullptr)
        {
            newHover = noteAt (e.getPosition());
            if (newHover >= 0)
                newEdge = edgeAt (noteRect (clip.notes[(size_t) newHover], track->kit->padForNote (clip.notes[(size_t) newHover].pitch)), e.x);
        }
        setMouseCursor (newEdge != 0 ? juce::MouseCursor::LeftRightResizeCursor
                                     : newHover >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        if (newHover != hoverNote)
        {
            hoverNote = newHover;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverNote != -1)
        {
            hoverNote = -1;
            repaint();
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown())
            main.zoomBy (w.deltaY > 0 ? 1.15 : 1.0 / 1.15, e.x);
        else
            Component::mouseWheelMove (e, w);
    }

private:
    enum class Mode { None, Paint, Stretch, Erase, Velocity, Move, ResizeStart, ResizeEnd, Marquee };
    std::vector<int> group;                         // (every selected hit when there are several)
    std::vector<std::pair<int, Note>> groupOrig;    // (those hits as they were when a drag began)
    std::vector<int> marqueeBase;
    juce::Point<int> marqueeStart;
    juce::Rectangle<int> marqueeRect;
    bool marqueeOn = false;

    // the rectangle of a hit (its whole row high), its grab zone at each end, and the hit under a point
    juce::Rectangle<int> noteRect (const Note& n, int pad) const
    {
        return { timeToX (clip.start + n.start) + 1, padToY (pad), juce::jmax (3, (int) std::round (n.length * main.getPixelsPerSecond()) - 1), rowH };
    }
    static int edgeAt (juce::Rectangle<int> r, int x)
    {
        if (r.getWidth() <= 14)   // (a short hit at a small zoom: its right half stretches it, its left half moves it)
            return x >= r.getX() + r.getWidth() / 2 ? 1 : 0;
        const int w = juce::jlimit (4, 8, r.getWidth() / 3);
        return x >= r.getRight() - w ? 1 : x <= r.getX() + w ? -1 : 0;
    }
    int noteAt (juce::Point<int> p) const
    {
        for (int i = (int) clip.notes.size() - 1; i >= 0; --i)
        {
            const int pad = track->kit->padForNote (clip.notes[(size_t) i].pitch);
            if (pad >= 0 && noteRect (clip.notes[(size_t) i], pad).contains (p))
                return i;
        }
        return -1;
    }

    bool load() { return track != nullptr && main.getEngine().getMidiClip (*track, clipId, clip); }

    double xToTime (int x) const { return (x + main.getScrollX()) / main.getPixelsPerSecond(); }
    int timeToX (double t) const { return (int) std::round (t * main.getPixelsPerSecond()) - main.getScrollX(); }
    double step() const { return stepSeconds(); }
    double stepSeconds() const { return main.getTempoMap().beatSecondsAt (clip.start) / 4.0; }   // a sixteenth at the clip's start

    // (step index, pad) under a point, or (-1, -1) outside the clip
    juce::Point<int> cellAt (juce::Point<int> p) const
    {
        const double t = xToTime (p.x) - clip.start;
        if (t < 0.0 || t >= clip.length)
            return { -1, -1 };
        return { (int) std::floor (t / step() + 1.0e-9), yToPad (p.y) };
    }

    int findNote (int stepIndex, int pad) const
    {
        const int pitch = track->kit->getPad (pad).note;
        const double start = stepIndex * step();
        for (int i = 0; i < (int) clip.notes.size(); ++i)
            if (clip.notes[(size_t) i].pitch == pitch && std::abs (clip.notes[(size_t) i].start - start) < step() * 0.5)
                return i;
        return -1;
    }

    void apply (juce::Point<int> cell)
    {
        if (cell == lastCell)
            return;
        lastCell = cell;

        const int existing = findNote (cell.x, cell.y);
        const bool adding = mode == Mode::Paint || mode == Mode::Stretch;
        if (adding && existing >= 0) return;
        if (mode == Mode::Erase && existing < 0) return;

        if (! gestureEdited)
        {
            main.beginEdit();
            gestureEdited = true;
        }

        const int pitch = track->kit->getPad (cell.y).note;
        if (adding)
        {
            Note n;
            n.pitch = pitch;
            n.start = cell.x * step();
            n.length = step();
            n.velocity = 0.85f;
            clip.notes.push_back (n);
            main.getEngine().previewNote (*track, pitch, true, 0.85f);
        }
        else
        {
            clip.notes.erase (clip.notes.begin() + existing);
        }
        main.getEngine().setMidiNotes (*track, clipId, clip.notes);
        repaint();
    }

    DrumSequencer& owner;
    MainComponent& main;
    Track* track = nullptr;
    int clipId = -1;
    MidiClip clip;
    Mode mode = Mode::None;
    bool gestureEdited = false;
    int velIndex = -1, velStartY = 0;
    float velStartValue = 0.8f;
    juce::Point<int> lastCell { -1, -1 };
    int active = -1, hoverNote = -1;   // (the hit being dragged, the hit under the mouse)
    bool moved = false;
    Note original;                     // (the hit as it was when the drag began)
    double grabTime = 0.0;
};

//==============================================================================
DrumSequencer::DrumSequencer (MainComponent& m) : main (m)
{
    pads = std::make_unique<Pads> (*this);
    grid = std::make_unique<Grid> (*this);
    addAndMakeVisible (*pads);
    addAndMakeVisible (*grid);

    title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    addAndMakeVisible (title);

    clearButton.onClick = [this] { grid->clearPattern(); };
    addAndMakeVisible (clearButton);
    closeButton.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeButton);
}

DrumSequencer::~DrumSequencer() = default;

void DrumSequencer::showClip (Track* t, int id)
{
    track = t;
    clipId = id;
    grid->setClip (t, id);
    if (t != nullptr)
        title.setText (t->name + "  -  drum pattern  (16th-note steps).  Drag the ends of a hit to stretch it, Ctrl+drag selects several", juce::dontSendNotification);
    pads->repaint();
    repaint();
}

bool DrumSequencer::stillValid() const
{
    return track != nullptr && main.getEngine().indexOfTrack (track) >= 0 && main.getEngine().isMidiClip (*track, clipId);
}

void DrumSequencer::repaintGrid() { grid->repaint(); }
bool DrumSequencer::deleteSelectedNote() { return grid->deleteGroup(); }
bool DrumSequencer::copyNotes (bool cut) { return grid->copyGroup (cut); }
bool DrumSequencer::pasteNotes (double atSeconds) { return grid->pasteGroup (atSeconds); }
bool DrumSequencer::duplicateNotes() { return grid->duplicateGroup(); }
bool DrumSequencer::selectAllNotes() { return grid->selectEverything(); }

void DrumSequencer::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
    g.setColour (Theme::panel);
    g.fillRect (0, 0, getWidth(), toolbarHeight);
    g.setColour (Theme::border);
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());
    g.drawHorizontalLine (toolbarHeight - 1, 0.0f, (float) getWidth());
}

void DrumSequencer::resized()
{
    auto bar = getLocalBounds().removeFromTop (toolbarHeight).reduced (8, 5);
    closeButton.setBounds (bar.removeFromRight (26));
    bar.removeFromRight (8);
    clearButton.setBounds (bar.removeFromRight (110));
    title.setBounds (bar);

    const int h = DrumKit::numPads * rowH;
    pads->setBounds (0, toolbarHeight, MainComponent::controlsWidth, h);
    grid->setBounds (MainComponent::controlsWidth, toolbarHeight, juce::jmax (0, getWidth() - MainComponent::controlsWidth), h);
}
