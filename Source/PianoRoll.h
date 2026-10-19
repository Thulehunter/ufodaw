#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <bitset>
#include "Engine.h"
#include "RollPanel.h"

class MainComponent;

// Note editor for one MIDI clip, shown below the arrangement. Its time axis follows the
// arrangement's zoom and scroll, so notes line up with the clips above.
class PianoRoll : public RollPanel
{
public:
    explicit PianoRoll (MainComponent&);
    ~PianoRoll() override;

    void showClip (Track*, int clipId) override;
    Track* getTrack() const override { return track; }
    int getClipId() const override { return clipId; }

    bool stillValid() const override;
    bool deleteSelectedNote() override;
    void repaintGrid() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int toolbarHeight = 34;
    static constexpr int keyWidth = 56;
    static constexpr int velocityHeight = 64;

    class Keys;
    class Grid;
    class Velocity;

private:
    std::bitset<128> activePitches();   // notes sounding now: played live, or under the playhead in this clip
    void scrollToNotes();               // scroll the grid so the clip's notes are in view
    void applySynthParams();
    void updateToolbar();   // plugin-instrument tracks show a "Plugin window" button instead of the built-in synth controls

    MainComponent& main;
    Track* track = nullptr;
    int clipId = -1;

    juce::Label title;
    juce::ComboBox waveform;
    juce::Label attackLabel { {}, "Attack" }, releaseLabel { {}, "Release" }, cutoffLabel { {}, "Cutoff" };
    juce::Slider attack, release, cutoff;
    juce::TextButton closeButton { "x" }, pluginButton { "Plugin window" }, fitButton { "Show notes" };
    // the key: root note, scale, chords, snapping (stored with the track)
    juce::ComboBox rootBox, scaleBox, chordBox;
    juce::ToggleButton snapToggle { "Snap to scale" };
    juce::TextButton fitScaleButton { "Fit notes" };
    juce::Label scaleLabel;
    void refreshScaleControls();
    void fitNotesToScale();
    juce::ComboBox laneBox;           // what the lane under the notes shows: velocity, or a controller (mod wheel, pitch bend...)
    void refreshLaneChoices();        // adds the controllers a clip contains that are not in the usual list
    bool pendingScroll = false;
    bool toolbarShowsPlugin = false;

    juce::Viewport viewport;
    juce::Component holder;
    std::unique_ptr<Keys> keys;
    std::unique_ptr<Grid> grid;
    std::unique_ptr<Velocity> velocity;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRoll)
};
