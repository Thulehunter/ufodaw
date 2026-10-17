#include "MainComponent.h"
#include <algorithm>
#include <map>

//==============================================================================
// Left-hand controls for one track: name, volume, pan, mute, solo, level meter.
class TrackHeader : public juce::Component
{
public:
    TrackHeader (Track& t, std::function<void (Track*)> onRemove) : track (t), remove (std::move (onRemove))
    {
        nameLabel.setText (track.name, juce::dontSendNotification);
        nameLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        addAndMakeVisible (nameLabel);

        volume.setSliderStyle (juce::Slider::LinearHorizontal);
        volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volume.setRange (0.0, 1.5, 0.001);
        volume.setValue (track.gain, juce::dontSendNotification);
        volume.onValueChange = [this] { track.gain = (float) volume.getValue(); };
        addAndMakeVisible (volume);

        pan.setSliderStyle (juce::Slider::LinearHorizontal);
        pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        pan.setRange (-1.0, 1.0, 0.01);
        pan.setValue (track.pan, juce::dontSendNotification);
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.onValueChange = [this] { track.pan = (float) pan.getValue(); };
        addAndMakeVisible (pan);

        muteButton.setClickingTogglesState (true);
        muteButton.setToggleState (track.mute, juce::dontSendNotification);
        muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orangered);
        muteButton.onClick = [this] { track.mute = muteButton.getToggleState(); };
        addAndMakeVisible (muteButton);

        soloButton.setClickingTogglesState (true);
        soloButton.setToggleState (track.solo, juce::dontSendNotification);
        soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::gold);
        soloButton.onClick = [this] { track.solo = soloButton.getToggleState(); };
        addAndMakeVisible (soloButton);

        deleteButton.onClick = [this] { remove (&track); };
        addAndMakeVisible (deleteButton);
    }

    Track& getTrack() { return track; }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff24272e));

        auto m = juce::Rectangle<int> (getWidth() - 8, 6, 4, getHeight() - 12);
        g.setColour (juce::Colours::black);
        g.fillRect (m);
        const float lvl = juce::jlimit (0.0f, 1.0f, shown);
        g.setColour (lvl > 0.9f ? juce::Colours::red : juce::Colours::limegreen);
        g.fillRect (m.withTop (m.getBottom() - (int) (lvl * (float) m.getHeight())));

        g.setColour (juce::Colour (0xff0b0c0e));
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    void resized() override
    {
        auto c = getLocalBounds().withTrimmedRight (14).reduced (8, 6);
        auto top = c.removeFromTop (20);
        deleteButton.setBounds (top.removeFromRight (22));
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
        repaint();
    }

private:
    Track& track;
    std::function<void (Track*)> remove;
    float shown = 0.0f;

    juce::Label nameLabel;
    juce::Slider volume, pan;
    juce::TextButton muteButton { "M" }, soloButton { "S" }, deleteButton { "x" };
};

//==============================================================================
// Bar/beat ruler above the lanes. Click or drag to move the playhead.
class TimeRuler : public juce::Component
{
public:
    explicit TimeRuler (MainComponent& m) : main (m) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff24272e));
        const double pps = main.getPixelsPerSecond();
        const double bar = main.barSeconds(), beat = main.beatSeconds();
        const double scroll = main.getScrollX();

        const int labelEvery = juce::jmax (1, (int) std::ceil (60.0 / (bar * pps)));
        const int firstBar = (int) std::floor (scroll / (bar * pps));

        g.setFont (11.0f);
        for (int b = firstBar;; ++b)
        {
            const int x = (int) (b * bar * pps - scroll);
            if (x > getWidth()) break;

            g.setColour (juce::Colours::white.withAlpha (0.4f));
            g.drawVerticalLine (x, (float) getHeight() * 0.45f, (float) getHeight());
            if (b % labelEvery == 0)
            {
                g.setColour (juce::Colours::white.withAlpha (0.75f));
                g.drawText (juce::String (b + 1), x + 3, 1, 50, 14, juce::Justification::left);
            }

            if (beat * pps >= 14.0)
                for (int k = 1; k < 4; ++k)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.2f));
                    g.drawVerticalLine (x + (int) (k * beat * pps), (float) getHeight() * 0.7f, (float) getHeight());
                }
        }
        g.setColour (juce::Colour (0xff0b0c0e));
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    void mouseDown (const juce::MouseEvent& e) override { move (e); }
    void mouseDrag (const juce::MouseEvent& e) override { move (e); }

