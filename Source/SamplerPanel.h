#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "Theme.h"
#include "Tuner.h"
#include "Beat.h"

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
            const int marked = sampler.params.markCount.load();
            const int count = marked > 0 ? marked : juce::jlimit (1, 64, sampler.params.slices.load());
            g.setFont (juce::FontOptions (11.0f));
            for (int i = 0; i < count; ++i)
            {
                const int x = marked > 0 ? juce::jmax (sx, xOf (sampler.params.marks[i].load()))
                                         : sx + (int) ((double) (ex - sx) * i / count);
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
        if (grabbed >= 0 && onBeginEdit)
            onBeginEdit();   // (one undo step for the whole drag)
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
public:
    std::function<void()> onBeginEdit;
};

// The tempo calculator of the sampler: how many beats the part between the yellow markers lasts, and what tempo that makes. It can set the
// project's tempo to it, or stretch the sample so that it fits the project's tempo.
class TempoCalcPanel : public juce::Component
{
public:
    TempoCalcPanel (double regionSeconds, double currentStretch, double projectBpm,
                    std::function<void (double)> onSetProjectBpm, std::function<void (double)> onStretchTo,
                    std::function<double()> onDetect = nullptr)
        : seconds (regionSeconds), stretchNow (currentStretch), projBpm (projectBpm),
          setProject (std::move (onSetProjectBpm)), stretchTo (std::move (onStretchTo)), detect (std::move (onDetect))
    {
        detectButton.setTooltip ("Listens to the part between the markers, finds its tempo and sets the number of beats to match");
        detectButton.onClick = [this]
        {
            const double bpm = detect ? detect() : 0.0;
            if (bpm <= 0.0)
            {
                result.setText ("No clear beat found", juce::dontSendNotification);
                return;
            }
            beats.setValue (juce::jlimit (1.0, 128.0, std::round (bpm * seconds / 60.0)), juce::dontSendNotification);
            detected = bpm;   // (the detected tempo itself is used, until the beats are changed by hand)
            update();
        };
        detectButton.setEnabled (detect != nullptr);
        addAndMakeVisible (detectButton);
        // a first guess: the number of beats that gives a tempo nearest to 120
        const int options[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 };
        int best = 4;
        double bestDistance = 1.0e9;
        for (int b : options)
        {
            const double d = std::abs (std::log2 (bpmFor (b) / 120.0));
            if (d < bestDistance) { bestDistance = d; best = b; }
        }

        info.setText ("The part between the markers lasts " + juce::String (seconds, 3) + " s.\nHow many beats (quarter notes) is that? (4 per bar in 4/4)",
                      juce::dontSendNotification);
        info.setColour (juce::Label::textColourId, Theme::textDim);
        info.setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (info);

        beats.setSliderStyle (juce::Slider::LinearBar);
        beats.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 22);
        beats.setRange (1.0, 128.0, 1.0);
        beats.setTextValueSuffix (" beats");
        beats.setValue (best, juce::dontSendNotification);
        beats.onValueChange = [this] { detected = 0.0; update(); };
        addAndMakeVisible (beats);
        for (auto* b : { &halve, &twice })
            addAndMakeVisible (*b);
        halve.onClick = [this] { scaleTempo (0.5); };
        twice.onClick = [this] { scaleTempo (2.0); };

        result.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        result.setColour (juce::Label::textColourId, Theme::yellow);
        result.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (result);

        setButton.onClick = [this] { if (setProject) setProject (bpmFor ((int) beats.getValue())); };
        setButton.setTooltip ("Makes this the tempo of the project (the first tempo section)");
        stretchButton.onClick = [this] { if (stretchTo) stretchTo (ratioFor ((int) beats.getValue())); };
        stretchButton.setTooltip ("Time-stretches the sample (the pitch stays) so that these beats last exactly one beat of the project's tempo each");
        addAndMakeVisible (setButton);
        addAndMakeVisible (stretchButton);

