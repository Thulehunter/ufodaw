#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "Theme.h"
#include "Tuner.h"

// The sample with its start / end markers (yellow), loop markers (orange) and slices; drag a marker to move it. Drop an audio
// file on it to load that sample.
class SamplerWaveform : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    SamplerWaveform (Sampler& s, std::function<void()> onChanged, std::function<void (const juce::File&)> onFile)
        : sampler (s), changed (std::move (onChanged)), fileDropped (std::move (onFile)) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::laneBg);
        const auto sample = sampler.getSample();
        if (sample == nullptr)
        {
            g.setColour (Theme::textFaint);
            g.setFont (juce::FontOptions (15.0f));
            g.drawText ("Drop an audio file here, or press Load...", getLocalBounds(), juce::Justification::centred);
            return;
        }

        // the waveform, one column per pixel
        const int w = getWidth(), h = getHeight(), total = sample->getNumSamples();
        if (sample.get() != peaksOf || (int) peaks.size() != w)
        {
            peaksOf = sample.get();
            peaks.assign ((size_t) juce::jmax (1, w), 0.0f);
            for (int ch = 0; ch < sample->getNumChannels(); ++ch)
            {
                const auto* d = sample->getReadPointer (ch);
                for (int x = 0; x < w; ++x)
                {
                    const int a = (int) ((double) x / w * total), b = juce::jmin (total, (int) ((double) (x + 1) / w * total) + 1);
                    float m = 0.0f;
                    for (int i = a; i < b; ++i)
                        m = juce::jmax (m, std::abs (d[i]));
                    peaks[(size_t) x] = juce::jmax (peaks[(size_t) x], m);
                }
            }
        }

        const float mid = (float) h * 0.5f;
        g.setColour (Theme::audioWave);
        for (int x = 0; x < w; ++x)
            g.drawVerticalLine (x, mid - peaks[(size_t) x] * mid * 0.92f, mid + peaks[(size_t) x] * mid * 0.92f);

        const int sx = xOf (sampler.params.start), ex = xOf (sampler.params.end);

        // what lies outside the region is dimmed
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRect (0, 0, juce::jmax (0, juce::jmin (sx, ex)), h);
        g.fillRect (juce::jmax (sx, ex), 0, w - juce::jmax (sx, ex), h);

        const int mode = sampler.params.mode.load();
        if (mode == Sampler::Slice)
        {
            const int count = juce::jlimit (1, 64, sampler.params.slices.load());
            g.setFont (juce::FontOptions (11.0f));
            for (int i = 0; i < count; ++i)
            {
                const int x = sx + (int) ((double) (ex - sx) * i / count);
                g.setColour (Theme::redBright.withAlpha (0.8f));
                g.drawVerticalLine (x, 0.0f, (float) h);
                g.setColour (Theme::text.withAlpha (0.85f));
                g.drawText (juce::MidiMessage::getMidiNoteName (sampler.params.sliceNote.load() + i, true, true, 3), x + 3, h - 16, 40, 14,
                            juce::Justification::left);
            }
        }
        else if (mode == Sampler::Classic && sampler.params.loop.load())
        {
            const int ls = xOf (sampler.params.loopStart), le = xOf (sampler.params.loopEnd);
            g.setColour (Theme::orange.withAlpha (0.18f));
            g.fillRect (juce::jmin (ls, le), 0, std::abs (le - ls), h);
            g.setColour (Theme::orange);
            g.drawVerticalLine (ls, 0.0f, (float) h);
            g.drawVerticalLine (le, 0.0f, (float) h);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("loop", ls + 4, 3, 40, 14, juce::Justification::left);
        }

        g.setColour (Theme::yellow);
        g.fillRect (sx - 1, 0, 3, h);
        g.fillRect (ex - 1, 0, 3, h);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText ("start", sx + 5, h - 16, 40, 14, juce::Justification::left);
        g.drawText ("end", ex - 33, h - 16, 30, 14, juce::Justification::right);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabbed = nearestMarker (e.x);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (grabbed < 0 || getWidth() <= 0)
            return;
        const float f = juce::jlimit (0.0f, 1.0f, (float) e.x / (float) getWidth());
        auto& p = sampler.params;
        switch (grabbed)
        {
            case 0: p.start = juce::jmin (f, p.end.load() - 0.001f); break;
            case 1: p.end = juce::jmax (f, p.start.load() + 0.001f); break;
            case 2: p.loopStart = juce::jmin (f, p.loopEnd.load() - 0.001f); break;
            case 3: p.loopEnd = juce::jmax (f, p.loopStart.load() + 0.001f); break;
            default: break;
        }
        repaint();
        changed();
    }

    void mouseUp (const juce::MouseEvent&) override { grabbed = -1; }

    bool isInterestedInFileDrag (const juce::StringArray& files) override { return files.size() > 0; }
    void filesDropped (const juce::StringArray& files, int, int) override
    {
        if (files.size() > 0 && fileDropped)
            fileDropped (juce::File (files[0]));
    }

