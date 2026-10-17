#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "Engine.h"
#include "MainComponent.h"
#include "Theme.h"

// The "Recording latency" tab of the audio settings: how much earlier a recording is placed, with a measurement.
class LatencyPanel : public juce::Component, private juce::Timer
{
public:
    LatencyPanel (juce::AudioDeviceManager& dm, Engine& e, std::function<void()> onChanged)
        : deviceManager (dm), engine (e), changed (std::move (onChanged))
    {
        auto label = [this] (juce::Label& l, const juce::String& text, bool dim)
        {
            l.setText (text, juce::dontSendNotification);
            l.setFont (juce::FontOptions (13.0f));
            l.setColour (juce::Label::textColourId, dim ? Theme::textDim : Theme::text);
            l.setJustificationType (juce::Justification::topLeft);
            addAndMakeVisible (l);
        };
        label (intro, "Sound takes a moment to travel through your interface (out to your headphones, and back in from the microphone "
                      "or guitar). A recording is placed on the timeline earlier by that round-trip time, so it lines up with what you "
                      "were playing along to.", true);
        label (reported, {}, false);
        label (modeLabel, "Compensation", true);
        label (measuredLabel, {}, false);
        label (inputLabel, "Listen on input", true);
        label (status, {}, false);
        label (extraLabel, "Extra offset", true);
        label (effective, {}, false);

        mode.addItem ("Automatic (what the driver reports)", 1);
        mode.addItem ("Measured (use the test below)", 2);
        mode.addItem ("None", 3);
        mode.setSelectedId (UiSettings::latencyMode + 1, juce::dontSendNotification);
        mode.onChange = [this]
        {
            UiSettings::latencyMode = mode.getSelectedId() - 1;
            changed();
            refresh();
        };
        addAndMakeVisible (mode);

        if (auto* device = dm.getCurrentAudioDevice())
        {
            const auto names = device->getInputChannelNames();
            const auto active = device->getActiveInputChannels();
            int n = 0;
            for (int c = active.findNextSetBit (0); c >= 0; c = active.findNextSetBit (c + 1), ++n)
                input.addItem ("Input " + juce::String (n + 1) + (juce::isPositiveAndBelow (c, names.size()) ? "  (" + names[c] + ")" : juce::String()), n + 1);
        }
        if (input.getNumItems() > 0)
            input.setSelectedId (1, juce::dontSendNotification);
        addAndMakeVisible (input);

        measure.onClick = [this] { startMeasuring(); };
        measure.setTooltip ("Plays a short click through the first two outputs and listens for it on the input. Turn your monitors down, and "
                            "connect an output of the interface to that input with a cable (or point a microphone at the speakers).");
        addAndMakeVisible (measure);

        extra.setSliderStyle (juce::Slider::LinearHorizontal);
        extra.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 22);
        extra.setRange (-200.0, 200.0, 0.1);
        extra.setValue (UiSettings::latencyExtraMs, juce::dontSendNotification);
        extra.setTextValueSuffix (" ms");
        extra.setDoubleClickReturnValue (true, 0.0);
        extra.setTooltip ("Fine tuning, in milliseconds: positive places recordings earlier, negative later (double-click: 0)");
        extra.onValueChange = [this] { UiSettings::latencyExtraMs = extra.getValue(); changed(); refresh(); };
        addAndMakeVisible (extra);

        refresh();
        setSize (540, 380);
    }

    ~LatencyPanel() override { engine.cancelLatencyTest(); }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        intro.setBounds (r.removeFromTop (58));
        r.removeFromTop (6);
        reported.setBounds (r.removeFromTop (20));
        r.removeFromTop (10);
        auto row = r.removeFromTop (26);
        modeLabel.setBounds (row.removeFromLeft (120));
        mode.setBounds (row);
        r.removeFromTop (10);
        measuredLabel.setBounds (r.removeFromTop (20));
        row = r.removeFromTop (26);
        inputLabel.setBounds (row.removeFromLeft (120));
        measure.setBounds (row.removeFromRight (110));
        row.removeFromRight (8);
        input.setBounds (row);
        r.removeFromTop (6);
        status.setBounds (r.removeFromTop (44));
        r.removeFromTop (6);
        row = r.removeFromTop (26);
        extraLabel.setBounds (row.removeFromLeft (120));
        extra.setBounds (row);
        r.removeFromTop (10);
        effective.setBounds (r.removeFromTop (22));
    }