        update();
        setSize (360, 248);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        info.setBounds (r.removeFromTop (40));
        r.removeFromTop (4);
        detectButton.setBounds (r.removeFromTop (28));
        r.removeFromTop (6);
        auto row = r.removeFromTop (26);
        twice.setBounds (row.removeFromRight (40));
        row.removeFromRight (4);
        halve.setBounds (row.removeFromRight (40));
        row.removeFromRight (6);
        beats.setBounds (row);
        r.removeFromTop (6);
        result.setBounds (r.removeFromTop (34));
        r.removeFromTop (6);
        setButton.setBounds (r.removeFromTop (28));
        r.removeFromTop (6);
        stretchButton.setBounds (r.removeFromTop (28));
    }

private:
    // half / double tempo: also of a detected tempo (which then stays the exact value)
    void scaleTempo (double factor)
    {
        if (detected > 0.0)
        {
            detected *= factor;
            beats.setValue (juce::jlimit (1.0, 128.0, std::round (detected * seconds / 60.0)), juce::dontSendNotification);
            update();
        }
        else
            beats.setValue (juce::jlimit (1.0, 128.0, std::round (beats.getValue() * factor)));   // (twice the tempo = twice the beats in the same time)
    }

    double bpmFor (int beatCount) const
    {
        if (detected > 0.0)
            return detected;
        return seconds > 0.0 ? 60.0 * beatCount / seconds : 0.0;
    }

    double ratioFor (int beatCount) const   // the stretch (relative to the sample as loaded) that makes the sample's tempo the project's tempo
    {
        return stretchNow * bpmFor (beatCount) / juce::jmax (1.0, projBpm);
    }

    void update()
    {
        const int n = (int) beats.getValue();
        const double bpm = bpmFor (n);
        result.setText (juce::String (bpm, 2) + " BPM", juce::dontSendNotification);
        setButton.setButtonText ("Set the project tempo to " + juce::String (bpm, 1));
        setButton.setEnabled (bpm >= 20.0 && bpm <= 400.0);
        const double ratio = ratioFor (n);
        stretchButton.setButtonText ("Stretch the sample to the project's " + juce::String (projBpm, 1) + " BPM");
        stretchButton.setEnabled (ratio >= 0.25 && ratio <= 4.0 && std::abs (ratio - stretchNow) > 0.001);
    }

    double seconds, stretchNow, projBpm, detected = 0.0;
    std::function<void (double)> setProject, stretchTo;
    std::function<double()> detect;
    juce::Label info, result;
    juce::Slider beats;
    juce::TextButton halve { "/ 2" }, twice { "x 2" }, setButton, stretchButton, detectButton { "Detect the tempo" };
};

