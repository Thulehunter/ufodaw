#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"

// Controls for one track's effect chain (EQ -> compressor -> delay -> reverb).
// Shown in a callout from the track header; every change applies immediately.
// The EQ is drawn as a curve with six bands you drag (the selected band has sliders under it); the compressor shows its curve and a gain reduction meter.
class FxPanel : public juce::Component
{
public:
    // `onBeginEdit` is called right before each user change, so the host can take an undo snapshot.
    // With an engine, the compressor can listen to another track (sidechain): a box under its sliders picks that track.
    FxPanel (Track&, std::function<void()> onBeginEdit, Engine* engine = nullptr);

    void paint (juce::Graphics&) override;
    void resized() override;

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
    };

    Section& addSection (const juce::String& title, std::atomic<bool>& enabled);
    void addRow (Section&, const juce::String& name, double lo, double hi, double mid,
                 const juce::String& suffix, int decimals, std::atomic<float>& param);

    void selectBand (int band);                  // the EQ band the three sliders under the curve edit
    void syncFromParams();                       // after presets, undo and drags in the curve: sliders show what the parameters say
    void applyEqPreset (int index);
    void applyCompPreset (int index, bool withUndo = true);
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
