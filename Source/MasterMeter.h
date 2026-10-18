#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "Loudness.h"
#include "Theme.h"

// The spectrum of the master (log frequency, dB), smoothed, with a slowly falling peak line.
class SpectrumView : public juce::Component
{
public:
    explicit SpectrumView (Engine& e) : engine (e) { shown.assign (bars, -120.0f); held.assign (bars, -120.0f); }

    // Message thread, ~25 times a second
    void update()
    {
        constexpr int n = 4096;
        float samples[n];
        std::vector<float> db;
        if (engine.spectrum.readLatest (samples, n))
            spectrumDb (samples, n, db);
        else
            db.assign (n / 2, -120.0f);

        const double rate = engine.getSampleRate(), binHz = rate / n;
        for (int b = 0; b < bars; ++b)
        {
            const double f0 = frequencyOf ((double) b / bars), f1 = frequencyOf ((double) (b + 1) / bars);
            const int i0 = juce::jlimit (1, n / 2 - 1, (int) std::floor (f0 / binHz)), i1 = juce::jlimit (i0, n / 2 - 1, (int) std::ceil (f1 / binHz));
            float m = -120.0f;
            for (int i = i0; i <= i1; ++i)
                m = juce::jmax (m, db[(size_t) i]);
            shown[(size_t) b] = m > shown[(size_t) b] ? m : juce::jmax (m, shown[(size_t) b] - 2.5f);   // (rises at once, falls smoothly)
            held[(size_t) b] = m > held[(size_t) b] ? m : juce::jmax (-120.0f, held[(size_t) b] - 0.35f);
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (Theme::laneBg);
        g.fillRoundedRectangle (area, 6.0f);
        const auto plot = area.reduced (34.0f, 14.0f).withTrimmedBottom (6.0f);

        g.setFont (juce::FontOptions (10.5f));
        for (int db = -90; db <= 0; db += 15)   // the level lines
        {
            const float y = yOf (db, plot);
            g.setColour (Theme::text.withAlpha (db == 0 ? 0.25f : 0.08f));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
            g.setColour (Theme::textFaint);
            g.drawText (juce::String (db), (int) area.getX() + 2, (int) y - 6, 28, 12, juce::Justification::right);
        }
        for (double f : { 30.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })   // the frequency lines
        {
            const float x = plot.getX() + (float) positionOf (f) * plot.getWidth();
            g.setColour (Theme::text.withAlpha (0.08f));
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (Theme::textFaint);
            g.drawText (f >= 1000.0 ? juce::String (f / 1000.0, f >= 10000.0 ? 0 : (f == 1000.0 || f == 2000.0 || f == 5000.0 ? 0 : 1)) + "k" : juce::String ((int) f),
                        (int) x - 16, (int) plot.getBottom() + 1, 32, 12, juce::Justification::centred);
        }

        const float bw = plot.getWidth() / (float) bars;
        for (int b = 0; b < bars; ++b)
        {
            const float level = shown[(size_t) b];
            const float top = yOf (level, plot);
            const auto colour = level > -6.0f ? Theme::redBright : level > -24.0f ? Theme::yellow : Theme::orange;
            g.setColour (colour.withAlpha (0.85f));
            g.fillRect (plot.getX() + b * bw + 0.5f, top, juce::jmax (1.0f, bw - 1.0f), plot.getBottom() - top);
            const float ph = yOf (held[(size_t) b], plot);
            g.setColour (Theme::text.withAlpha (0.6f));
            g.fillRect (plot.getX() + b * bw + 0.5f, ph, juce::jmax (1.0f, bw - 1.0f), 1.5f);
        }
    }

private:
    static constexpr int bars = 120;
    static double frequencyOf (double position) { return 20.0 * std::pow (1000.0, position); }       // 20 Hz .. 20 kHz
    static double positionOf (double hz) { return std::log (hz / 20.0) / std::log (1000.0); }
    static float yOf (float db, const juce::Rectangle<float>& plot) { return plot.getBottom() - juce::jlimit (0.0f, 1.0f, (db + 90.0f) / 90.0f) * plot.getHeight(); }

    Engine& engine;
    std::vector<float> shown, held;
};

//==============================================================================
// The master meter: loudness in LUFS (momentary, short-term, integrated) with a target to aim for, the peak, the spectrum, and below it
// a reference track to compare your mix with at the same loudness.
class MasterMeterPanel : public juce::Component, private juce::Timer
{
public:
    MasterMeterPanel (Engine& e, std::function<void()> onChanged) : engine (e), changed (std::move (onChanged)), spectrumView (e)
    {
        addAndMakeVisible (spectrumView);

        targetBox.addItem ("Target: -14 LUFS  (Spotify, YouTube, Tidal)", 1);
        targetBox.addItem ("Target: -16 LUFS  (Apple Music)", 2);
        targetBox.addItem ("Target: -23 LUFS  (broadcast, EBU R128)", 3);
        targetBox.addItem ("Target: -9 LUFS  (loud masters)", 4);
        targetBox.setSelectedId (1, juce::dontSendNotification);
        addAndMakeVisible (targetBox);

        restartButton.setTooltip ("Starts the integrated loudness and the peak again (play the whole song for a reading of the whole song)");
        restartButton.onClick = [this] { engine.loudness.restart(); repaint(); };
        addAndMakeVisible (restartButton);

        loadButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Choose a reference track (a finished song)", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
            {
                if (! fc.getResult().existsAsFile())
                    return;
                juce::MouseCursor::showWaitCursor();
                const bool ok = engine.loadReference (fc.getResult());
                juce::MouseCursor::hideWaitCursor();
                if (! ok)
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Cannot open the reference", fc.getResult().getFullPathName());
                else
                {
                    matchLoudness (false);
                    if (changed) changed();
                }
                refreshReference();
            });
        };
        loadButton.setTooltip ("A finished song to compare your mix with. It plays along with the transport, from the start of the project (or from the playhead: Start here)");
        addAndMakeVisible (loadButton);