// The sampler's own window: sample, markers, mode, root note, envelope, filter. Every change applies at once.
class SamplerPanel : public juce::Component
{
public:
    // `loadFile` puts a file into this sampler (true if it worked). Drum pads use the panel with allowSlice = false.
    SamplerPanel (Sampler& s, std::function<bool (const juce::File&)> loadFile, std::function<void()> onChanged, std::function<void (bool)> onPlay,
                  bool allowSlice = true, std::function<void (double, std::function<void (bool)>)> stretchFile = nullptr)
        : sampler (s), loader (std::move (loadFile)), changed (std::move (onChanged)), play (std::move (onPlay)), slicesAllowed (allowSlice), stretcher (std::move (stretchFile)),
          waveform (s, [this] { changedFn(); }, [this] (const juce::File& f) { load (f); })
    {
        auto& p = sampler.params;

        waveform.onBeginEdit = [this] { beginEditFn(); };
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
        modeBox.onChange = [this] { beginEditFn(); sampler.params.mode = modeBox.getSelectedId() - 1; refreshVisibility(); changedFn(); waveform.repaint(); };
        addAndMakeVisible (modeBox);

        for (int n = 24; n <= 108; ++n)   // C0 .. C8, with C3 = 60 (the naming of Ableton and most DAWs)
            rootBox.addItem (juce::MidiMessage::getMidiNoteName (n, true, true, 3), n + 1);
        rootBox.setSelectedId (p.rootNote.load() + 1, juce::dontSendNotification);
        rootBox.setTooltip ("The key at which the sample plays at its own pitch");
        rootBox.onChange = [this] { beginEditFn(); sampler.params.rootNote = rootBox.getSelectedId() - 1; changedFn(); };
        addAndMakeVisible (rootBox);
        rootLabel.setText ("Root", juce::dontSendNotification);
        rootLabel.setColour (juce::Label::textColourId, Theme::textDim);
        addAndMakeVisible (rootLabel);

        detectButton.setTooltip ("Listens to the sample, finds its pitch and sets the root note and the tuning so that it plays in tune");
        detectButton.onClick = [this] { detectPitch(); };
        addAndMakeVisible (detectButton);

        tempoButton.setTooltip ("Tempo calculator: how many beats the part between the markers lasts, and the tempo that makes");
        tempoButton.onClick = [this] { openTempoCalc(); };
        addAndMakeVisible (tempoButton);

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
            s.onDragStart = [this] { beginEditFn(); };
            s.onValueChange = [this, &s, set] { if (! s.isMouseButtonDown()) beginEditFn(); set (s.getValue()); changedFn(); };
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
        addSlider (slices, slicesLabel, "Slices", 1.0, 32.0, p.slices, 0.0, "", 0,
                   [this] (double v) { sampler.setMarks ({}); sampler.params.slices = (int) v; waveform.repaint(); });   // (equal slices again)
        addSlider (stretch, stretchLabel, "Stretch", 0.25, 4.0, sampler.getStretch(), 1.0, " x", 2, [] (double) {});
        stretch.setTooltip ("Time-stretch: 2 = twice as long, 0.5 = twice as fast. The pitch stays the same (Rubber Band, applied when you let go)");
        stretch.onValueChange = [this] { if (! stretch.isMouseButtonDown()) applyStretch(); };
        stretch.onDragStart = nullptr;   // (the snapshot is taken when the stretch is applied)
        stretch.onDragEnd = [this] { applyStretch(); };
        slices.setNumDecimalPlacesToDisplay (0);
        slices.setRange (1.0, 32.0, 1.0);

        autoSliceButton.setTooltip ("Cuts the part between the markers at its hits (kicks, snares, hats...) instead of in equal parts. "
                                    "Slider: how sensitive it is (more sensitive = more, quieter hits)");
        autoSliceButton.onClick = [this] { sliceByHits(); };
        sensitivity.setSliderStyle (juce::Slider::LinearHorizontal);
        sensitivity.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        sensitivity.setRange (0.0, 1.0, 0.01);
        sensitivity.setValue (0.3, juce::dontSendNotification);
        sensitivity.setDoubleClickReturnValue (true, 0.3);
        sensitivity.setTooltip ("How sensitive the hit detection is");
        sensitivity.onValueChange = [this] { if (sampler.params.markCount.load() > 0 && ! sensitivity.isMouseButtonDown()) sliceByHits(); };
        sensitivity.onDragEnd = [this] { if (sampler.params.markCount.load() > 0) sliceByHits(); };   // (the hits follow the slider once they were found)
        toMidiButton.setTooltip ("Puts the slices on this sampler's track as a pattern: one note per slice (from the slice key up), in the "
                                 "timing of the sample. Set the sample to the project's tempo first (Tempo...) so it lines up with the grid");
        toMidiButton.onClick = [this] { if (onSlicesToMidi) onSlicesToMidi(); };
        addAndMakeVisible (autoSliceButton);
        addAndMakeVisible (sensitivity);
        addAndMakeVisible (toMidiButton);

        loopToggle.setToggleState (p.loop, juce::dontSendNotification);
        loopToggle.onClick = [this] { beginEditFn(); sampler.params.loop = loopToggle.getToggleState(); waveform.repaint(); changedFn(); };
        loopToggle.setTooltip ("Classic mode: the part between the orange markers repeats while the key is held");
        addAndMakeVisible (loopToggle);
        reverseToggle.setToggleState (p.reverse, juce::dontSendNotification);
        reverseToggle.onClick = [this] { beginEditFn(); sampler.params.reverse = reverseToggle.getToggleState(); changedFn(); };
        addAndMakeVisible (reverseToggle);

        hint.setText ("Drag the yellow markers to choose the part that plays. Every change can be undone with Ctrl+Z, and is saved with the project.",
                      juce::dontSendNotification);
        hint.setFont (juce::FontOptions (12.0f));
        hint.setColour (juce::Label::textColourId, Theme::textFaint);
        addAndMakeVisible (hint);

        refreshName();
        refreshVisibility();
        setSize (780, 482);
    }