private:
    int xOf (float fraction) const { return (int) std::round (juce::jlimit (0.0f, 1.0f, fraction) * (float) getWidth()); }

    // 0 start, 1 end, 2 loop start, 3 loop end (the loop markers only when they are shown); -1 none
    int nearestMarker (int x) const
    {
        const bool loopShown = sampler.params.mode.load() == Sampler::Classic && sampler.params.loop.load();
        int best = -1, bestDistance = 10;
        const int positions[4] = { xOf (sampler.params.start), xOf (sampler.params.end), xOf (sampler.params.loopStart), xOf (sampler.params.loopEnd) };
        for (int i = 0; i < (loopShown ? 4 : 2); ++i)
            if (std::abs (positions[i] - x) < bestDistance)
            {
                bestDistance = std::abs (positions[i] - x);
                best = i;
            }
        return best;
    }

    Sampler& sampler;
    std::function<void()> changed;
    std::function<void (const juce::File&)> fileDropped;
    const void* peaksOf = nullptr;
    std::vector<float> peaks;
    int grabbed = -1;
};

// The sampler's own window: sample, markers, mode, root note, envelope, filter. Every change applies at once.
class SamplerPanel : public juce::Component
{
public:
    // `loadFile` puts a file into this sampler (true if it worked). Drum pads use the panel with allowSlice = false.
    SamplerPanel (Sampler& s, std::function<bool (const juce::File&)> loadFile, std::function<void()> onChanged, std::function<void (bool)> onPlay,
                  bool allowSlice = true, std::function<bool (double)> stretchFile = nullptr)
        : sampler (s), loader (std::move (loadFile)), changed (std::move (onChanged)), play (std::move (onPlay)), slicesAllowed (allowSlice), stretcher (std::move (stretchFile)),
          waveform (s, [this] { changedFn(); }, [this] (const juce::File& f) { load (f); })
    {
        auto& p = sampler.params;

        addAndMakeVisible (waveform);

        loadButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Choose a sample", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) load (fc.getResult()); });
        };
        addAndMakeVisible (loadButton);

        nameLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        nameLabel.setColour (juce::Label::textColourId, Theme::text);
        addAndMakeVisible (nameLabel);

        modeBox.addItem ("Classic (pitched by the key, loops)", 1);
        modeBox.addItem ("One-shot (plays out whatever the key does)", 2);
        if (slicesAllowed)
            modeBox.addItem ("Slice (a key per slice)", 3);
        modeBox.setSelectedId (p.mode.load() + 1, juce::dontSendNotification);
        modeBox.onChange = [this] { sampler.params.mode = modeBox.getSelectedId() - 1; refreshVisibility(); changedFn(); waveform.repaint(); };
        addAndMakeVisible (modeBox);

        for (int n = 24; n <= 108; ++n)   // C0 .. C8, with C3 = 60 (the naming of Ableton and most DAWs)
            rootBox.addItem (juce::MidiMessage::getMidiNoteName (n, true, true, 3), n + 1);
        rootBox.setSelectedId (p.rootNote.load() + 1, juce::dontSendNotification);
        rootBox.setTooltip ("The key at which the sample plays at its own pitch");
        rootBox.onChange = [this] { sampler.params.rootNote = rootBox.getSelectedId() - 1; changedFn(); };
        addAndMakeVisible (rootBox);
        rootLabel.setText ("Root", juce::dontSendNotification);
        rootLabel.setColour (juce::Label::textColourId, Theme::textDim);
        addAndMakeVisible (rootLabel);

