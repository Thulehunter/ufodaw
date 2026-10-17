#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"

class TrackStrip;

class MainComponent : public juce::Component, private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    static constexpr int controlsWidth = 270;
    static constexpr int stripHeight = 84;
    static constexpr int topBarHeight = 56;
    static constexpr double pixelsPerSecond = 100.0;

private:
    void timerCallback() override;
    void addTrackDialog();
    void addTrack (const juce::File&);
    void addStrip (Track&);
    void removeStrip (TrackStrip*);
    void saveProject (const juce::File&);
    void saveProjectAs();
    void loadProject (const juce::File&);
    void openProjectDialog();
    void exportDialog();
    void layoutStrips();
    juce::Rectangle<int> laneArea() const;

    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer player;
    Engine engine;

    juce::TextButton rewindButton { "|<" }, playButton { "Play" }, addButton { "+ Track" };
    juce::TextButton openButton { "Open" }, saveButton { "Save" }, saveAsButton { "Save As" }, exportButton { "Export WAV" };
    juce::File currentProject;
    juce::ToggleButton metroButton { "Metronome" };
    juce::Label bpmLabel { {}, "BPM" }, timeLabel;
    juce::Slider bpmSlider { juce::Slider::LinearBar, juce::Slider::TextBoxLeft };
    juce::Slider masterSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    float masterShown = 0.0f;

    juce::OwnedArray<TrackStrip> strips;
    juce::Viewport viewport;
    juce::Component stripHolder;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
