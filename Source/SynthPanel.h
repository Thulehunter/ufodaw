#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "SynthPresets.h"
#include "Theme.h"

// The window of the built-in synth: every setting of one track's synth, and the ready-made sounds. Changes apply at once (they are heard while a
// note plays); each drag is one undo step.
class SynthPanel : public juce::Component, private juce::Timer
{
public:
    SynthPanel (Track& t, std::function<void()> beginEdit, std::function<void()> changed)
        : track (t), begin (std::move (beginEdit)), changedFn (std::move (changed))
    {
        presets.setTextWhenNothingSelected ("Sounds...");
        int id = 1;
        const char* lastCategory = "";
        for (auto& p : SynthPresets::all())
        {
            if (std::string (lastCategory) != p.category)
            {
                if (id > 1)
                    presets.addSeparator();
                presets.addSectionHeading (p.category);
                lastCategory = p.category;
            }
            presets.addItem (p.name, id++);
        }
        presets.onChange = [this]
        {
            const int index = presets.getSelectedId() - 1;
            presets.setSelectedId (0, juce::dontSendNotification);
            if (index < 0 || index >= (int) SynthPresets::all().size())
                return;
            begin();
            track.synth.apply (SynthPresets::all()[(size_t) index].values);
            refresh();
            changedFn();
        };
        addAndMakeVisible (presets);

        // oscillators
        section ("ENGINE");
        choiceCombo ("Sound engine", track.synth.engine, { "Synth", "Piano" });
        section ("OSCILLATOR 1");
        waveCombo (wave1, "Wave", track.synth.waveform);
        floatSlider ("pulseWidth");
        intSlider ("Unison voices", track.synth.unison, 1, 5);
        floatSlider ("unisonDetune");
        floatSlider ("spread");
        section ("OSCILLATOR 2 AND SUB");
        waveCombo (wave2, "Wave", track.synth.osc2Wave);
        floatSlider ("osc2Mix");
        floatSlider ("osc2Semi", 1.0);
        floatSlider ("osc2Detune");
        floatSlider ("subLevel");
        floatSlider ("pitchEnv", 1.0);
        floatSlider ("pitchEnvTime");
        nextColumn();

        section ("FILTER");
        floatSlider ("cutoff", 0.0, 2000.0);
        floatSlider ("resonance");
        floatSlider ("drive");
        floatSlider ("keyTrack");
        floatSlider ("filterEnv");
        section ("FILTER ENVELOPE");
        floatSlider ("fAttack", 0.0, 0.1);
        floatSlider ("fDecay", 0.0, 0.3);
        floatSlider ("fSustain");
        floatSlider ("fRelease", 0.0, 0.4);
        section ("PIANO (ENGINE: PIANO)");
        floatSlider ("pianoDecay");
        floatSlider ("pianoBright");
        floatSlider ("pianoDetune");
        floatSlider ("pianoHammer");
        nextColumn();

        section ("AMPLIFIER");
        floatSlider ("attack", 0.0, 0.1);
        floatSlider ("decay", 0.0, 0.3);
        floatSlider ("sustain");
        floatSlider ("release", 0.0, 0.4);
        floatSlider ("glide", 0.0, 0.1);
        choiceCombo ("Voices", track.synth.mono, { "Polyphonic", "Mono, legato" });
        floatSlider ("level");
        section ("LFO");
        {
            auto* label = new juce::Label ({}, "Shape");
            auto* box = new juce::ComboBox();
            box->addItemList ({ "Sine", "Triangle", "Square", "Random steps" }, 1);
            box->setSelectedId (track.synth.lfoShape.load() + 1, juce::dontSendNotification);
            box->onChange = [this, box] { begin(); track.synth.lfoShape = juce::jmax (0, box->getSelectedId() - 1); changedFn(); };
            addRow (label, box);
            lfoShape = box;   // (owned by ownedComponents)
        }
        floatSlider ("lfoRate", 0.0, 2.0);
        floatSlider ("lfoPitch");
        floatSlider ("lfoCutoff");
        floatSlider ("lfoAmp");
        floatSlider ("lfoDelay");

        setSize (columnWidth * 3 + 28, 580);
        startTimerHz (4);
    }

    ~SynthPanel() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        for (auto& h : headings)
            g.drawText (h.text, h.bounds, juce::Justification::bottomLeft);
        g.setColour (Theme::gridLine);
        for (auto& h : headings)
            g.fillRect (h.bounds.getX(), h.bounds.getBottom() + 1, h.bounds.getWidth(), 1);
    }

    void resized() override
    {
        presets.setBounds (getWidth() - 190, 8, 180, 26);
        for (auto& r : rows)
        {
            r.label->setBounds (r.x, r.y, 104, rowHeight - 4);
            r.control->setBounds (r.x + 106, r.y, columnWidth - 116, rowHeight - 4);
        }
    }

    // reads every control from the synth again (after a preset, undo, or automation)
    void refresh()
    {
        for (auto& f : floats)
            f.slider->setValue ((double) (track.synth.*(f.info->member)).load(), juce::dontSendNotification);
        for (auto& i : ints)
            i.slider->setValue (i.target->load(), juce::dontSendNotification);
        for (auto& w : waves)
            w.box->setSelectedId (w.target->load() + 1, juce::dontSendNotification);
        if (lfoShape != nullptr)
            lfoShape->setSelectedId (track.synth.lfoShape.load() + 1, juce::dontSendNotification);
    }

    const Track* getTrack() const { return &track; }