        abButton.setClickingTogglesState (true);
        abButton.setColour (juce::TextButton::buttonOnColourId, Theme::orange);
        abButton.setTooltip ("Switch between your mix and the reference (key B). The reference is not part of exports.");
        abButton.onClick = [this] { engine.setReferenceOn (abButton.getToggleState()); };
        addAndMakeVisible (abButton);

        gain.setSliderStyle (juce::Slider::LinearHorizontal);
        gain.setTextBoxStyle (juce::Slider::TextBoxRight, false, 66, 20);
        gain.setRange (-30.0, 20.0, 0.1);
        gain.setTextValueSuffix (" dB");
        gain.setDoubleClickReturnValue (true, 0.0);
        gain.setTooltip ("The level of the reference");
        gain.onValueChange = [this] { engine.setReferenceGainDb ((float) gain.getValue()); if (changed) changed(); };
        addAndMakeVisible (gain);

        matchButton.setTooltip ("Sets the level of the reference to the loudness of your mix (play your mix for a moment first), so that louder does not sound better");
        matchButton.onClick = [this] { matchLoudness (true); };
        addAndMakeVisible (matchButton);

        startButton.setTooltip ("The reference starts at the playhead: line up the part of the song you want to compare with the part of your project");
        startButton.onClick = [this] { engine.setReferenceOffset (engine.getPositionSeconds()); if (changed) changed(); refreshReference(); };
        addAndMakeVisible (startButton);

        removeButton.onClick = [this] { engine.clearReference(); if (changed) changed(); refreshReference(); };
        addAndMakeVisible (removeButton);

        referenceLabel.setFont (juce::FontOptions (13.0f));
        referenceLabel.setColour (juce::Label::textColourId, Theme::text);
        addAndMakeVisible (referenceLabel);

