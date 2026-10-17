#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "RollPanel.h"

class MainComponent;

// Step sequencer for a drum track's clip: one row per pad, one cell per 16th note.
// Like the piano roll it follows the arrangement's zoom and scroll, so cells line up with the clips above.
// The notes it creates are ordinary MIDI notes on the pads' trigger pitches.
class DrumSequencer : public RollPanel
{
public:
    explicit DrumSequencer (MainComponent&);
    ~DrumSequencer() override;

    void showClip (Track*, int clipId) override;
    Track* getTrack() const override { return track; }
    int getClipId() const override { return clipId; }

    bool stillValid() const override;
    bool deleteSelectedNote() override { return false; }   // cells are toggled with the mouse
    void repaintGrid() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int toolbarHeight = 34;
    static constexpr int rowHeight = 28;

    class Pads;
    class Grid;

private:
    MainComponent& main;
    Track* track = nullptr;
    int clipId = -1;

    juce::Label title;
    juce::TextButton clearButton { "Clear pattern" }, closeButton { "x" };
    std::unique_ptr<Pads> pads;
    std::unique_ptr<Grid> grid;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumSequencer)
};