private:
    static constexpr int columnWidth = 300, rowHeight = 30, top = 42;

    struct Row { juce::Component* label; juce::Component* control; int x, y; };
    struct Heading { juce::String text; juce::Rectangle<int> bounds; };
    struct FloatControl { const SynthParams::FloatInfo* info; juce::Slider* slider; };
    struct IntControl { std::atomic<int>* target; juce::Slider* slider; };
    struct WaveControl { std::atomic<int>* target; juce::ComboBox* box; };

    void section (const juce::String& title)
    {
        if (y > top)
            y += 8;
        headings.push_back ({ title, { x, y, columnWidth - 10, 20 } });
        y += 26;
    }

    void nextColumn() { x += columnWidth; y = top; }

    void addRow (juce::Component* label, juce::Component* control)
    {
        ownedComponents.add (label);
        ownedComponents.add (control);
        addAndMakeVisible (label);
        addAndMakeVisible (control);
        rows.push_back ({ label, control, x, y });
        y += rowHeight;
    }

    void choiceCombo (const juce::String& name, std::atomic<int>& target, const juce::StringArray& items)
    {
        auto* label = new juce::Label ({}, name);
        auto* box = new juce::ComboBox();
        box->addItemList (items, 1);
        box->setSelectedId (target.load() + 1, juce::dontSendNotification);
        auto* t = &target;
        box->onChange = [this, box, t] { begin(); *t = juce::jmax (0, box->getSelectedId() - 1); changedFn(); };
        addRow (label, box);
        waves.push_back ({ &target, box });
    }

    void waveCombo (std::unique_ptr<juce::ComboBox>&, const juce::String& name, std::atomic<int>& target)
    {
        choiceCombo (name, target, { "Sine", "Saw", "Square", "Triangle" });
    }

    void intSlider (const juce::String& name, std::atomic<int>& target, int lo, int hi)
    {
        auto* label = new juce::Label ({}, name);
        auto* s = new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
        s->setRange (lo, hi, 1);
        s->setValue (target.load(), juce::dontSendNotification);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 20);
        auto* t = &target;
        s->onDragStart = [this] { begin(); };
        s->onValueChange = [this, s, t] { *t = (int) s->getValue(); changedFn(); };
        addRow (label, s);
        ints.push_back ({ &target, s });
    }

    // `skewMid` 0 = linear; `step` > 0 snaps (semitones)
    void floatSlider (const char* name, double step = 0.0, double skewMid = 0.0)
    {
        const SynthParams::FloatInfo* info = nullptr;
        for (auto& f : SynthParams::floatInfo())
            if (juce::String (f.name) == name)
                info = &f;
        jassert (info != nullptr);
        if (info == nullptr)
            return;

        // (the second number of the older calls was a skew mid-point; a value below the range's start means none)
        const double mid = skewMid;
        auto* label = new juce::Label ({}, info->label);
        auto* s = new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
        s->setRange (info->lo, info->hi, step);
        if (mid > info->lo && mid < info->hi)
            s->setSkewFactorFromMidPoint (mid);
        else if (step == 0.0 && info->lo >= 0.0 && (double) info->hi / juce::jmax (1.0e-6, (double) info->lo) > 40.0 && info->lo > 0.0)
            s->setSkewFactorFromMidPoint (std::sqrt (info->lo * info->hi) * 0.5);   // (times and frequencies: more room for the small values)
        s->setValue ((double) (track.synth.*(info->member)).load(), juce::dontSendNotification);
        s->setNumDecimalPlacesToDisplay (info->hi > 100.0 ? 0 : info->hi > 10.0 ? 1 : 2);
        s->setDoubleClickReturnValue (true, (double) info->def);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 20);
        s->onDragStart = [this] { begin(); };
        s->onValueChange = [this, s, info] { (track.synth.*(info->member)) = (float) s->getValue(); changedFn(); };
        addRow (label, s);
        floats.push_back ({ info, s });
    }

    void timerCallback() override
    {
        // keep the controls in step with the synth (undo, automation) unless one is being dragged
        for (auto& f : floats)
            if (f.slider->isMouseButtonDown())
                return;
        refresh();
    }

    Track& track;
    std::function<void()> begin, changedFn;
    juce::ComboBox presets;
    std::unique_ptr<juce::ComboBox> wave1, wave2;   // (unused holders)
    juce::ComboBox* lfoShape = nullptr;               // (a child that ownedComponents owns: never delete it here)
    juce::OwnedArray<juce::Component> ownedComponents;
    std::vector<Row> rows;
    std::vector<Heading> headings;
    std::vector<FloatControl> floats;
    std::vector<IntControl> ints;
    std::vector<WaveControl> waves;
    int x = 10, y = top;
};

class SynthWindow : public juce::DocumentWindow
{
public:
    SynthWindow (SynthPanel* panel, const juce::String& trackName, std::function<void()> onClosed)
        : juce::DocumentWindow ("Synth - " + trackName, Theme::background, juce::DocumentWindow::closeButton), closed (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (panel, true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { if (closed) closed(); }

private:
    std::function<void()> closed;
};