        detectButton.setTooltip ("Listens to the sample, finds its pitch and sets the root note and the tuning so that it plays in tune");
        detectButton.onClick = [this] { detectPitch(); };
        addAndMakeVisible (detectButton);

        playButton.setClickingTogglesState (false);
        playButton.setTooltip ("Hold to play the sample at the root note");
        playButton.onStateChange = [this]
        {
            const bool down = playButton.isDown();
            if (down != playing)
            {
                playing = down;
                play (down);
            }
        };
        addAndMakeVisible (playButton);

        auto addSlider = [this] (juce::Slider& s, juce::Label& l, const juce::String& name, double lo, double hi, double value, double mid,
                                 const juce::String& suffix, int decimals, std::function<void (double)> set)
        {
            l.setText (name, juce::dontSendNotification);
            l.setColour (juce::Label::textColourId, Theme::textDim);
            l.setFont (juce::FontOptions (13.0f));
            addAndMakeVisible (l);
            s.setSliderStyle (juce::Slider::LinearHorizontal);
            s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 74, 20);
            s.setRange (lo, hi);
            if (mid > lo && mid < hi)
                s.setSkewFactorFromMidPoint (mid);
            s.setNumDecimalPlacesToDisplay (decimals);
            s.setTextValueSuffix (suffix);
            s.setValue (value, juce::dontSendNotification);
            s.setDoubleClickReturnValue (true, value);
            s.onValueChange = [this, &s, set] { set (s.getValue()); changedFn(); };
            addAndMakeVisible (s);
        };
        addSlider (tune, tuneLabel, "Tune", -100.0, 100.0, p.tuneCents, 0.0, " ct", 0, [this] (double v) { sampler.params.tuneCents = (float) v; });
        addSlider (volume, volumeLabel, "Volume", 0.0, 1.5, p.volume, 0.0, "", 2, [this] (double v) { sampler.params.volume = (float) v; });
        addSlider (velocity, velocityLabel, "Velocity", 0.0, 1.0, p.velocitySens, 0.0, "", 2, [this] (double v) { sampler.params.velocitySens = (float) v; });
        addSlider (filter, filterLabel, "Filter", 40.0, 20000.0, p.cutoff, 1500.0, " Hz", 0, [this] (double v) { sampler.params.cutoff = (float) v; });
        addSlider (attack, attackLabel, "Attack", 0.0005, 5.0, p.attack, 0.1, " s", 3, [this] (double v) { sampler.params.attack = (float) v; });
        addSlider (decay, decayLabel, "Decay", 0.001, 8.0, p.decay, 0.3, " s", 3, [this] (double v) { sampler.params.decay = (float) v; });
        addSlider (sustain, sustainLabel, "Sustain", 0.0, 1.0, p.sustain, 0.0, "", 2, [this] (double v) { sampler.params.sustain = (float) v; });
        addSlider (release, releaseLabel, "Release", 0.002, 8.0, p.release, 0.3, " s", 3, [this] (double v) { sampler.params.release = (float) v; });
        addSlider (slices, slicesLabel, "Slices", 1.0, 32.0, p.slices, 0.0, "", 0, [this] (double v) { sampler.params.slices = (int) v; waveform.repaint(); });
        addSlider (stretch, stretchLabel, "Stretch", 0.25, 4.0, sampler.getStretch(), 1.0, " x", 2, [] (double) {});
        stretch.setTooltip ("Time-stretch: 2 = twice as long, 0.5 = twice as fast. The pitch stays the same (Rubber Band, applied when you let go)");
        stretch.onValueChange = [this] { if (! stretch.isMouseButtonDown()) applyStretch(); };
        stretch.onDragEnd = [this] { applyStretch(); };
        slices.setNumDecimalPlacesToDisplay (0);
        slices.setRange (1.0, 32.0, 1.0);

        loopToggle.setToggleState (p.loop, juce::dontSendNotification);
        loopToggle.onClick = [this] { sampler.params.loop = loopToggle.getToggleState(); waveform.repaint(); changedFn(); };
        loopToggle.setTooltip ("Classic mode: the part between the orange markers repeats while the key is held");
        addAndMakeVisible (loopToggle);
        reverseToggle.setToggleState (p.reverse, juce::dontSendNotification);
        reverseToggle.onClick = [this] { sampler.params.reverse = reverseToggle.getToggleState(); changedFn(); };
        addAndMakeVisible (reverseToggle);

        hint.setText ("Drag the yellow markers to choose the part that plays. Changes here are not part of undo, but they are saved with the project.",
                      juce::dontSendNotification);
        hint.setFont (juce::FontOptions (12.0f));
        hint.setColour (juce::Label::textColourId, Theme::textFaint);
        addAndMakeVisible (hint);

        refreshName();
        refreshVisibility();
        setSize (780, 482);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        auto top = r.removeFromTop (28);
        loadButton.setBounds (top.removeFromLeft (84));
        top.removeFromLeft (8);
        playButton.setBounds (top.removeFromRight (64));
        top.removeFromRight (6);
        detectButton.setBounds (top.removeFromRight (104));
        top.removeFromRight (6);
        rootBox.setBounds (top.removeFromRight (80));
        top.removeFromRight (4);
        rootLabel.setBounds (top.removeFromRight (36));
        top.removeFromRight (8);
        modeBox.setBounds (top.removeFromRight (290));
        top.removeFromRight (8);
        nameLabel.setBounds (top);

        r.removeFromTop (8);
        waveform.setBounds (r.removeFromTop (190));
        r.removeFromTop (10);

        auto columns = r.removeFromTop (5 * 30);
        auto left = columns.removeFromLeft (columns.getWidth() / 2 - 8);
        columns.removeFromLeft (16);
        auto row = [] (juce::Rectangle<int>& area, juce::Label& l, juce::Component& c)
        {
            auto line = area.removeFromTop (30);
            l.setBounds (line.removeFromLeft (70));
            c.setBounds (line.reduced (0, 3));
        };
        row (left, tuneLabel, tune);
        row (left, volumeLabel, volume);
        row (left, velocityLabel, velocity);
        row (left, filterLabel, filter);
        row (left, stretchLabel, stretch);
        row (columns, attackLabel, attack);
        row (columns, decayLabel, decay);
        row (columns, sustainLabel, sustain);
        row (columns, releaseLabel, release);

        r.removeFromTop (6);
        auto bottom = r.removeFromTop (28);
        loopToggle.setBounds (bottom.removeFromLeft (150));
        reverseToggle.setBounds (bottom.removeFromLeft (110));
        bottom.removeFromLeft (20);
        slicesLabel.setBounds (bottom.removeFromLeft (60));
        slices.setBounds (bottom.removeFromLeft (260));
        r.removeFromTop (6);
        hint.setBounds (r.removeFromTop (22));
    }

    void refreshName()
    {
        stretch.setValue (sampler.getStretch(), juce::dontSendNotification);
        stretch.setEnabled (stretcher != nullptr && sampler.getSample() != nullptr);
        nameLabel.setText (sampler.getSample() == nullptr ? juce::String ("No sample") : sampler.getName(), juce::dontSendNotification);
        waveform.repaint();
    }