private:
    void move (const juce::MouseEvent& e)
    {
        main.setPlayhead ((e.x + main.getScrollX()) / main.getPixelsPerSecond());
    }

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

        g.fillAll (juce::Colour (0xff15171b));

        // grid
        const double bar = main.barSeconds(), beat = main.beatSeconds();
        const int firstBar = juce::jmax (0, (int) std::floor (area.getX() / (bar * pps)));
        for (int b = firstBar;; ++b)
        {
            const int x = (int) (b * bar * pps);
            if (x > area.getRight()) break;
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.drawVerticalLine (x, 0.0f, (float) getHeight());
            if (beat * pps >= 14.0)
            {
                g.setColour (juce::Colours::white.withAlpha (0.04f));
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
            g.setColour (sel ? juce::Colour (0xff3f7fb5) : juce::Colour (0xff2f5d8a));
            g.fillRoundedRectangle (r.toFloat(), 4.0f);

            const auto& peaks = peaksFor (c, pps);
            const float mid = (float) r.getCentreY();
            const float half = (float) r.getHeight() * 0.5f - 3.0f;
            g.setColour (juce::Colour (0xffbfe0ff));
            for (int x = juce::jmax (r.getX(), area.getX()); x < juce::jmin (r.getRight(), area.getRight()); ++x)
            {
                const int idx = x - r.getX();
                if (idx >= 0 && idx < (int) peaks.size())
                    g.drawVerticalLine (x, mid - peaks[(size_t) idx] * half, mid + peaks[(size_t) idx] * half);
            }

            g.setColour (sel ? juce::Colours::white : juce::Colours::white.withAlpha (0.35f));
            g.drawRoundedRectangle (r.toFloat(), 4.0f, sel ? 2.0f : 1.0f);

            g.saveState();
            g.reduceClipRegion (r.reduced (2));
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.setFont (11.0f);
            g.drawText (c.sourceFile.getFileNameWithoutExtension(), r.getX() + 5, r.getY() + 2, 400, 13, juce::Justification::left);
            g.restoreState();
        }

        g.setColour (juce::Colour (0xff0b0c0e));
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        main.grabKeyboardFocus();
        const double pps = main.getPixelsPerSecond();
        const double t = e.x / pps;

        dragId = -1;
        const auto clips = main.getEngine().getClips (track);
        for (auto it = clips.rbegin(); it != clips.rend(); ++it)   // topmost first
            if (clipRect (*it, pps).contains (e.getPosition()))
            {
                dragId = it->id;
                grabOffset = t - it->start;
                main.selectClip (&track, dragId);
                return;
            }

        main.selectClip (nullptr, -1);
        main.setPlayhead (t);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const double t = juce::jmax (0.0, e.x / main.getPixelsPerSecond());
        if (dragId < 0)
        {
            main.setPlayhead (t);   // scrub
            return;
        }

        double newStart = t - grabOffset;
        if (! e.mods.isAltDown())
            newStart = main.snapTime (newStart);
        main.getEngine().moveClip (track, dragId, newStart);
        main.clipsChanged();
    }

    void mouseUp (const juce::MouseEvent&) override { dragId = -1; }

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

    static juce::Rectangle<int> clipRect (const Clip& c, double pps)
    {
        return { (int) std::round (c.start * pps), 3, juce::jmax (2, (int) std::round (c.length * pps)), MainComponent::stripHeight - 7 };
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
    int dragId = -1;
    double grabOffset = 0.0;
};