private:
    double reportedMs() const
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
            return (double) (device->getInputLatencyInSamples() + device->getOutputLatencyInSamples()) * 1000.0
                   / juce::jmax (1.0, device->getCurrentSampleRate());
        return 0.0;
    }

    void refresh()
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            const double rate = juce::jmax (1.0, device->getCurrentSampleRate());
            reported.setText ("The driver reports  " + juce::String ((double) device->getInputLatencyInSamples() * 1000.0 / rate, 1) + " ms in  +  "
                              + juce::String ((double) device->getOutputLatencyInSamples() * 1000.0 / rate, 1) + " ms out  =  "
                              + juce::String (reportedMs(), 1) + " ms", juce::dontSendNotification);
        }
        else
        {
            reported.setText ("No audio device", juce::dontSendNotification);
        }

        measuredLabel.setText (UiSettings::latencyMeasuredMs > 0.0 ? "Measured round trip:  " + juce::String (UiSettings::latencyMeasuredMs, 1) + " ms"
                                                                   : juce::String ("Not measured yet"), juce::dontSendNotification);

        double ms = UiSettings::latencyMode == 0 ? reportedMs() : UiSettings::latencyMode == 1 ? UiSettings::latencyMeasuredMs : 0.0;
        ms += UiSettings::latencyExtraMs;
        effective.setText ("Recordings are placed  " + juce::String (ms, 1) + " ms  earlier", juce::dontSendNotification);
    }

    void startMeasuring()
    {
        engine.stop();
        if (input.getNumItems() == 0 || ! engine.startLatencyTest (input.getSelectedId() - 1))
        {
            status.setText ("Cannot measure: there is no input, or the transport is running.", juce::dontSendNotification);
            return;
        }
        status.setText ("Measuring...", juce::dontSendNotification);
        measure.setEnabled (false);
        startTimerHz (20);
    }

    void timerCallback() override
    {
        const int state = engine.latencyTestState();
        if (state == 1)
            return;

        stopTimer();
        measure.setEnabled (true);
        if (state != 2)
            return;

        const int samples = engine.analyseLatencyTest();
        if (samples < 0)
        {
            status.setText ("The click was not heard. Connect an output of the interface to the input above with a cable (or point a "
                            "microphone at the speakers), check the input level, and try again.", juce::dontSendNotification);
            return;
        }

        const double rate = juce::jmax (1.0, engine.getSampleRate());
        UiSettings::latencyMeasuredMs = (double) samples * 1000.0 / rate;
        UiSettings::latencyMode = 1;
        mode.setSelectedId (2, juce::dontSendNotification);
        status.setText ("Measured  " + juce::String (UiSettings::latencyMeasuredMs, 2) + " ms  (" + juce::String (samples) + " samples). "
                        "Compensation is now set to Measured.", juce::dontSendNotification);
        changed();
        refresh();
    }

    juce::AudioDeviceManager& deviceManager;
    Engine& engine;
    std::function<void()> changed;
    juce::Label intro, reported, modeLabel, measuredLabel, inputLabel, status, extraLabel, effective;
    juce::ComboBox mode, input;
    juce::TextButton measure { "Measure" };
    juce::Slider extra;
};

// The "MIDI clock" tab: which hardware follows UFODAW's tempo.
class ClockPanel : public juce::Component
{
public:
    ClockPanel (Engine& e, std::function<void()> onChanged) : engine (e), changed (std::move (onChanged))
    {
        intro.setText ("Send MIDI clock to a drum machine, sequencer or synthesizer so it follows UFODAW's tempo (and its tempo changes). "
                       "Start, or Continue with the song position, goes out when playback begins or jumps, and Stop when it stops.",
                       juce::dontSendNotification);
        intro.setFont (juce::FontOptions (13.0f));
        intro.setColour (juce::Label::textColourId, Theme::textDim);
        intro.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (intro);

        listLabel.setText ("Send MIDI clock to:", juce::dontSendNotification);
        listLabel.setColour (juce::Label::textColourId, Theme::text);
        addAndMakeVisible (listLabel);

        const auto devices = juce::MidiOutput::getAvailableDevices();
        for (auto& d : devices)
        {
            auto box = std::make_unique<juce::ToggleButton> (d.name);
            box->setToggleState (UiSettings::clockDevices.contains (d.identifier), juce::dontSendNotification);
            const auto id = d.identifier;
            auto* raw = box.get();
            box->onClick = [this, id, raw]
            {
                if (raw->getToggleState())
                    UiSettings::clockDevices.addIfNotAlreadyThere (id);
                else
                    UiSettings::clockDevices.removeString (id);
                apply();
            };
            addAndMakeVisible (*box);
            boxes.push_back (std::move (box));
        }
        if (devices.isEmpty())
        {
            none.setText ("No MIDI outputs found. Connect your interface or a USB MIDI device.", juce::dontSendNotification);
            none.setColour (juce::Label::textColourId, Theme::textFaint);
            addAndMakeVisible (none);
        }

        offsetLabel.setText ("Clock shift", juce::dontSendNotification);
        offsetLabel.setColour (juce::Label::textColourId, Theme::textDim);
        addAndMakeVisible (offsetLabel);
        offset.setSliderStyle (juce::Slider::LinearHorizontal);
        offset.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 22);
        offset.setRange (-50.0, 50.0, 0.1);
        offset.setValue (UiSettings::clockOffsetMs, juce::dontSendNotification);
        offset.setTextValueSuffix (" ms");
        offset.setDoubleClickReturnValue (true, 0.0);
        offset.setTooltip ("Sends the clock earlier (-) or later (+) to line your hardware up with the audio (double-click: 0)");
        offset.onValueChange = [this] { UiSettings::clockOffsetMs = offset.getValue(); apply(); };
        addAndMakeVisible (offset);

        setSize (540, 250 + (int) boxes.size() * 26);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        intro.setBounds (r.removeFromTop (58));
        r.removeFromTop (8);
        listLabel.setBounds (r.removeFromTop (22));
        for (auto& b : boxes)
            b->setBounds (r.removeFromTop (26));
        none.setBounds (r.removeFromTop (26));
        r.removeFromTop (14);
        auto row = r.removeFromTop (26);
        offsetLabel.setBounds (row.removeFromLeft (100));
        offset.setBounds (row);
    }

private:
    void apply()
    {
        engine.setClockOutputs (UiSettings::clockDevices);
        engine.setClockOffsetMs (UiSettings::clockOffsetMs);
        changed();
    }

    Engine& engine;
    std::function<void()> changed;
    juce::Label intro, listLabel, offsetLabel, none;
    std::vector<std::unique_ptr<juce::ToggleButton>> boxes;
    juce::Slider offset;
};
