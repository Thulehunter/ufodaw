#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"

class MainComponent;

// Note editor for one MIDI clip, shown below the arrangement. Its time axis follows the
// arrangement's zoom and scroll, so notes line up with the clips above.
class PianoRoll : public juce::Component
{
public:
    explicit PianoRoll (MainComponent&);
    ~PianoRoll() override;

    void showClip (Track*, int clipId);
    Track* getTrack() const { return track; }
    int getClipId() const { return clipId; }

    bool stillValid() const;          // false once the clip or its track is gone
    bool deleteSelectedNote();        // true if a note was deleted
    void repaintGrid();

    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void()> onClose;

    static constexpr int toolbarHeight = 34;
    static constexpr int keyWidth = 56;

    class Keys;
    class Grid;

private:
    void applySynthParams();

    MainComponent& main;
    Track* track = nullptr;
    int clipId = -1;

    juce::Label title;
    juce::ComboBox waveform;
    juce::Label attackLabel { {}, "Attack" }, releaseLabel { {}, "Release" }, cutoffLabel { {}, "Cutoff" };
    juce::Slider attack, release, cutoff;
    juce::TextButton closeButton { "x" };

    juce::Viewport viewport;
    juce::Component holder;
    std::unique_ptr<Keys> keys;
    std::unique_ptr<Grid> grid;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRoll)
};