//==============================================================================
MainComponent::MainComponent()
{
    ruler = std::make_unique<TimeRuler> (*this);   // must exist before setSize() triggers resized()

    setWantsKeyboardFocus (true);

    deviceManager.initialiseWithDefaultDevices (2, 2);
    player.setSource (&engine);
    deviceManager.addAudioCallback (&player);

    for (juce::Component* b : std::initializer_list<juce::Component*> {
             &rewindButton, &playButton, &pauseButton, &stopButton, &recordButton, &addButton,
             &openButton, &saveButton, &saveAsButton, &exportButton, &zoomOutButton, &zoomInButton })
        addAndMakeVisible (b);
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
    bpmSlider.onValueChange = [this] { engine.bpm = (float) bpmSlider.getValue(); clipsChanged(); };

    masterSlider.setRange (0.0, 1.2, 0.001);
    masterSlider.setValue (engine.masterGain, juce::dontSendNotification);
    masterSlider.onValueChange = [this] { engine.masterGain = (float) masterSlider.getValue(); };

    timeLabel.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 24.0f, juce::Font::plain));
    timeLabel.setJustificationType (juce::Justification::centred);

    laneViewport.setViewedComponent (&laneHolder, false);
    laneViewport.setScrollBarsShown (true, true);
    laneViewport.onScroll = [this]
    {
        headerViewport.setViewPosition (0, laneViewport.getViewPositionY());
        ruler->repaint();
        repaint();
    };
    addAndMakeVisible (laneViewport);

    headerViewport.setViewedComponent (&headerHolder, false);
    headerViewport.setScrollBarsShown (false, false);
    addAndMakeVisible (headerViewport);
    addAndMakeVisible (*ruler);

    setSize (1250, 620);   // last: resized() touches all the children above
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
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
    repaint();
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
    repaint();
}

void MainComponent::clipsChanged()
{
    layoutContent();
    ruler->repaint();
    repaint();
}

//==============================================================================
void MainComponent::addTrackUI (Track& t)
{
    auto* header = headers.add (new TrackHeader (t, [this] (Track* tr)
    {
        // Deferred: this runs inside a button owned by the header being deleted.
        juce::MessageManager::callAsync ([this, tr] { removeTrackUI (tr); });
    }));
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
            if (selTrack == t)
                selectClip (nullptr, -1);
            headers.remove (i);
            lanes.remove (i);
            engine.removeTrack (t);
            layoutContent();
            return;
        }
}

void MainComponent::clearTracksUI()
{
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
    laneViewport.setBounds (controlsWidth, y0, laneW, getHeight() - y0 - bottomBarHeight);
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
    auto top = getLocalBounds().removeFromTop (topBarHeight).reduced (8, 10);
    addButton.setBounds (top.removeFromLeft (80));
    top.removeFromLeft (16);
    timeLabel.setBounds (top.removeFromLeft (170));
    top.removeFromLeft (16);
    bpmLabel.setBounds (top.removeFromLeft (36));
    bpmSlider.setBounds (top.removeFromLeft (70));
    top.removeFromLeft (10);
    metroButton.setBounds (top.removeFromLeft (110));
    top.removeFromLeft (16);
    openButton.setBounds (top.removeFromLeft (56));
    top.removeFromLeft (4);
    saveButton.setBounds (top.removeFromLeft (56));
    top.removeFromLeft (4);
    saveAsButton.setBounds (top.removeFromLeft (64));
    top.removeFromLeft (4);
    exportButton.setBounds (top.removeFromLeft (90));
    masterSlider.setBounds (top.removeFromRight (150));

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
    g.fillAll (juce::Colour (0xff1b1d22));

    g.setColour (juce::Colour (0xff24272e));
    g.fillRect (0, getHeight() - bottomBarHeight, getWidth(), bottomBarHeight);
    g.setColour (juce::Colour (0xff0b0c0e));
    g.drawHorizontalLine (getHeight() - bottomBarHeight, 0.0f, (float) getWidth());

    auto m = juce::Rectangle<int> (getWidth() - 158, 8, 150, 4);
    g.setColour (juce::Colours::black);
    g.fillRect (m);
    g.setColour (masterShown > 0.95f ? juce::Colours::red : juce::Colours::limegreen);
    g.fillRect (m.withWidth ((int) (juce::jlimit (0.0f, 1.0f, masterShown) * (float) m.getWidth())));
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    const int x = laneViewport.getX() + (int) (engine.getPositionSeconds() * pixelsPerSecond) - getScrollX();
    if (x >= laneViewport.getX() && x < laneViewport.getRight())
    {
        g.setColour (juce::Colours::orange);
        g.drawVerticalLine (x, (float) ruler->getY(), (float) laneViewport.getBottom());
    }
}

