#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "DrumKit.h"
#include "Theme.h"

// The drum rack's window: a 4x4 grid of pads (pad 1 bottom left, like a drum machine). Click a pad to hear it and select it; below
// are the selected pad's level, pan, mute, solo, choke group and sample. "Edit sampler..." opens the pad's Simpler.
// Dropping an audio file on a pad loads it there.
class DrumRackPanel : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    struct Hooks
    {
        std::function<void (int note, bool down)> preview;     // hear a pad
        std::function<bool (int pad, const juce::File&)> load; // load a sample into a pad
        std::function<void (int pad)> reset;                   // back to the built-in sound
        std::function<void (int pad)> editSampler;             // open the pad's Simpler
        std::function<bool (int pad, int layer, const juce::File&)> loadLayer;   // an extra sample of a pad (layer 0..2)
        std::function<void (int pad, int layer)> clearLayer;
        std::function<void()> changed;                         // something was edited
        std::function<void()> beginEdit;                       // called before every change (the undo snapshot)
    };

    DrumRackPanel (DrumKit& k, Hooks h) : kit (k), hooks (std::move (h))
    {
        auto label = [this] (juce::Label& l, const juce::String& text)
        {
            l.setText (text, juce::dontSendNotification);
            l.setColour (juce::Label::textColourId, Theme::textDim);
            addAndMakeVisible (l);
        };
        label (gainLabel, "Level");
        label (panLabel, "Pan");
        label (chokeLabel, "Choke");

        nameLabel.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        nameLabel.setColour (juce::Label::textColourId, Theme::text);
        addAndMakeVisible (nameLabel);
        fileLabel.setColour (juce::Label::textColourId, Theme::textFaint);
        fileLabel.setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (fileLabel);

        for (auto* s : { &gain, &pan })
        {
            s->setSliderStyle (juce::Slider::LinearHorizontal);
            s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
            addAndMakeVisible (*s);
        }
        gain.setRange (0.0, 1.5);
        gain.setNumDecimalPlacesToDisplay (2);
        gain.setDoubleClickReturnValue (true, 1.0);
        pan.setRange (-1.0, 1.0);
        pan.setNumDecimalPlacesToDisplay (2);
        pan.setDoubleClickReturnValue (true, 0.0);
        gain.onDragStart = [this] { begin(); };
        pan.onDragStart = [this] { begin(); };
        gain.onValueChange = [this] { if (! gain.isMouseButtonDown()) begin(); kit.getPad (selected).gain = (float) gain.getValue(); edited(); };
        pan.onValueChange  = [this] { if (! pan.isMouseButtonDown()) begin(); kit.getPad (selected).pan = (float) pan.getValue(); edited(); };

        muteButton.onClick = [this] { begin(); kit.getPad (selected).mute = muteButton.getToggleState(); edited(); repaint(); };
        soloButton.onClick = [this] { begin(); kit.getPad (selected).solo = soloButton.getToggleState(); edited(); repaint(); };
        addAndMakeVisible (muteButton);
        addAndMakeVisible (soloButton);

        chokeBox.addItem ("None", 1);
        for (int i = 1; i <= 8; ++i)
            chokeBox.addItem ("Group " + juce::String (i), i + 1);
        chokeBox.setTooltip ("Pads in the same group cut each other off (closed hat / open hat)");
        chokeBox.onChange = [this] { begin(); kit.getPad (selected).chokeGroup = chokeBox.getSelectedId() - 1; edited(); repaint(); };
        addAndMakeVisible (chokeBox);

        loadButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Choose a sample for this pad", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
            const int pad = selected;
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this, pad] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) loadInto (pad, fc.getResult()); });
        };
        resetButton.onClick = [this] { begin(); if (hooks.reset) hooks.reset (selected); edited(); refreshControls(); repaint(); };
        editButton.onClick = [this] { if (hooks.editSampler) hooks.editSampler (selected); };
        for (auto* b : { &loadButton, &resetButton, &editButton })
            addAndMakeVisible (*b);

        // Velocity layers / round-robin: the pad's own sample and up to three more, each with the velocity range it answers to. Samples whose
        // ranges overlap take turns (round-robin); ranges side by side make a soft / hard pair.
        layersLabel.setText ("Samples and velocity ranges (overlapping ranges take turns, round-robin)", juce::dontSendNotification);
        layersLabel.setColour (juce::Label::textColourId, Theme::textDim);
        layersLabel.setFont (juce::FontOptions (12.5f));
        addAndMakeVisible (layersLabel);
        for (int i = 0; i < DrumKit::maxLayers + 1; ++i)
        {
            auto& row = layerRows[(size_t) i];
            row.slider.setSliderStyle (juce::Slider::TwoValueHorizontal);
            row.slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            row.slider.setRange (0.0, 127.0, 1.0);
            row.slider.setMinAndMaxValues (0.0, 127.0, juce::dontSendNotification);
            row.slider.setTooltip ("The velocities (1-127) this sample plays for");
            row.slider.onDragStart = [this] { begin(); };
            row.slider.onValueChange = [this, i]
            {
                auto& r = layerRows[(size_t) i];
                const float lo = (float) r.slider.getMinValue() / 127.0f, hi = (float) r.slider.getMaxValue() / 127.0f;
                if (i == 0) { kit.getPad (selected).velLo = lo; kit.getPad (selected).velHi = hi; }
                else        { kit.getPad (selected).layers[(size_t) i - 1].velLo = lo; kit.getPad (selected).layers[(size_t) i - 1].velHi = hi; }
                rangeText (i);
                edited();
            };
            addAndMakeVisible (row.slider);
            row.range.setFont (juce::FontOptions (12.0f));
            row.range.setColour (juce::Label::textColourId, Theme::textDim);
            row.range.setJustificationType (juce::Justification::centredRight);
            addAndMakeVisible (row.range);
            row.name.setFont (juce::FontOptions (13.0f));
            row.name.setColour (juce::Label::textColourId, Theme::text);
            addAndMakeVisible (row.name);
            if (i > 0)
            {
                row.add.onClick = [this, i]
                {
                    chooser = std::make_unique<juce::FileChooser> ("Choose a sample for this layer", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
                    const int pad = selected;
                    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                          [this, pad, i] (const juce::FileChooser& fc)
                                          {
                                              if (! fc.getResult().existsAsFile())
                                                  return;
                                              begin();
                                              if (hooks.loadLayer && hooks.loadLayer (pad, i - 1, fc.getResult()))
                                              {
                                                  edited();
                                                  refreshControls();
                                              }
                                              else
                                                  juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Cannot open sample", fc.getResult().getFullPathName());
                                          });
                };
                row.remove.setTooltip ("Remove this sample");
                row.remove.onClick = [this, i] { begin(); if (hooks.clearLayer) hooks.clearLayer (selected, i - 1); edited(); refreshControls(); };
                addAndMakeVisible (row.add);
                addAndMakeVisible (row.remove);
            }
        }

        hint.setText ("Click a pad, or play the keys Z X C V / A S D F / Q W E R / 1 2 3 4 (like the pads). Drop an audio file on a pad to load it. "
                      "Each pad is a small Simpler: Edit sampler... for start, "
                      "end, pitch, envelope and filter.", juce::dontSendNotification);
        hint.setFont (juce::FontOptions (12.0f));
        hint.setColour (juce::Label::textColourId, Theme::textFaint);
        addAndMakeVisible (hint);

        refreshControls();
        setWantsKeyboardFocus (true);
        setSize (520, 700);
        startTimerHz (15);   // lights the pads that are playing
    }

    ~DrumRackPanel() override { stopTimer(); }

    // The pads on the computer keyboard, laid out like the grid (the bottom row is pads 1-4): Z X C V / A S D F / Q W E R / 1 2 3 4
    static constexpr int padKeys[16] = { 'Z', 'X', 'C', 'V', 'A', 'S', 'D', 'F', 'Q', 'W', 'E', 'R', '1', '2', '3', '4' };

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k.getModifiers().isAnyModifierKeyDown())
            return false;   // (Ctrl+Z and friends belong to the window)
        for (int code : padKeys)
            if (k.getKeyCode() == code)
                return true;   // played in keyStateChanged()
        return false;
    }

    bool keyStateChanged (bool) override
    {
        const bool modifiers = juce::ModifierKeys::currentModifiers.isCtrlDown() || juce::ModifierKeys::currentModifiers.isAltDown()
                               || juce::ModifierKeys::currentModifiers.isCommandDown();
        bool used = false;
        for (int pad = 0; pad < DrumKit::numPads; ++pad)
        {
            const bool down = ! modifiers && juce::KeyPress::isKeyCurrentlyDown (padKeys[pad]);
            if (down != keyDown[(size_t) pad])
            {
                keyDown[(size_t) pad] = down;
                if (hooks.preview)
                    hooks.preview (kit.getPad (pad).note, down);
                repaint();
                used = true;
            }
        }
        return used;
    }

    void select (int pad) { selected = juce::jlimit (0, DrumKit::numPads - 1, pad); refreshControls(); repaint(); }
    int getSelected() const { return selected; }

    void refreshControls()
    {
        auto& p = kit.getPad (selected);
        nameLabel.setText ("Pad " + juce::String (selected + 1) + " - " + p.name + "  (" + juce::MidiMessage::getMidiNoteName (p.note, true, true, 3) + ")",
                           juce::dontSendNotification);
        fileLabel.setText (p.file != juce::File() ? p.file.getFileName() : (kit.hasSample (selected) ? juce::String ("built-in sound") : juce::String ("empty - load or drop a sample")),
                           juce::dontSendNotification);
        gain.setValue (p.gain.load(), juce::dontSendNotification);
        pan.setValue (p.pan.load(), juce::dontSendNotification);
        muteButton.setToggleState (p.mute.load(), juce::dontSendNotification);
        soloButton.setToggleState (p.solo.load(), juce::dontSendNotification);
        chokeBox.setSelectedId (p.chokeGroup.load() + 1, juce::dontSendNotification);
        resetButton.setEnabled (p.file != juce::File() || selected < 8);
        editButton.setEnabled (kit.hasSample (selected));

        for (int i = 0; i < DrumKit::maxLayers + 1; ++i)
        {
            auto& row = layerRows[(size_t) i];
            const bool used = i == 0 || p.layers[(size_t) i - 1].used.load();
            const float lo = i == 0 ? p.velLo.load() : p.layers[(size_t) i - 1].velLo.load();
            const float hi = i == 0 ? p.velHi.load() : p.layers[(size_t) i - 1].velHi.load();
            row.slider.setMinAndMaxValues (std::round (lo * 127.0f), std::round (hi * 127.0f), juce::dontSendNotification);
            row.slider.setEnabled (used && (i > 0 || kit.hasSample (selected)));
            row.slider.setVisible (used);
            row.range.setVisible (used);
            row.name.setVisible (used);
            if (i == 0)
                row.name.setText ("Sample 1 (the pad's own)", juce::dontSendNotification);
            else
            {
                row.add.setVisible (! used);
                row.remove.setVisible (used);
                row.add.setButtonText ("+ Add sample " + juce::String (i + 1) + "...");
                row.name.setText (used ? "Sample " + juce::String (i + 1) + ": " + p.layers[(size_t) i - 1].file.getFileName() : juce::String(), juce::dontSendNotification);
            }
            rangeText (i);
        }
        resized();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        for (int pad = 0; pad < DrumKit::numPads; ++pad)
        {
            const auto r = padBounds (pad).toFloat().reduced (3.0f);
            const auto& p = kit.getPad (pad);
            const bool has = kit.hasSample (pad);
            const float hue = 0.01f + 0.125f * (float) pad / (float) (DrumKit::numPads - 1);
            auto colour = juce::Colour::fromHSV (hue, 0.72f, 0.97f, 1.0f);
            if (p.mute.load())
                colour = colour.withSaturation (0.1f).withBrightness (0.45f);

            g.setColour (has ? colour.withAlpha (pad == pressed || keyDown[(size_t) pad] ? 0.95f : 0.38f) : Theme::laneBg);
            g.fillRoundedRectangle (r, 8.0f);
            g.setColour (pad == selected ? Theme::yellow : Theme::gridLineStrong);
            g.drawRoundedRectangle (r, 8.0f, pad == selected ? 2.5f : 1.0f);

            g.setColour (has ? Theme::text : Theme::textFaint);
            g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            g.drawText (p.name, r.reduced (6.0f, 4.0f).removeFromTop (22.0f), juce::Justification::centredLeft, true);
            g.setColour (Theme::textDim);
            g.setFont (11.0f);
            g.drawText (juce::MidiMessage::getMidiNoteName (p.note, true, true, 3), r.reduced (6.0f, 4.0f), juce::Justification::bottomLeft);

            g.setColour (Theme::textFaint);
            g.drawText (juce::String::charToString ((juce::juce_wchar) padKeys[pad]), r.reduced (6.0f, 4.0f), juce::Justification::topRight);
            g.setColour (Theme::textDim);
            juce::String tags;
            if (p.solo.load())        tags << "S ";
            if (p.mute.load())        tags << "M ";
            if (p.chokeGroup.load())  tags << "choke " << p.chokeGroup.load();
            g.drawText (tags, r.reduced (6.0f, 4.0f), juce::Justification::bottomRight);
        }
    }

    void resized() override
    {
        gridArea = { 12, 12, getWidth() - 24, 330 };
        auto r = getLocalBounds().reduced (12);
        r.removeFromTop (330 + 10);

        nameLabel.setBounds (r.removeFromTop (24));
        fileLabel.setBounds (r.removeFromTop (20));
        r.removeFromTop (6);

        auto row = [&r] (juce::Label& l, juce::Component& c)
        {
            auto line = r.removeFromTop (28);
            l.setBounds (line.removeFromLeft (60));
            c.setBounds (line.reduced (0, 2));
        };
        row (gainLabel, gain);
        row (panLabel, pan);
        row (chokeLabel, chokeBox);
        auto toggles = r.removeFromTop (28);
        muteButton.setBounds (toggles.removeFromLeft (80));
        soloButton.setBounds (toggles.removeFromLeft (80));
        r.removeFromTop (6);
        auto buttons = r.removeFromTop (30);
        loadButton.setBounds (buttons.removeFromLeft (110));
        buttons.removeFromLeft (8);
        editButton.setBounds (buttons.removeFromLeft (140));
        buttons.removeFromLeft (8);
        resetButton.setBounds (buttons.removeFromLeft (130));
        r.removeFromTop (8);
        layersLabel.setBounds (r.removeFromTop (20));
        for (int i = 0; i < DrumKit::maxLayers + 1; ++i)
        {
            auto line = r.removeFromTop (28);
            auto& row = layerRows[(size_t) i];
            if (i > 0)
            {
                row.remove.setBounds (line.removeFromRight (26).reduced (0, 2));
                row.add.setBounds (line.withWidth (150).reduced (0, 2));   // (shown while the layer is empty, in the place of the name)
            }
            row.name.setBounds (line.removeFromLeft (190));
            row.range.setBounds (line.removeFromRight (70));
            row.slider.setBounds (line.reduced (0, 2));
        }
        r.removeFromTop (6);
        hint.setBounds (r.removeFromTop (40));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int pad = padAt (e.getPosition());
        if (pad < 0)
            return;
        select (pad);
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addItem (1, "Load sample...");
            m.addItem (2, "Edit sampler...", kit.hasSample (pad));
            m.addItem (3, "Back to built-in sound", kit.getPad (pad).file != juce::File() || pad < 8);
            m.showMenuAsync ({}, [this, pad] (int result)
            {
                if (result == 1) loadButton.onClick();
                if (result == 2 && hooks.editSampler) hooks.editSampler (pad);
                if (result == 3) resetButton.onClick();
            });
            return;
        }
        pressed = pad;
        grabKeyboardFocus();   // (so the keys play the pads from here on)
        if (hooks.preview)
            hooks.preview (kit.getPad (pad).note, true);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (pressed >= 0 && hooks.preview)
            hooks.preview (kit.getPad (pressed).note, false);
        pressed = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int pad = padAt (e.getPosition());
        if (pad >= 0 && ! e.mods.isPopupMenu() && hooks.editSampler && kit.hasSample (pad))
            hooks.editSampler (pad);
    }

    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        for (auto& f : files)
            if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;mp3;ogg"))
                return true;
        return false;
    }

    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        const int pad = padAt ({ x, y });
        if (pad >= 0 && files.size() > 0)
            loadInto (pad, juce::File (files[0]));
    }

