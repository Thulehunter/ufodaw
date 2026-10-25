#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "SynthPresets.h"
#include "Theme.h"

#include "Wavetable.h"

// The table the wavetable engine reads, as a stack of waves: the first wave in front, the last at the back, and the wave at the current position in orange.
// A strip on the right shows the position and how far the LFO, the envelope and the velocity can move it. Click or drag to set the position.
class WavetableView : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    WavetableView (const SynthParams& p, std::function<void()> start, std::function<void (float)> setPosition)
        : params (p), onStart (std::move (start)), onPosition (std::move (setPosition))
    {
        setTooltip ("The table as a stack of waves (the first in front). Click or drag to set the position; the bar on the right shows how far the LFO, envelope and velocity move it.");
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        startTimerHz (12);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Theme::laneBg);
        g.fillRoundedRectangle (r, 3.0f);

        const int set = juce::jlimit (0, Wavetable::numSets - 1, params.wtTable.load());
        const float pos = juce::jlimit (0.0f, 1.0f, params.wtPos.load());
        const auto plot = plotArea();
        const float range = plot.getHeight() - 2.0f * amplitude (plot);
        const float base0 = plot.getBottom() - amplitude (plot);
        auto& bank = Wavetable::bank();

        // one wave as a line (filled below with the background so the waves in front hide those behind)
        auto drawWave = [&] (double framePos, bool current)
        {
            const int f0 = juce::jmin ((int) framePos, Wavetable::numFrames - 2);
            const float frac = (float) (framePos - f0);
            const float* a = bank.frame (set, f0, 0);
            const float* b = bank.frame (set, f0 + 1, 0);
            const float baseline = base0 - (float) (framePos / (Wavetable::numFrames - 1)) * range;
            const int points = juce::jmax (2, (int) plot.getWidth() / 2);
            juce::Path line;
            for (int i = 0; i < points; ++i)
            {
                const float x = plot.getX() + plot.getWidth() * (float) i / (float) (points - 1);
                const float y = baseline - Wavetable::read (a, b, frac, (double) i / (points - 1)) * amplitude (plot);
                if (i == 0) line.startNewSubPath (x, y); else line.lineTo (x, y);
            }
            juce::Path area (line);
            area.lineTo (plot.getRight(), baseline + 3.0f);
            area.lineTo (plot.getX(), baseline + 3.0f);
            area.closeSubPath();
            g.setColour (Theme::laneBg);
            g.fillPath (area);
            g.setColour (current ? Theme::orange : Theme::textDim.withAlpha (0.55f));
            g.strokePath (line, juce::PathStrokeType (current ? 2.0f : 1.0f));
        };

        constexpr int slots = 16;
        const double current = (double) pos * (Wavetable::numFrames - 1);
        bool drawnCurrent = false;
        for (int i = slots - 1; i >= 0; --i)
        {
            const double fp = (double) i * (Wavetable::numFrames - 1) / (slots - 1);
            if (! drawnCurrent && current >= fp)
            {
                drawWave (current, true);
                drawnCurrent = true;
            }
            drawWave (fp, false);
        }
        if (! drawnCurrent)
            drawWave (current, true);

        // the position strip: the range the modulation reaches (light) and the position (line)
        const auto strip = stripArea();
        g.setColour (Theme::gridLine);
        g.fillRoundedRectangle (strip, 2.0f);
        const float lfo = 0.5f * params.wtLfo.load(), env = params.wtEnv.load(), vel = 0.5f * std::abs (params.wtVel.load());
        const float lo = juce::jlimit (0.0f, 1.0f, pos - lfo - vel + juce::jmin (0.0f, env)), hi = juce::jlimit (0.0f, 1.0f, pos + lfo + vel + juce::jmax (0.0f, env));
        auto yOf = [&] (float p) { return base0 - p * range; };
        if (hi - lo > 0.001f)
        {
            g.setColour (Theme::orange.withAlpha (0.35f));
            g.fillRoundedRectangle (strip.getX(), yOf (hi), strip.getWidth(), yOf (lo) - yOf (hi), 2.0f);
        }
        g.setColour (Theme::orange);
        g.fillRect (strip.getX() - 2.0f, yOf (pos) - 1.0f, strip.getWidth() + 4.0f, 2.0f);

        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("Wave " + juce::String (juce::roundToInt (pos * (Wavetable::numFrames - 1)) + 1) + " / " + juce::String (Wavetable::numFrames),
                    getLocalBounds().reduced (6, 2), juce::Justification::topLeft);
    }

    void mouseDown (const juce::MouseEvent& e) override { onStart(); setFrom (e); }
    void mouseDrag (const juce::MouseEvent& e) override { setFrom (e); }

