#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"

// Controls for one track's effect chain (EQ -> compressor -> delay -> reverb).
// Shown in a callout from the track header; every change applies immediately.
class FxPanel : public juce::Component
{
public:
    explicit FxPanel (Track&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Row
    {
        juce::Label label;
        juce::Slider slider;
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

    Track& track;
    std::vector<std::unique_ptr<Section>> sections;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxPanel)
};