private:
    juce::Rectangle<int> padBounds (int pad) const
    {
        const int col = pad % 4, rowFromBottom = pad / 4;
        const int cw = gridArea.getWidth() / 4, ch = gridArea.getHeight() / 4;
        return { gridArea.getX() + col * cw, gridArea.getY() + (3 - rowFromBottom) * ch, cw, ch };
    }

    int padAt (juce::Point<int> p) const
    {
        for (int pad = 0; pad < DrumKit::numPads; ++pad)
            if (padBounds (pad).contains (p))
                return pad;
        return -1;
    }

    void loadInto (int pad, const juce::File& f)
    {
        begin();
        if (hooks.load && hooks.load (pad, f))
        {
            select (pad);
            edited();
        }
        else
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Cannot open sample", f.getFullPathName());
    }

    void rangeText (int i)
    {
        auto& row = layerRows[(size_t) i];
        row.range.setText ("vel " + juce::String ((int) std::max (1.0, row.slider.getMinValue())) + "-" + juce::String ((int) row.slider.getMaxValue()),
                           juce::dontSendNotification);
    }

    void edited() { if (hooks.changed) hooks.changed(); }
    void begin() { if (hooks.beginEdit) hooks.beginEdit(); }

    void timerCallback() override
    {
        if (kit.isActive() || pressed >= 0)
            repaint();
    }

    DrumKit& kit;
    Hooks hooks;
    int selected = 0, pressed = -1;
    std::array<bool, 16> keyDown {};
    juce::Rectangle<int> gridArea { 12, 12, 496, 330 };
    std::unique_ptr<juce::FileChooser> chooser;

    struct LayerRow
    {
        juce::Label name, range;
        juce::Slider slider;
        juce::TextButton add, remove { "x" };
    };
    std::array<LayerRow, DrumKit::maxLayers + 1> layerRows;
    juce::Label layersLabel;
    juce::Label nameLabel, fileLabel, gainLabel, panLabel, chokeLabel, hint;
    juce::Slider gain, pan;
    juce::ToggleButton muteButton { "Mute" }, soloButton { "Solo" };
    juce::ComboBox chokeBox;
    juce::TextButton loadButton { "Load sample..." }, editButton { "Edit sampler..." }, resetButton { "Built-in sound" };
};

class DrumRackWindow : public juce::DocumentWindow
{
public:
    DrumRackWindow (const juce::String& title, DrumKit& kit, DrumRackPanel::Hooks hooks, std::function<void()> onClosed)
        : juce::DocumentWindow (title, Theme::background, juce::DocumentWindow::closeButton), closed (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);
        panel = new DrumRackPanel (kit, std::move (hooks));
        setContentOwned (panel, true);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    DrumRackPanel& getPanel() { return *panel; }

    // Ctrl+Z / Ctrl+Y work in this window too: the owner decides (it undoes the project)
    std::function<bool (const juce::KeyPress&)> onKey;
    bool keyPressed (const juce::KeyPress& k) override { return (onKey && onKey (k)) || juce::DocumentWindow::keyPressed (k); }
    void closeButtonPressed() override { if (closed) closed(); }

private:
    DrumRackPanel* panel = nullptr;
    std::function<void()> closed;
};
