#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "Theme.h"

// Controls for one track's effect chain (EQ -> compressor -> delay -> reverb).
// Shown in a callout from the track header; every change applies immediately.
// The EQ is drawn as a curve with six bands you drag (the selected band has sliders under it); the compressor shows its curve and a gain reduction meter.
class FxPanel : public juce::Component, private juce::Timer
{
public:
    // `onBeginEdit` is called right before each user change, so the host can take an undo snapshot.
    // With an engine, the compressor can listen to another track (sidechain): a box under its sliders picks that track.
    FxPanel (Track&, std::function<void()> onBeginEdit, Engine* engine = nullptr);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    struct Row
    {
        juce::Label label;
        juce::Slider slider;
        std::atomic<float>* param = nullptr;
    };

    struct Section
    {
        juce::String title;
        juce::ToggleButton toggle;
        std::vector<std::unique_ptr<Row>> rows;
        std::vector<std::vector<juce::Component*>> footerRows;   // rows under the sliders: toggles, a combo, the presets (each row shared between its components)
        std::vector<juce::Component*> meters;                     // small gain reduction bars in the title row
        int page = 0;                                              // which page of the panel it is on
    };

    struct PresetDef
    {
        juce::String name;
        std::vector<std::pair<std::atomic<float>*, float>> floats;
        std::vector<std::pair<std::atomic<bool>*, bool>> bools;
    };

    // helpers for the effect pages: components the panel owns, put in a footer row of a section
    juce::ToggleButton& addToggle (Section&, std::atomic<bool>& param, const juce::String& text, const juce::String& tooltip, bool sameRowAsPrevious = false);
    void addPresets (Section&, std::atomic<bool>& enabled, std::vector<PresetDef> presets);
    void addMeter (Section&, std::function<float()> read, const juce::String& caption, float maxDb);
    void setSectionVisible (Section&, bool);
    juce::Rectangle<int> tileRect (int k) const;   // the k-th tile of the chain strip along the top
    void selectSection (int sectionIndex);          // which effect the big view below the strip shows
    void timerCallback() override { repaint (0, 0, getWidth(), 80); }   // (the on/off lights follow changes made elsewhere)
    int selected = 0;
    juce::Rectangle<int> detail;                    // the card with the controls of the selected effect

    Section& addSection (const juce::String& title, std::atomic<bool>& enabled);
    void addRow (Section&, const juce::String& name, double lo, double hi, double mid,
                 const juce::String& suffix, int decimals, std::atomic<float>& param);

    void selectBand (int band);                  // the EQ band the three sliders under the curve edit
    void syncFromParams();                       // after presets, undo and drags in the curve: sliders show what the parameters say
    void applyEqPreset (int index);
    void applyCompPreset (int index, bool withUndo = true);
    juce::Label satTypeLabel;
    juce::ComboBox satTypeBox;
    std::vector<std::unique_ptr<juce::Component>> owned;                    // toggles, presets boxes and meters of the effect pages
    std::vector<std::pair<juce::ToggleButton*, std::atomic<bool>*>> toggleLinks;   // (to show the parameters again after a preset or undo)
    void applyDelayPreset (int index);
    void applyReverbPreset (int index);
    void turnOn (Section&, std::atomic<bool>&);

    Section* delaySection = nullptr;
    Section* reverbSection = nullptr;
    juce::ToggleButton pingToggle { "Ping-pong" };
    juce::ComboBox delaySync, delayPresets, reverbPresets;

    std::unique_ptr<juce::Component> eqCurve;    // the response of the EQ, with its bands to drag
    std::unique_ptr<juce::Component> compView;   // the compressor's curve and gain reduction
    Row bandRows[3];                             // gain, frequency, Q of the selected band
    int band = 2;
    juce::ComboBox eqPresets, compPresets;
    juce::TextButton eqReset { "Reset" };
    juce::ToggleButton autoToggle { "Auto makeup" };
    juce::ToggleButton monoToggle;
    Track& track;
    Engine* engine = nullptr;
    Section* eqSection = nullptr;
    Section* compSection = nullptr;
    juce::Label sideLabel;
    juce::ComboBox sideBox;
    std::vector<Track*> sideTracks;
    std::function<void()> beginEdit;
    std::vector<std::unique_ptr<Section>> sections;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxPanel)
};

// The effects of a track in a window of its own: it can be dragged anywhere, and the windows of several tracks can be open next to each other.
class FxWindow : public juce::DocumentWindow
{
public:
    FxWindow (const juce::String& title, FxPanel* panel, std::function<void()> onClosed)
        : juce::DocumentWindow (title, Theme::background, juce::DocumentWindow::closeButton), closed (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (panel, true);
        setResizable (false, false);
        setVisible (true);
    }

    void closeButtonPressed() override { if (closed) closed(); }

private:
    std::function<void()> closed;   // (the owner deletes this window)
};
