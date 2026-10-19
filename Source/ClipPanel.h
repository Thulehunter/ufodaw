#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "Theme.h"

// Gain, time-stretch and pitch-shift of one audio clip. The gain applies at once; stretch and pitch are processed when you press
// Apply (it takes a moment: the original audio is kept, so Reset puts it back).
class ClipPanel : public juce::Component
{
public:
    // `projectBpm` is the tempo at the clip, for "Fit to project tempo". `process(stretch, semitones)` does the work behind a progress
    // window; `beginEdit` is called before every change (undo), `changed` after it (repaint).
    ClipPanel (Engine& e, Track& t, int clip, double projectBpm, std::function<void()> beginEdit, std::function<void()> changed,
               std::function<void (double, double, bool)> process)
        : engine (e), track (t), clipId (clip), bpm (projectBpm), begin (std::move (beginEdit)), changedFn (std::move (changed)),
          processFn (std::move (process))
    {
        Clip c;
        engine.getClip (track, clipId, c);

        auto label = [this] (juce::Label& l, const juce::String& text, bool dim = true)
        {
            l.setText (text, juce::dontSendNotification);
            l.setFont (juce::FontOptions (13.0f));
            l.setColour (juce::Label::textColourId, dim ? Theme::textDim : Theme::text);
            addAndMakeVisible (l);
        };
        label (gainLabel, "Gain");
        label (pitchLabel, "Pitch");
        label (lengthLabel, "Length");
        label (bpmLabel, "Clip tempo");
        label (note, "Stretch and pitch are made offline from the original when you press Apply; Reset puts the original back.", true);
        note.setFont (juce::FontOptions (12.0f));
        note.setJustificationType (juce::Justification::topLeft);

        // gain, in dB (applies live)
        gain.setSliderStyle (juce::Slider::LinearHorizontal);
        gain.setTextBoxStyle (juce::Slider::TextBoxRight, false, 78, 22);
        gain.setRange (-36.0, 18.0, 0.1);
        gain.setValue (20.0 * std::log10 (juce::jmax (1.0e-4f, c.gain)), juce::dontSendNotification);
        gain.setTextValueSuffix (" dB");
        gain.setDoubleClickReturnValue (true, 0.0);
        gain.onDragStart = [this] { begin(); };
        gain.onValueChange = [this]
        {
            engine.setClipGain (track, clipId, (float) std::pow (10.0, gain.getValue() / 20.0));
            changedFn();
        };
        addAndMakeVisible (gain);

        normalize.setTooltip ("Sets the gain so that the loudest sample of the clip reaches -0.3 dB");
        normalize.onClick = [this]
        {
            begin();
            engine.normalizeClip (track, clipId);
            Clip now;
            engine.getClip (track, clipId, now);
            gain.setValue (20.0 * std::log10 (juce::jmax (1.0e-4f, now.gain)), juce::dontSendNotification);
            changedFn();
        };
        addAndMakeVisible (normalize);

        // pitch in semitones
        pitch.setSliderStyle (juce::Slider::LinearHorizontal);
        pitch.setTextBoxStyle (juce::Slider::TextBoxRight, false, 78, 22);
        pitch.setRange (-24.0, 24.0, 0.01);
        pitch.setValue (c.processed() ? c.pitch : 0.0, juce::dontSendNotification);
        pitch.setTextValueSuffix (" st");
        pitch.setDoubleClickReturnValue (true, 0.0);
        addAndMakeVisible (pitch);

        // length as a percentage of the original (200% = twice as long = half the speed)
        length.setSliderStyle (juce::Slider::LinearHorizontal);
        length.setTextBoxStyle (juce::Slider::TextBoxRight, false, 78, 22);
        length.setRange (25.0, 400.0, 0.1);
        length.setSkewFactorFromMidPoint (100.0);
        length.setValue ((c.processed() ? c.stretch : 1.0) * 100.0, juce::dontSendNotification);
        length.setTextValueSuffix (" %");
        length.setDoubleClickReturnValue (true, 100.0);
        addAndMakeVisible (length);

        originalBpm.setText (juce::String (bpm, 1), false);
        originalBpm.setInputRestrictions (6, "0123456789.");
        originalBpm.setJustification (juce::Justification::centred);
        originalBpm.setTooltip ("The tempo this clip was played at (the project is at " + juce::String (bpm, 1) + " BPM)");
        addAndMakeVisible (originalBpm);

        fit.setTooltip ("Sets the length so that the clip plays at the project's tempo (clip tempo / project tempo)");
        fit.onClick = [this]
        {
            const double original = originalBpm.getText().getDoubleValue();
            if (original > 20.0 && bpm > 20.0)
                length.setValue (juce::jlimit (25.0, 400.0, 100.0 * original / bpm), juce::sendNotificationSync);
        };
        addAndMakeVisible (fit);

        apply.onClick = [this] { processFn (length.getValue() / 100.0, pitch.getValue(), formants.getToggleState()); };
        formants.setToggleState (c.processed() && c.formants, juce::dontSendNotification);
        formants.setTooltip ("When the pitch changes: keeps the character of the voice or instrument (so a voice does not sound like a chipmunk or a giant). "
                             "Good for vocals; leave it off for drums and effects.");
        addAndMakeVisible (formants);
        addAndMakeVisible (apply);

        reset.onClick = [this]
        {
            length.setValue (100.0, juce::dontSendNotification);
            pitch.setValue (0.0, juce::dontSendNotification);
            processFn (1.0, 0.0, false);
        };
        addAndMakeVisible (reset);

        setSize (400, 292);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 12);
        auto row = [&] (juce::Label& l, juce::Component& c)
        {
            auto line = r.removeFromTop (26);
            l.setBounds (line.removeFromLeft (84));
            c.setBounds (line);
            r.removeFromTop (6);
        };

        auto gainLine = r.removeFromTop (26);
        gainLabel.setBounds (gainLine.removeFromLeft (84));
        normalize.setBounds (gainLine.removeFromRight (84));
        gainLine.removeFromRight (6);
        gain.setBounds (gainLine);
        r.removeFromTop (6);

        r.removeFromTop (6);
        row (pitchLabel, pitch);
        row (lengthLabel, length);
        formants.setBounds (r.removeFromTop (24).withTrimmedLeft (84));
        r.removeFromTop (4);

        auto bpmLine = r.removeFromTop (26);
        bpmLabel.setBounds (bpmLine.removeFromLeft (84));
        originalBpm.setBounds (bpmLine.removeFromLeft (70));
        bpmLine.removeFromLeft (8);
        fit.setBounds (bpmLine.removeFromLeft (170));
        r.removeFromTop (10);

        auto buttons = r.removeFromTop (28);
        apply.setBounds (buttons.removeFromLeft (170));
        buttons.removeFromLeft (8);
        reset.setBounds (buttons.removeFromLeft (110));
        r.removeFromTop (8);
        note.setBounds (r.removeFromTop (34));
    }

private:
    Engine& engine;
    Track& track;
    int clipId;
    double bpm;
    std::function<void()> begin, changedFn;
    std::function<void (double, double, bool)> processFn;

    juce::Label gainLabel, pitchLabel, lengthLabel, bpmLabel, note;
    juce::Slider gain, pitch, length;
    juce::TextEditor originalBpm;
    juce::ToggleButton formants { "Keep the voice character (formants)" };
    juce::TextButton normalize { "Normalize" }, fit { "Fit to project tempo" }, apply { "Apply stretch and pitch" }, reset { "Reset" };
};