        refreshReference();
        setSize (760, 580);
        startTimerHz (20);
    }

    ~MasterMeterPanel() override { stopTimer(); }

    void refreshReference()
    {
        const bool has = engine.hasReference();
        abButton.setEnabled (has);
        gain.setEnabled (has);
        matchButton.setEnabled (has);
        startButton.setEnabled (has);
        removeButton.setEnabled (has);
        abButton.setToggleState (engine.isReferenceOn(), juce::dontSendNotification);
        gain.setValue (engine.getReferenceGainDb(), juce::dontSendNotification);
        referenceLabel.setText (has ? engine.getReferenceName() + "   |   " + juce::String (engine.getReferenceLufs(), 1) + " LUFS   |   starts at "
                                          + juce::String (engine.getReferenceOffset(), 2) + " s"
                                    : juce::String ("No reference yet: load a finished song to compare your mix with."),
                                juce::dontSendNotification);
        abButton.setButtonText (engine.isReferenceOn() ? "Hearing: REFERENCE" : "Hearing: your mix   (B)");
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        const double target = targetLufs();
        struct Box { const char* name; double value; bool isPeak; };
        const Box boxes[] = { { "MOMENTARY", engine.loudness.momentary(), false }, { "SHORT-TERM", engine.loudness.shortTerm(), false },
                              { "INTEGRATED", engine.loudness.integrated(), false },
                              { "PEAK", (double) engine.loudness.samplePeak(), true } };
        for (int i = 0; i < 4; ++i)
        {
            const auto r = boxRect (i);
            g.setColour (Theme::laneBg);
            g.fillRoundedRectangle (r.toFloat(), 8.0f);
            g.setColour (Theme::textDim);
            g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
            g.drawText (boxes[i].name, r.withHeight (20).translated (0, 4), juce::Justification::centred);

            juce::String text;
            auto colour = Theme::text;
            if (boxes[i].isPeak)
            {
                const double db = boxes[i].value > 1.0e-6 ? 20.0 * std::log10 (boxes[i].value) : -120.0;
                text = db > -100.0 ? juce::String (db, 1) + " dB" : juce::String ("-inf");
                colour = db >= -0.1 ? Theme::redBright : db > -1.0 ? Theme::yellow : Theme::text;
            }
            else
            {
                text = boxes[i].value > -100.0 ? juce::String (boxes[i].value, 1) : juce::String ("-");
                if (boxes[i].value > -100.0)
                {
                    const double off = boxes[i].value - target;
                    colour = std::abs (off) <= 1.0 ? Theme::yellow : off > 1.0 ? Theme::redBright : Theme::orange;
                }
            }
            g.setColour (colour);
            g.setFont (juce::FontOptions (30.0f, juce::Font::bold));
            g.drawText (text, r.withTrimmedTop (22).withTrimmedBottom (18), juce::Justification::centred);
            g.setColour (Theme::textFaint);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (boxes[i].isPeak ? "highest sample" : "LUFS", r.withTop (r.getBottom() - 18), juce::Justification::centred);
        }

        // how far the integrated loudness is from the target
        const double integrated = boxes[2].value;
        g.setFont (juce::FontOptions (13.0f));
        g.setColour (Theme::textDim);
        juce::String advice = "Play the song from start to end for the integrated reading.";
        if (integrated > -100.0)
        {
            const double off = integrated - target;
            advice = std::abs (off) <= 1.0 ? "Right on the target."
                                           : off > 0 ? juce::String (off, 1) + " LU louder than the target: turn the master down, or the streaming service will."
                                                     : juce::String (-off, 1) + " LU quieter than the target.";
        }
        g.drawText (advice, adviceArea, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        r.removeFromTop (92);                                  // the four readouts (drawn in paint)
        r.removeFromTop (6);
        auto row = r.removeFromTop (28);
        restartButton.setBounds (row.removeFromRight (90));
        row.removeFromRight (8);
        targetBox.setBounds (row.removeFromRight (330));
        row.removeFromRight (10);
        adviceArea = row;
        r.removeFromTop (8);

        auto reference = r.removeFromBottom (92);              // the reference
        r.removeFromBottom (8);
        spectrumView.setBounds (r);

        auto line1 = reference.removeFromTop (28);
        loadButton.setBounds (line1.removeFromLeft (150));
        line1.removeFromLeft (8);
        removeButton.setBounds (line1.removeFromRight (70));
        line1.removeFromRight (8);
        referenceLabel.setBounds (line1);
        reference.removeFromTop (8);
        auto line2 = reference.removeFromTop (30);
        abButton.setBounds (line2.removeFromLeft (200));
        line2.removeFromLeft (10);
        startButton.setBounds (line2.removeFromRight (110));
        line2.removeFromRight (6);
        matchButton.setBounds (line2.removeFromRight (130));
        line2.removeFromRight (10);
        gain.setBounds (line2);
    }

private:
    juce::Rectangle<int> boxRect (int i) const
    {
        const int w = (getWidth() - 24 - 3 * 10) / 4;
        return { 12 + i * (w + 10), 10, w, 92 };
    }

    double targetLufs() const
    {
        switch (targetBox.getSelectedId())
        {
            case 2:  return -16.0;
            case 3:  return -23.0;
            case 4:  return -9.0;
            default: return -14.0;
        }
    }

    // The reference gets the level that makes it as loud as the mix (the mix's integrated loudness when there is one, else its short-term loudness)
    void matchLoudness (bool announce)
    {
        double mix = engine.loudness.integrated();
        if (mix < -70.0)
            mix = engine.loudness.shortTerm();
        if (mix < -70.0 || engine.getReferenceLufs() < -100.0)
        {
            if (announce)
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Play your mix first",
                                                        "To match the loudness, play your project for a moment (the meter above has to show a reading).");
            return;
        }
        engine.setReferenceGainDb ((float) (mix - engine.getReferenceLufs()));
        gain.setValue (engine.getReferenceGainDb(), juce::dontSendNotification);
        if (changed) changed();
    }

    void timerCallback() override
    {
        spectrumView.update();
        // (the A/B key in the main window may have switched it)
        if (abButton.getToggleState() != engine.isReferenceOn())
            refreshReference();
        repaint (0, 0, getWidth(), 140);
    }

    Engine& engine;
    std::function<void()> changed;
    SpectrumView spectrumView;
    juce::ComboBox targetBox;
    juce::TextButton restartButton { "Restart" }, loadButton { "Load reference..." }, abButton, matchButton { "Match loudness" },
                     startButton { "Start here" }, removeButton { "Remove" };
    juce::Slider gain;
    juce::Label referenceLabel;
    juce::Rectangle<int> adviceArea;
    std::unique_ptr<juce::FileChooser> chooser;
};

class MasterMeterWindow : public juce::DocumentWindow
{
public:
    MasterMeterWindow (Engine& e, std::function<void()> onChanged, std::function<void()> onClosed)
        : juce::DocumentWindow ("Master meter", Theme::background, juce::DocumentWindow::closeButton), engine (e), closed (std::move (onClosed))
    {
        engine.analysisOn = true;
        setUsingNativeTitleBar (true);
        panel = new MasterMeterPanel (e, std::move (onChanged));
        setContentOwned (panel, true);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    ~MasterMeterWindow() override { engine.analysisOn = false; }

    MasterMeterPanel& getPanel() { return *panel; }
    void closeButtonPressed() override { if (closed) closed(); }

private:
    Engine& engine;
    MasterMeterPanel* panel = nullptr;
    std::function<void()> closed;
};