    std::function<void()> onBeginEdit;   // set by the owner: called before every change (the undo snapshot)
    std::function<void()> onAbortEdit;   // set by the owner: the change did not happen after all (a cancelled stretch)
    std::function<void()> onSlicesToMidi;             // set by the owner: the "To MIDI" button (the owner builds the pattern)
    std::function<double()> projectBpm;               // set by the owner: the project's tempo (for the tempo calculator)
    std::function<void (double)> setProjectBpm;       // set by the owner: change the project's tempo (the owner takes the undo snapshot)

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
        if (loopToggle.isVisible())
            loopToggle.setBounds (bottom.removeFromLeft (150));
        reverseToggle.setBounds (bottom.removeFromLeft (100));
        bottom.removeFromLeft (8);
        tempoButton.setBounds (bottom.removeFromRight (96));
        toMidiButton.setBounds (bottom.removeFromRight (84).reduced (0, 1));
        bottom.removeFromRight (4);
        slicesLabel.setBounds (bottom.removeFromLeft (52));
        slices.setBounds (bottom.removeFromLeft (150));
        bottom.removeFromLeft (6);
        autoSliceButton.setBounds (bottom.removeFromLeft (104).reduced (0, 1));
        sensitivity.setBounds (bottom.removeFromLeft (84).reduced (0, 3));
        r.removeFromTop (6);
        hint.setBounds (r.removeFromTop (22));
    }

    // After an undo / redo: every control shows the sampler's values again
    void refresh()
    {
        auto& p = sampler.params;
        modeBox.setSelectedId (p.mode.load() + 1, juce::dontSendNotification);
        rootBox.setSelectedId (p.rootNote.load() + 1, juce::dontSendNotification);
        tune.setValue (p.tuneCents, juce::dontSendNotification);
        volume.setValue (p.volume, juce::dontSendNotification);
        velocity.setValue (p.velocitySens, juce::dontSendNotification);
        filter.setValue (p.cutoff, juce::dontSendNotification);
        attack.setValue (p.attack, juce::dontSendNotification);
        decay.setValue (p.decay, juce::dontSendNotification);
        sustain.setValue (p.sustain, juce::dontSendNotification);
        release.setValue (p.release, juce::dontSendNotification);
        slices.setValue (p.slices, juce::dontSendNotification);
        loopToggle.setToggleState (p.loop, juce::dontSendNotification);
        reverseToggle.setToggleState (p.reverse, juce::dontSendNotification);
        refreshVisibility();
        refreshName();   // (the name, the stretch and the waveform)
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
    void beginEditFn() { if (onBeginEdit) onBeginEdit(); }   // called before a change, so that it can be undone

    // Cuts the part between the markers at its hits; the slice keys follow (the first slice plays from the first marker)
    void sliceByHits()
    {
        const auto sample = sampler.getSample();
        if (sample == nullptr)
            return;
        const int total = sample->getNumSamples();
        const double a = juce::jmin ((double) sampler.params.start.load(), (double) sampler.params.end.load());
        const double b = juce::jmax ((double) sampler.params.start.load(), (double) sampler.params.end.load());
        const auto hits = detectOnsets (*sample, sampler.getSampleRate(), (int) (a * total), (int) (b * total), (float) sensitivity.getValue());
        std::vector<float> marks;
        for (int h : hits)
            marks.push_back ((float) h / (float) total);
        beginEditFn();
        sampler.setMarks (marks);
        slices.setValue ((double) juce::jmin (32, (int) marks.size()), juce::dontSendNotification);
        waveform.repaint();
        changedFn();
    }

    void openTempoCalc()
    {
        const auto sample = sampler.getSample();
        if (sample == nullptr || sampler.getSampleRate() <= 0.0)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "No sample", "Load a sample first.");
            return;
        }
        const double length = (double) sample->getNumSamples() / sampler.getSampleRate()
                              * std::abs ((double) sampler.params.end.load() - (double) sampler.params.start.load());
        const double project = projectBpm ? projectBpm() : 120.0;
        juce::Component::SafePointer<SamplerPanel> safe (this);
        auto calc = std::make_unique<TempoCalcPanel> (length, sampler.getStretch(), project,
            [safe] (double bpm) { if (safe != nullptr && safe->setProjectBpm) safe->setProjectBpm (bpm); },
            [safe] (double ratio)
            {
                if (safe == nullptr)
                    return;
                safe->stretch.setValue (ratio, juce::dontSendNotification);
                safe->applyStretch();
            },
            [safe]() -> double
            {
                if (safe == nullptr)
                    return 0.0;
                const auto s = safe->sampler.getSample();
                if (s == nullptr)
                    return 0.0;
                const double a = juce::jmin ((double) safe->sampler.params.start.load(), (double) safe->sampler.params.end.load());
                const double b = juce::jmax ((double) safe->sampler.params.start.load(), (double) safe->sampler.params.end.load());
                return detectBpm (*s, safe->sampler.getSampleRate(), (int) (a * s->getNumSamples()), (int) (b * s->getNumSamples()));
            });
        juce::CallOutBox::launchAsynchronously (std::move (calc), tempoButton.getScreenBounds(), nullptr);
    }

    void applyStretch()
    {
        if (stretcher == nullptr || std::abs (stretch.getValue() - sampler.getStretch()) < 0.001)
            return;
        beginEditFn();
        // the stretch runs in the background behind a progress window; this comes back when it is done (or cancelled)
        juce::Component::SafePointer<SamplerPanel> safe (this);
        stretcher (stretch.getValue(), [safe] (bool applied)
        {
            if (safe == nullptr)
                return;
            if (! applied && safe->onAbortEdit)
                safe->onAbortEdit();   // cancelled: the undo step taken before is dropped again
            safe->stretch.setValue (safe->sampler.getStretch(), juce::dontSendNotification);
            safe->waveform.repaint();
            safe->changedFn();
        });
    }

    void refreshVisibility()
    {
        const bool sliceMode = sampler.params.mode.load() == Sampler::Slice;
        slices.setVisible (sliceMode);
        slicesLabel.setVisible (sliceMode);
        autoSliceButton.setVisible (sliceMode);
        sensitivity.setVisible (sliceMode);
        toMidiButton.setVisible (sliceMode);
        loopToggle.setVisible (sampler.params.mode.load() == Sampler::Classic);
        rootBox.setEnabled (! sliceMode);
        detectButton.setEnabled (! sliceMode);
        resized();   // (the bottom row depends on what is shown)
    }

    void load (const juce::File& file)
    {
        beginEditFn();
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
        beginEditFn();
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
    std::function<void (double, std::function<void (bool)>)> stretcher;   // (ratio, done): runs in the background
    bool playing = false;
    std::unique_ptr<juce::FileChooser> chooser;

    SamplerWaveform waveform;
    juce::TextButton loadButton { "Load..." }, detectButton { "Detect pitch" }, playButton { "Play" }, tempoButton { "Tempo..." },
                     autoSliceButton { "Slice by hits" }, toMidiButton { "To MIDI" };
    juce::Slider sensitivity;
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
    SamplerPanel& getPanel() { return *panel; }

    // Ctrl+Z / Ctrl+Y work in this window too: the owner decides (it undoes the project)
    std::function<bool (const juce::KeyPress&)> onKey;
    bool keyPressed (const juce::KeyPress& k) override
    {
        return (onKey && onKey (k)) || juce::DocumentWindow::keyPressed (k);
    }

    SamplerWindow (const juce::String& title, Sampler& s, std::function<bool (const juce::File&)> loadFile, std::function<void()> onChanged,
                   std::function<void (bool)> onPlay, std::function<void()> onClosed, bool allowSlice = true,
                   std::function<void (double, std::function<void (bool)>)> stretchFile = nullptr)
        : juce::DocumentWindow (title, Theme::background, juce::DocumentWindow::closeButton), closed (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);
        panel = new SamplerPanel (s, std::move (loadFile), std::move (onChanged), std::move (onPlay), allowSlice, std::move (stretchFile));
        setContentOwned (panel, true);
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
    SamplerPanel* panel = nullptr;
    std::function<void()> closed;
};
