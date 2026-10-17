#include "MainComponent.h"

//==============================================================================
class TrackStrip : public juce::Component
{
public:
    TrackStrip (Track& t, std::function<void (TrackStrip*)> onRemove) : track (t), remove (std::move (onRemove))
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
        muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orangered);
        muteButton.onClick = [this] { track.mute = muteButton.getToggleState(); };
        addAndMakeVisible (muteButton);

        soloButton.setClickingTogglesState (true);
        soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::gold);
        soloButton.onClick = [this] { track.solo = soloButton.getToggleState(); };
        addAndMakeVisible (soloButton);

        deleteButton.onClick = [this] { remove (this); };
        addAndMakeVisible (deleteButton);

        buildPeaks();
    }

    Track& getTrack() { return track; }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds();
        g.setColour (juce::Colour (0xff24272e));
        g.fillRect (b.removeFromLeft (MainComponent::controlsWidth));
        g.setColour (juce::Colour (0xff15171b));
        g.fillRect (b);

        // level meter
        auto m = juce::Rectangle<int> (MainComponent::controlsWidth - 8, 6, 4, getHeight() - 12);
        g.setColour (juce::Colours::black);
        g.fillRect (m);
        const float lvl = juce::jlimit (0.0f, 1.0f, shown);
        g.setColour (lvl > 0.9f ? juce::Colours::red : juce::Colours::limegreen);
        g.fillRect (m.withTop (m.getBottom() - (int) (lvl * (float) m.getHeight())));

        // clip with waveform
        const int x0 = MainComponent::controlsWidth + (int) (track.startSeconds * MainComponent::pixelsPerSecond);
        const int w = (int) peaks.size();
        auto clip = juce::Rectangle<int> (x0, 4, w, getHeight() - 8);
        g.setColour (juce::Colour (0xff2f5d8a));
        g.fillRoundedRectangle (clip.toFloat(), 4.0f);
        g.setColour (juce::Colour (0xffbfe0ff));
        const float mid = (float) clip.getCentreY();
        const float half = (float) clip.getHeight() * 0.5f - 3.0f;
        for (int i = 0; i < w; ++i)
            g.drawVerticalLine (x0 + i, mid - peaks[(size_t) i] * half, mid + peaks[(size_t) i] * half);
        g.drawRoundedRectangle (clip.toFloat(), 4.0f, 1.0f);

        g.setColour (juce::Colour (0xff0b0c0e));
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    void resized() override
    {
        auto c = getLocalBounds().removeFromLeft (MainComponent::controlsWidth - 14).reduced (8, 6);
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
        repaint (0, 0, MainComponent::controlsWidth, getHeight());
    }

private:
    void buildPeaks()
    {
        const int w = juce::jmax (1, (int) (track.lengthSeconds() * MainComponent::pixelsPerSecond));
        peaks.assign ((size_t) w, 0.0f);
        const int n = track.audio.getNumSamples();
        const double spp = (double) n / w;
        for (int ch = 0; ch < track.audio.getNumChannels(); ++ch)
        {
            auto* d = track.audio.getReadPointer (ch);
            for (int i = 0; i < w; ++i)
            {
                const int a = (int) (i * spp), b = juce::jmin (n, (int) ((i + 1) * spp) + 1);
                float p = 0.0f;
                for (int s = a; s < b; ++s)
                    p = juce::jmax (p, std::abs (d[s]));
                peaks[(size_t) i] = juce::jmax (peaks[(size_t) i], p);
            }
        }
    }

    Track& track;
    std::function<void (TrackStrip*)> remove;
    std::vector<float> peaks;
    float shown = 0.0f;

    juce::Label nameLabel;
    juce::Slider volume, pan;
    juce::TextButton muteButton { "M" }, soloButton { "S" }, deleteButton { "x" };
};

//==============================================================================
MainComponent::MainComponent()
{
    setSize (1100, 560);
    setWantsKeyboardFocus (true);

    deviceManager.initialiseWithDefaultDevices (0, 2);
    player.setSource (&engine);
    deviceManager.addAudioCallback (&player);

    for (auto* b : { &rewindButton, &playButton, &addButton })
        addAndMakeVisible (b);
    addAndMakeVisible (metroButton);
    addAndMakeVisible (bpmLabel);
    addAndMakeVisible (bpmSlider);
    addAndMakeVisible (masterSlider);
    addAndMakeVisible (timeLabel);

    rewindButton.onClick = [this] { engine.setPositionSeconds (0.0); };
    playButton.onClick = [this]
    {
        if (engine.isPlaying()) engine.stop(); else engine.play();
        playButton.setButtonText (engine.isPlaying() ? "Pause" : "Play");
    };
    addButton.onClick = [this] { addTrackDialog(); };
    metroButton.onClick = [this] { engine.metronome = metroButton.getToggleState(); };

    bpmSlider.setRange (40.0, 240.0, 1.0);
    bpmSlider.setValue (120.0, juce::dontSendNotification);
    bpmSlider.onValueChange = [this] { engine.bpm = (float) bpmSlider.getValue(); };

    masterSlider.setRange (0.0, 1.2, 0.001);
    masterSlider.setValue (engine.masterGain, juce::dontSendNotification);
    masterSlider.onValueChange = [this] { engine.masterGain = (float) masterSlider.getValue(); };

    timeLabel.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 24.0f, juce::Font::plain));
    timeLabel.setJustificationType (juce::Justification::centred);

    viewport.setViewedComponent (&stripHolder, false);
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);

    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    deviceManager.removeAudioCallback (&player);
    player.setSource (nullptr);
}

