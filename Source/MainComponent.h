#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"

class TrackHeader;
class TrackLane;
class TimeRuler;
class PianoRoll;

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

// Round icon-style transport button. `lit` shows an active state (playing, recording, ...).
class TransportButton : public juce::Button
{
public:
    enum Shape { Rewind, Play, Pause, Stop, Record };

    TransportButton (Shape s, const juce::String& tooltip) : juce::Button (tooltip), shape (s)
    {
        setTooltip (tooltip);
        setMouseClickGrabsKeyboardFocus (false);
    }

    void setLit (bool shouldBeLit)
    {
        if (lit != shouldBeLit)
        {
            lit = shouldBeLit;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (juce::Colour (down ? 0xff4a505c : over ? 0xff3a3f49 : 0xff2b2f37));
        g.fillEllipse (b);
        g.setColour (juce::Colour (0xff0b0c0e));
        g.drawEllipse (b, 1.0f);

        juce::Colour c (0xffc8ccd4);
        if (shape == Play && lit)   c = juce::Colour (0xff3ddc84);
        if (shape == Pause && lit)  c = juce::Colour (0xffffc247);
        if (shape == Record)        c = lit ? juce::Colour (0xffff4d4d) : juce::Colour (0xffb04a4a);
        g.setColour (c);

        const auto r = b.reduced (b.getWidth() * 0.28f);
        const float cy = r.getCentreY();
        juce::Path p;
        switch (shape)
        {
            case Play:   p.addTriangle (r.getX() + r.getWidth() * 0.1f, r.getY(), r.getX() + r.getWidth() * 0.1f, r.getBottom(),
                                        r.getRight(), cy); break;
            case Pause:  p.addRectangle (r.getX() + r.getWidth() * 0.05f, r.getY(), r.getWidth() * 0.32f, r.getHeight());
                         p.addRectangle (r.getRight() - r.getWidth() * 0.37f, r.getY(), r.getWidth() * 0.32f, r.getHeight()); break;
            case Stop:   p.addRectangle (r); break;
            case Record: p.addEllipse (r); break;
            case Rewind: p.addRectangle (r.getX(), r.getY(), r.getWidth() * 0.15f, r.getHeight());
                         p.addTriangle (r.getRight(), r.getY(), r.getRight(), r.getBottom(), r.getX() + r.getWidth() * 0.25f, cy); break;
        }
        g.fillPath (p);
    }

private:
    Shape shape;
    bool lit = false;
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
    bool keyStateChanged (bool isKeyDown) override;
    void focusLost (FocusChangeType) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override { importFiles (files, nullptr, 0.0); }

    static constexpr int controlsWidth = 270;
    static constexpr int stripHeight = 84;
    static constexpr int topBarHeight = 56;
    static constexpr int rulerHeight = 26;
    static constexpr int bottomBarHeight = 60;
    static constexpr int rollHeight = 320;

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

    // Call right before a change the user should be able to undo. Once per gesture.
    void beginEdit();
    void cancelEdit();   // drops the snapshot beginEdit() just took, if the edit turned out to change nothing

private:
    void timerCallback() override;
    void addTrackUI (Track&);
    void removeTrackUI (Track*);
    void clearTracksUI();
    void layoutContent();
    void addTrackDialog();
    void addMidiTrack();
    void undo();
    void redo();
    void restoreSnapshot (const ProjectState&);
    void updateUndoButtons();
    void showRoll (Track*, int clipId);
    void hideRoll();

    // ---- Live MIDI (hardware keyboard or computer keys) -> synth of the armed track
    void enableAllMidiInputs();
    void updateArmedTrack();
    void liveNote (int pitch, bool on, float velocity);
    void finishMidiRecording();

    void startPlayback();
    void pausePlayback();
    void stopPlayback();
    void toggleRecording();
    void finishRecording();

    void splitAtPlayhead();
    void deleteSelected();
    void duplicateSelected();

    void saveProject (const juce::File&);
    void saveProjectAs();
    void loadProject (const juce::File&);
    void openProjectDialog();
    void exportDialog();

    struct MidiForwarder : public juce::MidiInputCallback
    {
        explicit MidiForwarder (MainComponent& o) : owner (o) {}
        void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;   // MIDI thread
        MainComponent& owner;
    };

    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer player;
    Engine engine;
    MidiForwarder midiForwarder { *this };
    juce::MidiDeviceListConnection midiListConnection;

    juce::TextButton addButton { "+ Track" }, addMidiButton { "+ MIDI Track" };
    std::unique_ptr<PianoRoll> roll;
    bool rollVisible = false;
    juce::TextButton undoButton { "Undo" }, redoButton { "Redo" };

    // The MIDI track that live notes go to: the selected MIDI clip's track, or the open piano roll's.
    // Read from the MIDI thread, so it is atomic; the pointer is only ever compared / passed by address.
    std::atomic<Track*> armedTrack { nullptr };
    juce::Label statusLabel;
    juce::String midiInputText;
    juce::ToggleButton keysButton { "Keys" };   // computer keyboard as piano
    int pianoOctave = 0;
    bool pianoKeyDown[16] = {};
    int pianoKeyPitch[16] = {};

    struct HeldNote { int pitch; double start; float velocity; };
    juce::CriticalSection midiRecLock;
    std::atomic<bool> midiRecording { false };
    std::vector<HeldNote> heldNotes;
    std::vector<Note> recordedNotes;   // start = absolute timeline seconds until the clip is created
    Track* recMidiTrack = nullptr;
    std::vector<ProjectState> undoStack, redoStack;
    TransportButton rewindButton { TransportButton::Rewind, "Rewind (Enter)" },
                    playButton   { TransportButton::Play,   "Play (Space)" },
                    pauseButton  { TransportButton::Pause,  "Pause (Space)" },
                    stopButton   { TransportButton::Stop,   "Stop" },
                    recordButton { TransportButton::Record, "Record (R)" };
    juce::TooltipWindow tooltipWindow { this };
    double playStartSeconds = 0.0;
    double recStartSeconds = 0.0;
    juce::File recordingFile;
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
