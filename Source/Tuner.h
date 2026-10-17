#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"

// Pitch of a block of samples in Hz (YIN), or 0 when there is no clear pitch. `rms` gets the signal level.
float detectPitchHz (const float* samples, int numSamples, double sampleRate, float& rms);

// A chromatic tuner for the audio input: note name, how many cents sharp/flat, and a needle.
// It listens to one input channel of the interface (the engine keeps the latest samples of it while the tuner is open).
class TunerPanel : public juce::Component, private juce::Timer
{
public:
    // `inputNames` are the enabled input channels of the device, `firstInput` the one to start on.
    TunerPanel (Engine&, const juce::StringArray& inputNames, int firstInput);
    ~TunerPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    Engine& engine;
    juce::ComboBox inputBox;
    juce::TextButton refDown { "-" }, refUp { "+" };
    int refHz = 440;

    float shownCents = 0.0f;      // smoothed needle
    float hz = 0.0f;
    int midiNote = -1;
    int holdFrames = 0;           // keeps the last note visible for a moment when the string dies away
    float level = 0.0f;
};

// The floating window around the panel.
class TunerWindow : public juce::DocumentWindow
{
public:
    TunerWindow (Engine&, const juce::StringArray& inputNames, int firstInput, std::function<void()> onClosed);
    ~TunerWindow() override;
    void closeButtonPressed() override;

private:
    Engine& engine;
    std::function<void()> closed;
};
