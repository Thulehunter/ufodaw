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
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (info.name, r.withTrimmedLeft (16).withWidth (110), juce::Justification::centredLeft);

            g.setColour (Theme::text.withAlpha (0.45f));
            g.setFont (11.0f);
            g.drawText (info.file == juce::File() ? juce::String ("built-in sound") : info.file.getFileName(),
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
            menu.showMenuAsync ({}, [this, pad] (int result)
            {
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

        // hits
        for (auto& n : clip.notes)
        {
            const int pad = track->kit->padForNote (n.pitch);
            if (pad < 0)
                continue;
            const int x = timeToX (clip.start + n.start);
            const int w = juce::jmax (3, (int) std::round (step * pps) - 1);
            // a hit's height shows its velocity (Ctrl+drag a hit up or down to change it)
            const int full = rowH - 4;
            const int h = juce::jmax (4, (int) std::round ((float) full * (0.2f + 0.8f * n.velocity)));
            const juce::Rectangle<int> r (x + 1, padToY (pad) + 2 + (full - h), w - 1, h);
            if (! r.intersects (area))
                continue;
            g.setColour (padColour (pad).withMultipliedBrightness (0.7f + 0.4f * n.velocity));
            g.fillRoundedRectangle (r.toFloat(), 3.0f);
        }

        // dim everything outside the clip
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRect (0, 0, juce::jmax (0, timeToX (clip.start)), getHeight());
        const int endX = timeToX (clip.end());
        g.fillRect (endX, 0, getWidth() - endX, getHeight());

        g.setColour (Theme::playhead);
        g.drawVerticalLine (timeToX (main.getEngine().getPositionSeconds()), 0.0f, (float) getHeight());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        main.grabKeyboardFocus();
        if (! load() || track->kit == nullptr)
            return;

        gestureEdited = false;
        lastCell = { -1, -1 };

        const auto cell = cellAt (e.getPosition());
        if (cell.x < 0)
            return;

        const int existingIndex = findNote (cell.x, cell.y);
        const bool exists = existingIndex >= 0;

        if (e.mods.isCtrlDown() && exists)   // velocity: drag up for harder, down for softer
        {
            mode = Mode::Velocity;
            velIndex = existingIndex;
            velStartY = e.y;
            velStartValue = clip.notes[(size_t) existingIndex].velocity;
            return;
        }

        if (e.mods.isPopupMenu())
            mode = Mode::Erase;
        else
            mode = exists ? Mode::Erase : Mode::Add;
        apply (cell);
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

        const auto cell = cellAt (e.getPosition());
        if (cell.x >= 0)
            apply (cell);
    }

    void mouseUp (const juce::MouseEvent&) override { mode = Mode::None; }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown())
            main.zoomBy (w.deltaY > 0 ? 1.15 : 1.0 / 1.15, e.x);
        else
            Component::mouseWheelMove (e, w);
    }

private:
    enum class Mode { None, Add, Erase, Velocity };

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
        if (mode == Mode::Add && existing >= 0) return;
        if (mode == Mode::Erase && existing < 0) return;

        if (! gestureEdited)
        {
            main.beginEdit();
            gestureEdited = true;
        }

        const int pitch = track->kit->getPad (cell.y).note;
        if (mode == Mode::Add)
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
        title.setText (t->name + "  -  drum pattern  (16th-note steps)", juce::dontSendNotification);
    pads->repaint();
    repaint();
}

bool DrumSequencer::stillValid() const
{
    return track != nullptr && main.getEngine().indexOfTrack (track) >= 0 && main.getEngine().isMidiClip (*track, clipId);
}

void DrumSequencer::repaintGrid() { grid->repaint(); }

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