private:
    static float amplitude (juce::Rectangle<float> plot) { return plot.getHeight() * 0.15f; }
    juce::Rectangle<float> plotArea() const { return getLocalBounds().toFloat().reduced (6.0f, 6.0f).withTrimmedRight (16.0f); }
    juce::Rectangle<float> stripArea() const
    {
        const auto plot = plotArea();
        const float pad = amplitude (plot);
        return { getWidth() - 14.0f, plot.getY() + pad, 6.0f, plot.getHeight() - 2.0f * pad };
    }

    void setFrom (const juce::MouseEvent& e)
    {
        const auto strip = stripArea();
        const float p = juce::jlimit (0.0f, 1.0f, (strip.getBottom() - (float) e.y) / strip.getHeight());
        onPosition (p);
        repaint();
    }

    void timerCallback() override
    {
        const float sig = (float) params.wtTable.load() * 1000.0f + params.wtPos.load() * 100.0f + params.wtLfo.load() * 10.0f + params.wtEnv.load() * 3.0f + params.wtVel.load() * 7.0f;
        if (sig != lastSignature)
        {
            lastSignature = sig;
            repaint();
        }
    }

    const SynthParams& params;
    std::function<void()> onStart;
    std::function<void (float)> onPosition;
    float lastSignature = -1.0e9f;
};

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

        myButton.setTooltip ("Your own sounds: save the current settings under a name, load them again later, in any project");
        myButton.onClick = [this] { showMySounds(); };
        addAndMakeVisible (myButton);

        // oscillators
        section ("ENGINE");
        choiceCombo ("Sound engine", track.synth.engine, { "Synth", "Piano", "Wavetable" });
        section ("OSCILLATOR 1");
        group = &synthOnly;
        waveCombo (wave1, "Wave", track.synth.waveform);
        floatSlider ("pulseWidth");
        group = nullptr;
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
        group = &pianoOnly;
        floatSlider ("pianoDecay");
        floatSlider ("pianoBright");
        floatSlider ("pianoDetune");
        floatSlider ("pianoHammer");
        group = nullptr;
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
        nextColumn();

        section ("WAVETABLE (ENGINE: WAVETABLE)");
        group = &wavetableOnly;
        {
            juce::StringArray names;
            for (int i = 0; i < Wavetable::numSets; ++i)
                names.add (Wavetable::setName (i));
            choiceCombo ("Table", track.synth.wtTable, names);
        }
        floatSlider ("wtPos");
        floatSlider ("wtLfo");
        floatSlider ("wtEnv");
        floatSlider ("wtVel");
        addRow (nullptr, new WavetableView (track.synth, [this] { begin(); }, [this] (float p) { track.synth.wtPos = p; refresh(); changedFn(); }), 170, true);
        group = nullptr;

        setSize (columnWidth * 4 + 28, 700);
        updateDimming();
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
        myButton.setBounds (getWidth() - 190 - 130, 8, 120, 26);
        for (auto& r : rows)
        {
            if (r.full)
                r.control->setBounds (r.x, r.y, columnWidth - 12, r.height - 4);
            else
            {
                r.label->setBounds (r.x, r.y, 120, r.height - 4);
                r.control->setBounds (r.x + 122, r.y, columnWidth - 132, r.height - 4);
            }
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
        updateDimming();
    }

    const Track* getTrack() const { return &track; }

    // ---- your own sounds: one small file per sound in %APPDATA%\UFODAW\presets\synth, usable in every project
    static juce::File userFolder()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("UFODAW").getChildFile ("presets").getChildFile ("synth");
        dir.createDirectory();
        return dir;
    }

    static juce::StringArray userNames()
    {
        juce::StringArray names;
        for (auto& f : userFolder().findChildFiles (juce::File::findFiles, false, "*.json"))
            names.add (f.getFileNameWithoutExtension());
        names.sort (true);
        return names;
    }

    static bool saveUser (const juce::String& name, const SynthValues& v)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("format", 1);
       #define UFODAW_X(n, def, lo, hi, label) o->setProperty (#n, (double) v.n);
        UFODAW_SYNTH_FLOATS (UFODAW_X)
       #undef UFODAW_X
       #define UFODAW_X(n, def) o->setProperty (#n, v.n);
        UFODAW_SYNTH_INTS (UFODAW_X)
       #undef UFODAW_X
        return userFolder().getChildFile (name + ".json").replaceWithText (juce::JSON::toString (juce::var (o)));
    }

    static bool loadUser (const juce::String& name, SynthValues& v)
    {
        const auto parsed = juce::JSON::parse (userFolder().getChildFile (name + ".json"));
        if (! parsed.isObject())
            return false;
       #define UFODAW_X(n, def, lo, hi, label) v.n = juce::jlimit ((float) (lo), (float) (hi), (float) (double) parsed.getProperty (#n, (double) (def)));
        UFODAW_SYNTH_FLOATS (UFODAW_X)
       #undef UFODAW_X
       #define UFODAW_X(n, def) v.n = (int) parsed.getProperty (#n, def);
        UFODAW_SYNTH_INTS (UFODAW_X)
       #undef UFODAW_X
        v.unison = juce::jlimit (1, 5, v.unison);
        v.waveform = juce::jlimit (0, 3, v.waveform);
        v.osc2Wave = juce::jlimit (0, 3, v.osc2Wave);
        v.lfoShape = juce::jlimit (0, 3, v.lfoShape);
        v.engine = juce::jlimit (0, 2, v.engine);
        v.wtTable = juce::jlimit (0, Wavetable::numSets - 1, v.wtTable);
        v.mono = juce::jlimit (0, 1, v.mono);
        return true;
    }