private:
    void changedFn() { if (changed) changed(); }

    void applyStretch()
    {
        if (stretcher == nullptr || std::abs (stretch.getValue() - sampler.getStretch()) < 0.001)
            return;
        juce::MouseCursor::showWaitCursor();
        const bool ok = stretcher (stretch.getValue());
        juce::MouseCursor::hideWaitCursor();
        if (! ok)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Cannot stretch", "The sample could not be time-stretched.");
        stretch.setValue (sampler.getStretch(), juce::dontSendNotification);
        waveform.repaint();
        changedFn();
    }

    void refreshVisibility()
    {
        const bool sliceMode = sampler.params.mode.load() == Sampler::Slice;
        slices.setVisible (sliceMode);
        slicesLabel.setVisible (sliceMode);
        loopToggle.setVisible (sampler.params.mode.load() == Sampler::Classic);
        rootBox.setEnabled (! sliceMode);
        detectButton.setEnabled (! sliceMode);
    }

    void load (const juce::File& file)
    {
        if (! loader (file))
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Cannot load the sample", file.getFullPathName());
            return;
        }
        auto& p = sampler.params;   // a new sample: the whole of it plays, from the start
        p.start = 0.0f;
        p.end = 1.0f;
        p.loopStart = 0.0f;
        p.loopEnd = 1.0f;
        refreshName();
        changedFn();
    }

    // The pitch of the loudest part of the sample (YIN, the same detector as the tuner): sets the root note and the tuning
    void detectPitch()
    {
        const auto sample = sampler.getSample();
        if (sample == nullptr)
            return;

        constexpr int window = 4096;
        const int total = sample->getNumSamples();
        if (total < window)
            return;

        // the loudest 4096 samples within the first few seconds
        const int limit = juce::jmin (total - window, (int) (sampler.getSampleRate() * 6.0));
        int bestStart = 0;
        float bestEnergy = 0.0f;
        for (int s = 0; s <= limit; s += window / 2)
        {
            float energy = 0.0f;
            for (int i = 0; i < window; i += 8)
                energy += std::abs (sample->getSample (0, s + i));
            if (energy > bestEnergy)
            {
                bestEnergy = energy;
                bestStart = s;
            }
        }

        std::vector<float> mono ((size_t) window);
        for (int i = 0; i < window; ++i)
            mono[(size_t) i] = sample->getNumChannels() > 1 ? 0.5f * (sample->getSample (0, bestStart + i) + sample->getSample (1, bestStart + i))
                                                             : sample->getSample (0, bestStart + i);
        float rms = 0.0f;
        const float hz = detectPitchHz (mono.data(), window, sampler.getSampleRate(), rms);
        if (hz <= 0.0f)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "No pitch found",
                                                    "The sample has no clear pitch (drums and noise do not). Set the root note by hand.");
            return;
        }

        const double midi = 69.0 + 12.0 * std::log2 ((double) hz / 440.0);
        const int note = juce::jlimit (24, 108, (int) std::lround (midi));
        const float cents = (float) ((midi - (double) note) * 100.0);   // how sharp it is: the tuning takes that away again
        sampler.params.rootNote = note;
        sampler.params.tuneCents = juce::jlimit (-100.0f, 100.0f, -cents);
        rootBox.setSelectedId (note + 1, juce::dontSendNotification);
        tune.setValue (sampler.params.tuneCents, juce::dontSendNotification);
        changedFn();
    }

    Sampler& sampler;
    std::function<bool (const juce::File&)> loader;
    std::function<void()> changed;
    std::function<void (bool)> play;
    bool slicesAllowed = true;
    std::function<bool (double)> stretcher;
    bool playing = false;
    std::unique_ptr<juce::FileChooser> chooser;

    SamplerWaveform waveform;
    juce::TextButton loadButton { "Load..." }, detectButton { "Detect pitch" }, playButton { "Play" };
    juce::Label nameLabel, rootLabel, hint;
    juce::ComboBox modeBox, rootBox;
    juce::Label stretchLabel, tuneLabel, volumeLabel, velocityLabel, filterLabel, attackLabel, decayLabel, sustainLabel, releaseLabel, slicesLabel;
    juce::Slider stretch, tune, volume, velocity, filter, attack, decay, sustain, release, slices;
    juce::ToggleButton loopToggle { "Loop" }, reverseToggle { "Reverse" };
};

// A window of its own for a track's sampler (like the editor window of a plugin).
class SamplerWindow : public juce::DocumentWindow
{
public:
    SamplerWindow (const juce::String& title, Sampler& s, std::function<bool (const juce::File&)> loadFile, std::function<void()> onChanged,
                   std::function<void (bool)> onPlay, std::function<void()> onClosed, bool allowSlice = true,
                   std::function<bool (double)> stretchFile = nullptr)
        : juce::DocumentWindow (title, Theme::background, juce::DocumentWindow::closeButton), closed (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new SamplerPanel (s, std::move (loadFile), std::move (onChanged), std::move (onPlay), allowSlice, std::move (stretchFile)), true);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        if (closed)
            closed();   // the owner deletes this window
    }

private:
    std::function<void()> closed;
};