//==============================================================================
void MainComponent::splitAtPlayhead()
{
    const double t = engine.getPositionSeconds();

    if (selTrack != nullptr && selClip >= 0)
    {
        engine.splitClip (*selTrack, selClip, t);
    }
    else
    {
        for (int i = 0; i < engine.getNumTracks(); ++i)
            if (auto* tr = engine.getTrack (i))
                for (auto& c : engine.getClips (*tr))
                    engine.splitClip (*tr, c.id, t);
    }
    clipsChanged();
}

void MainComponent::deleteSelected()
{
    if (selTrack != nullptr && selClip >= 0)
    {
        engine.deleteClip (*selTrack, selClip);
        selectClip (nullptr, -1);
        clipsChanged();
    }
}

void MainComponent::duplicateSelected()
{
    if (selTrack != nullptr && selClip >= 0)
    {
        const int id = engine.duplicateClip (*selTrack, selClip);
        if (id >= 0)
            selectClip (selTrack, id);
        clipsChanged();
    }
}

bool MainComponent::keyPressed (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    const auto ch = juce::CharacterFunctions::toLowerCase (k.getTextCharacter());

    if (mods.isCommandDown())
    {
        if (ch == 's' || k.getKeyCode() == 'S') { saveButton.triggerClick(); return true; }
        if (ch == 'o' || k.getKeyCode() == 'O') { openButton.triggerClick(); return true; }
        if (k.getKeyCode() == 'D')              { duplicateSelected(); return true; }
        return false;
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

    const bool recording = engine.isRecording();
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
    if (engine.isRecording())
    {
        finishRecording();   // keeps playing, like most DAWs
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

// Closes the recording file and puts it on a new track at the position where recording began.
void MainComponent::finishRecording()
{
    if (! engine.isRecording())
        return;

    engine.stopRecording();

    auto* t = engine.addTrack (recordingFile.getFileNameWithoutExtension());
    if (engine.addClipFromFile (*t, recordingFile, recStartSeconds) < 0)
    {
        engine.removeTrack (t);
        recordingFile.deleteFile();
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
    const bool intoTrack = target != nullptr && paths.size() == 1;

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
        o->setProperty ("gain", (double) t->gain.load());
        o->setProperty ("pan", (double) t->pan.load());
        o->setProperty ("mute", t->mute.load());
        o->setProperty ("solo", t->solo.load());
        o->setProperty ("clips", clipList);
        trackList.add (juce::var (o));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 2);
    root->setProperty ("bpm", (double) engine.bpm.load());
    root->setProperty ("masterGain", (double) engine.masterGain.load());
    root->setProperty ("metronome", engine.metronome.load());
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

    engine.bpm = (float) (double) root.getProperty ("bpm", 120.0);
    engine.masterGain = (float) (double) root.getProperty ("masterGain", 0.9);
    engine.metronome = (bool) root.getProperty ("metronome", false);
    bpmSlider.setValue (engine.bpm, juce::dontSendNotification);
    masterSlider.setValue (engine.masterGain, juce::dontSendNotification);
    metroButton.setToggleState (engine.metronome, juce::dontSendNotification);

    juce::StringArray missing;
    const auto baseDir = file.getParentDirectory();

    for (auto& item : *root["tracks"].getArray())
    {
        auto* t = engine.addTrack (item.getProperty ("name", "Track").toString());
        t->gain = (float) (double) item.getProperty ("gain", 0.8);
        t->pan = (float) (double) item.getProperty ("pan", 0.0);
        t->mute = (bool) item.getProperty ("mute", false);
        t->solo = (bool) item.getProperty ("solo", false);

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

        if (auto* clipArray = item["clips"].getArray())
            for (auto& cv : *clipArray)
                loadClip (cv);
        else if (item.hasProperty ("file"))   // version 1: one clip per track
            loadClip (item);

        addTrackUI (*t);
    }

    currentProject = file;
    if (auto* w = getTopLevelComponent())
        w->setName ("MiniDAW - " + file.getFileNameWithoutExtension());
    clipsChanged();

    if (missing.size() > 0)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Some audio files were not found",
                                                missing.joinIntoString ("\n"));
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
                              const bool ok = engine.renderToFile (f, engine.getTotalLengthSeconds() + 0.5, false);
                              juce::AlertWindow::showMessageBoxAsync (ok ? juce::MessageBoxIconType::InfoIcon
                                                                         : juce::MessageBoxIconType::WarningIcon,
                                                                      ok ? "Export finished" : "Export failed",
                                                                      f.getFullPathName());
                          });
}
