#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "Theme.h"

// Quantize, swing and humanize for the notes of a MIDI clip.
struct QuantizeSettings
{
    double gridSeconds = 0.25;       // one grid step
    float strength = 1.0f;           // 0..1: how far notes move toward the grid (0 = not at all, 1 = exactly on it)
    float swing = 0.0f;              // 0..1: delay of every second step; 1 = triplet feel (a third of a step)
    double humanizeSeconds = 0.0;    // each note moves by a random amount up to this, earlier or later
    float humanizeVelocity = 0.0f;   // each note's velocity changes by a random amount up to this (0..1 scale)
    bool quantizeLength = false;     // note lengths are pulled to the grid as well
};

// Notes are placed relative to the clip; the grid is anchored to the timeline (bar 1 = 0 s), so a clip that does not start on
// the grid still lands on it. Result: the notes moved, clamped inside the clip.
inline void quantizeNotes (std::vector<Note>& notes, double clipStart, double clipLength, const QuantizeSettings& s, juce::Random& rng)
{
    const double grid = juce::jmax (1.0e-3, s.gridSeconds);
    for (auto& n : notes)
    {
        const double absolute = clipStart + n.start;
        const auto step = (juce::int64) std::llround (absolute / grid);
        double target = (double) step * grid;
        if ((step & 1) != 0)
            target += (double) s.swing * grid / 3.0;   // every second step comes late

        double moved = absolute + (target - absolute) * (double) s.strength;
        if (s.humanizeSeconds > 0.0)
            moved += (rng.nextDouble() * 2.0 - 1.0) * s.humanizeSeconds;

        if (s.quantizeLength)
        {
            const double wanted = juce::jmax (grid, std::round (n.length / grid) * grid);
            n.length += (wanted - n.length) * (double) s.strength;
        }

        n.start = juce::jlimit (0.0, juce::jmax (0.0, clipLength - 0.01), moved - clipStart);
        n.length = juce::jlimit (0.02, juce::jmax (0.02, clipLength - n.start), n.length);

        if (s.humanizeVelocity > 0.0f)
            n.velocity = juce::jlimit (0.02f, 1.0f, n.velocity + (rng.nextFloat() * 2.0f - 1.0f) * s.humanizeVelocity);
    }
}

// The controls: pick the grid, how strong, how much swing, how loose. Apply as often as you like; every Apply starts from the
// original notes (so humanize does not pile up), and the first Apply is one undo step.
class QuantizePanel : public juce::Component
{
public:
    QuantizePanel (double beatSeconds, std::function<void (const QuantizeSettings&)> onApply) : beat (beatSeconds), apply (std::move (onApply))
    {
        auto setupLabel = [this] (juce::Label& l, const juce::String& text)
        {
            l.setText (text, juce::dontSendNotification);
            l.setColour (juce::Label::textColourId, Theme::textDim);
            l.setFont (juce::FontOptions (13.0f));
            addAndMakeVisible (l);
        };
        setupLabel (gridLabel, "Grid");
        setupLabel (strengthLabel, "Strength");
        setupLabel (swingLabel, "Swing");
        setupLabel (humTimeLabel, "Humanize time");
        setupLabel (humVelLabel, "Humanize velocity");

        grid.addItem ("1/4", 1);
        grid.addItem ("1/8", 2);
        grid.addItem ("1/8 triplet", 3);
        grid.addItem ("1/16", 4);
        grid.addItem ("1/16 triplet", 5);
        grid.addItem ("1/32", 6);
        grid.setSelectedId (lastGrid(), juce::dontSendNotification);
        grid.onChange = [] {};
        addAndMakeVisible (grid);

        auto setupSlider = [this] (juce::Slider& s, double lo, double hi, double v, const juce::String& suffix)
        {
            s.setSliderStyle (juce::Slider::LinearHorizontal);
            s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
            s.setRange (lo, hi, 1.0);
            s.setValue (v, juce::dontSendNotification);
            s.setTextValueSuffix (suffix);
            addAndMakeVisible (s);
        };
        setupSlider (strength, 0, 100, lastStrength(), " %");
        setupSlider (swing, 0, 100, lastSwing(), " %");
        setupSlider (humTime, 0, 40, lastHumTime(), " ms");
        setupSlider (humVel, 0, 40, lastHumVel(), " %");
        swing.setTooltip ("Delays every second step of the grid. 100% = triplet feel. Use 1/8 or 1/16 as the grid.");
        strength.setTooltip ("100% puts notes exactly on the grid; lower keeps some of the original feel");
        humTime.setTooltip ("Each note moves a random amount up to this (milliseconds), earlier or later");
        humVel.setTooltip ("Each note's velocity changes a random amount up to this");

        lengths.setButtonText ("Quantize note lengths too");
        lengths.setToggleState (lastLengths(), juce::dontSendNotification);
        addAndMakeVisible (lengths);

        applyButton.onClick = [this] { remember(); apply (settings()); };
        addAndMakeVisible (applyButton);

        setSize (340, 232);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        auto row = [&] (juce::Label& l, juce::Component& c)
        {
            auto line = r.removeFromTop (28);
            l.setBounds (line.removeFromLeft (120));
            c.setBounds (line);
            r.removeFromTop (4);
        };
        row (gridLabel, grid);
        row (strengthLabel, strength);
        row (swingLabel, swing);
        row (humTimeLabel, humTime);
        row (humVelLabel, humVel);
        lengths.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);
        applyButton.setBounds (r.removeFromTop (28).removeFromRight (120));
    }

private:
    QuantizeSettings settings() const
    {
        QuantizeSettings s;
        const double steps[] = { 1.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125 };   // in beats
        s.gridSeconds = beat * steps[juce::jlimit (0, 5, grid.getSelectedId() - 1)];
        s.strength = (float) strength.getValue() / 100.0f;
        s.swing = (float) swing.getValue() / 100.0f;
        s.humanizeSeconds = humTime.getValue() / 1000.0;
        s.humanizeVelocity = (float) humVel.getValue() / 100.0f;
        s.quantizeLength = lengths.getToggleState();
        return s;
    }

    // the last used values come back the next time the panel opens
    struct Memory { int grid = 4; double strength = 100, swing = 0, humTime = 0, humVel = 0; bool lengths = false; };
    static Memory& memory() { static Memory m; return m; }
    static int lastGrid() { return memory().grid; }
    static double lastStrength() { return memory().strength; }
    static double lastSwing() { return memory().swing; }
    static double lastHumTime() { return memory().humTime; }
    static double lastHumVel() { return memory().humVel; }
    static bool lastLengths() { return memory().lengths; }
    void remember() const
    {
        auto& m = memory();
        m.grid = grid.getSelectedId();
        m.strength = strength.getValue();
        m.swing = swing.getValue();
        m.humTime = humTime.getValue();
        m.humVel = humVel.getValue();
        m.lengths = lengths.getToggleState();
    }

    double beat;
    std::function<void (const QuantizeSettings&)> apply;
    juce::Label gridLabel, strengthLabel, swingLabel, humTimeLabel, humVelLabel;
    juce::ComboBox grid;
    juce::Slider strength, swing, humTime, humVel;
    juce::ToggleButton lengths;
    juce::TextButton applyButton { "Apply" };
};