void MainComponent::addTrackDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose audio files", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems
                              | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              for (auto& f : fc.getResults())
                                  addTrack (f);
                          });
}

void MainComponent::addTrack (const juce::File& file)
{
    if (auto* t = engine.addTrackFromFile (file))
    {
        auto* strip = strips.add (new TrackStrip (*t, [this] (TrackStrip* s) { removeStrip (s); }));
        stripHolder.addAndMakeVisible (strip);
        layoutStrips();
    }
    else
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Cannot open file", file.getFullPathName());
    }
}

void MainComponent::removeStrip (TrackStrip* s)
{
    // Deferred: called from a button inside the strip that is about to be destroyed.
    juce::MessageManager::callAsync ([this, s]
    {
        if (strips.contains (s))
        {
            engine.removeTrack (&s->getTrack());
            strips.removeObject (s);
            layoutStrips();
        }
    });
}

void MainComponent::layoutStrips()
{
    int w = viewport.getMaximumVisibleWidth();
    for (auto* s : strips)
        w = juce::jmax (w, controlsWidth + (int) ((s->getTrack().startSeconds + s->getTrack().lengthSeconds()) * pixelsPerSecond) + 40);

    int y = 0;
    for (auto* s : strips)
    {
        s->setBounds (0, y, w, stripHeight);
        y += stripHeight;
    }
    stripHolder.setSize (w, juce::jmax (y, viewport.getMaximumVisibleHeight()));
}

juce::Rectangle<int> MainComponent::laneArea() const
{
    return { controlsWidth, topBarHeight, getWidth() - controlsWidth, getHeight() - topBarHeight };
}

void MainComponent::resized()
{
    auto top = getLocalBounds().removeFromTop (topBarHeight).reduced (8, 10);
    rewindButton.setBounds (top.removeFromLeft (40));
    top.removeFromLeft (6);
    playButton.setBounds (top.removeFromLeft (70));
    top.removeFromLeft (6);
    addButton.setBounds (top.removeFromLeft (80));
    top.removeFromLeft (16);
    timeLabel.setBounds (top.removeFromLeft (170));
    top.removeFromLeft (16);
    bpmLabel.setBounds (top.removeFromLeft (36));
    bpmSlider.setBounds (top.removeFromLeft (70));
    top.removeFromLeft (10);
    metroButton.setBounds (top.removeFromLeft (110));
    masterSlider.setBounds (top.removeFromRight (150));

    viewport.setBounds (0, topBarHeight, getWidth(), getHeight() - topBarHeight);
    layoutStrips();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1b1d22));

    // master meter above the master slider
    auto m = juce::Rectangle<int> (getWidth() - 158, 8, 150, 4);
    g.setColour (juce::Colours::black);
    g.fillRect (m);
    g.setColour (masterShown > 0.95f ? juce::Colours::red : juce::Colours::limegreen);
    g.fillRect (m.withWidth ((int) (juce::jlimit (0.0f, 1.0f, masterShown) * (float) m.getWidth())));
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    // second markers and playhead, clipped to the lane area
    const int scrollX = viewport.getViewPositionX();
    g.saveState();
    g.reduceClipRegion (laneArea());
    g.setFont (11.0f);
    for (int sec = 0; ; ++sec)
    {
        const int x = controlsWidth + (int) (sec * pixelsPerSecond) - scrollX;
        if (x > getWidth()) break;
        if (x < controlsWidth) continue;
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawVerticalLine (x, (float) topBarHeight, (float) getHeight());
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.drawText (juce::String (sec) + "s", x + 3, topBarHeight + 1, 40, 12, juce::Justification::left);
    }
    const int px = controlsWidth + (int) (engine.getPositionSeconds() * pixelsPerSecond) - scrollX;
    g.setColour (juce::Colours::orange);
    g.drawVerticalLine (px, (float) topBarHeight, (float) getHeight());
    g.restoreState();
}

void MainComponent::mouseDown (const juce::MouseEvent& e)
{
    // Clicking empty lane space moves the playhead
    if (laneArea().contains (e.getPosition()))
        engine.setPositionSeconds ((e.x - controlsWidth + viewport.getViewPositionX()) / pixelsPerSecond);
}

bool MainComponent::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::spaceKey)  { playButton.triggerClick(); return true; }
    if (k == juce::KeyPress::returnKey) { rewindButton.triggerClick(); return true; }
    return false;
}

void MainComponent::timerCallback()
{
    const double p = engine.getPositionSeconds();
    const int mins = (int) (p / 60.0);
    timeLabel.setText (juce::String::formatted ("%02d:%06.3f", mins, p - mins * 60.0), juce::dontSendNotification);

    const float ml = engine.masterLevel;
    masterShown = ml > masterShown ? ml : masterShown * 0.85f;

    for (auto* s : strips)
        s->tick();

    repaint();
}
