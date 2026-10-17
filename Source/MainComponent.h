#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"

class TrackHeader;
class TrackLane;
class TimeRuler;

// A viewport that tells us when it scrolls, so the track headers and the ruler can follow.
class SyncViewport : public juce::Viewport
{
public:
    std::function<void()> onScroll;

    void visibleAreaChanged (const juce::Rectangle<int>& area) override
    {
        juce::Viewport::visibleAreaChanged (area);
        if (onScroll) onScroll();
    }
};

class MainComponent : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override { importFiles (files, nullptr, 0.0); }

    static constexpr int controlsWidth = 270;
    static constexpr int stripHeight = 84;
    static constexpr int topBarHeight = 56;
    static constexpr int rulerHeight = 26;

    // ---- Used by the timeline components
    Engine& getEngine() { return engine; }
    SyncViewport& getLaneViewport() { return laneViewport; }
    double getPixelsPerSecond() const { return pixelsPerSecond; }
    int getScrollX() const { return laneViewport.getViewPositionX(); }
    double beatSeconds() const;
    double barSeconds() const { return beatSeconds() * 4.0; }
    double snapTime (double seconds) const;

    void selectClip (Track*, int clipId);
    bool isSelected (const Track* t, int clipId) const { return t == selTrack && clipId == selClip && clipId >= 0; }
    void setPlayhead (double seconds);
    void zoomBy (double factor, int anchorX);
    void importFiles (const juce::StringArray& paths, Track* target, double timeSeconds);
    void clipsChanged();

private:
    void timerCallback() override;
    void addTrackUI (Track&);
    void removeTrackUI (Track*);
    void clearTracksUI();
    void layoutContent();
    void addTrackDialog();

    void splitAtPlayhead();
    void deleteSelected();
    void duplicateSelected();

    void saveProject (const juce::File&);
    void saveProjectAs();
    void loadProject (const juce::File&);
    void openProjectDialog();
    void exportDialog();

    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer player;
    Engine engine;

    juce::TextButton rewindButton { "|<" }, playButton { "Play" }, addButton { "+ Track" };
    juce::TextButton openButton { "Open" }, saveButton { "Save" }, saveAsButton { "Save As" }, exportButton { "Export WAV" };
    juce::TextButton zoomOutButton { "-" }, zoomInButton { "+" };
    juce::ToggleButton snapButton { "Snap" };
    juce::File currentProject;
    juce::ToggleButton metroButton { "Metronome" };
    juce::Label bpmLabel { {}, "BPM" }, timeLabel;
    juce::Slider bpmSlider { juce::Slider::LinearBar, juce::Slider::TextBoxLeft };
    juce::Slider masterSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    float masterShown = 0.0f;

    double pixelsPerSecond = 100.0;
    Track* selTrack = nullptr;
    int selClip = -1;

    juce::OwnedArray<TrackHeader> headers;
    juce::OwnedArray<TrackLane> lanes;
    SyncViewport laneViewport;
    juce::Viewport headerViewport;
    juce::Component laneHolder, headerHolder;
    std::unique_ptr<TimeRuler> ruler;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