private:
    static constexpr int columnWidth = 300, rowHeight = 30, top = 42;

    struct Row { juce::Component* label; juce::Component* control; int x, y, height; bool full; };
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

    // `full`: no label, the control takes the whole column width (the wave display)
    void addRow (juce::Component* label, juce::Component* control, int height = rowHeight, bool full = false)
    {
        for (auto* c : { label, control })
            if (c != nullptr)
            {
                ownedComponents.add (c);
                addAndMakeVisible (c);
                if (group != nullptr)
                    group->push_back (c);
            }
        rows.push_back ({ label, control, x, y, height, full });
        y += height;
    }

    // the controls of an engine that is not in use are shown dimmer (they still work, so a sound can be set up before switching)
    void updateDimming()
    {
        const int engine = track.synth.engine.load();
        auto apply = [] (const std::vector<juce::Component*>& list, bool used) { for (auto* c : list) c->setAlpha (used ? 1.0f : 0.4f); };
        apply (synthOnly, engine == 0);
        apply (pianoOnly, engine == 1);
        apply (wavetableOnly, engine == 2);
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

    void showMySounds()
    {
        const auto names = userNames();
        juce::PopupMenu menu;
        menu.addItem (1, "Save the current sound...");
        menu.addSeparator();
        if (names.isEmpty())
            menu.addItem (2, "(nothing saved yet)", false);
        for (int i = 0; i < names.size(); ++i)
            menu.addItem (100 + i, names[i]);
        if (! names.isEmpty())
        {
            juce::PopupMenu remove;
            for (int i = 0; i < names.size(); ++i)
                remove.addItem (1000 + i, names[i]);
            menu.addSeparator();
            menu.addSubMenu ("Delete a sound", remove);
        }

        juce::Component::SafePointer<SynthPanel> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&myButton), [safe, names] (int result)
        {
            if (safe == nullptr || result == 0)
                return;
            if (result == 1)   // (a moment later: the menu is still closing, and a dialog opened inside its callback would stay blocked)
                juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->askNameAndSave(); });
            else if (result >= 1000 && result - 1000 < names.size())
                safe->userFolder().getChildFile (names[result - 1000] + ".json").deleteFile();
            else if (result >= 100 && result - 100 < names.size())
            {
                SynthValues v;
                if (loadUser (names[result - 100], v))
                {
                    safe->begin();
                    safe->track.synth.apply (v);
                    safe->refresh();
                    safe->changedFn();
                }
            }
        });
    }

    void askNameAndSave()
    {
        juce::Component::SafePointer<SynthPanel> safe (this);
        auto* box = new juce::AlertWindow ("Save a sound", "A name for this sound (it is kept for every project, under My sounds):", juce::MessageBoxIconType::NoIcon);
        box->addTextEditor ("name", "My sound");
        box->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        box->enterModalState (true, juce::ModalCallbackFunction::create ([safe, box] (int ok)
        {
            if (ok != 1 || safe == nullptr)
                return;
            const auto name = juce::File::createLegalFileName (box->getTextEditorContents ("name").trim());
            if (name.isNotEmpty())
                saveUser (name, safe->track.synth.capture());
        }), true);   // (true: the dialog deletes itself when it is closed)
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
    juce::TextButton myButton { "My sounds..." };
    std::unique_ptr<juce::ComboBox> wave1, wave2;   // (unused holders)
    juce::ComboBox* lfoShape = nullptr;               // (a child that ownedComponents owns: never delete it here)
    juce::OwnedArray<juce::Component> ownedComponents;
    std::vector<Row> rows;
    std::vector<Heading> headings;
    std::vector<FloatControl> floats;
    std::vector<IntControl> ints;
    std::vector<WaveControl> waves;
    std::vector<juce::Component*> synthOnly, pianoOnly, wavetableOnly;
    std::vector<juce::Component*>* group = nullptr;   // the controls added now belong to this engine
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
