#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "PluginHost.h"
#include "Theme.h"
#include "Tuner.h"

// Settings shared by the track headers and the lanes
namespace UiSettings
{
    inline bool panSnap = true;        // pan sliders and pan envelopes snap to panStep (Alt while dragging flips it)
    constexpr double panStep = 0.05;   // 5 % towards left or right
}

class TrackHeader;
class TrackLane;
class TimeRuler;
class PianoRoll;
class DrumSequencer;
class RollPanel;

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
        g.setColour (Theme::border);
        g.drawEllipse (b, 1.0f);

        juce::Colour c (0xffc8ccd4);
        if (shape == Play && lit)   c = Theme::orange;
        if (shape == Pause && lit)  c = Theme::yellow;
        if (shape == Record)        c = lit ? Theme::redBright : Theme::redDim;
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
                      private juce::Timer,
                      private juce::ChangeListener
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
    static constexpr int stripHeight = 106;
    static constexpr int topBarHeight = 56;
    static constexpr int rulerHeight = 26;
    static constexpr int bottomBarHeight = 60;
    static constexpr int rollHeight = 320;
    static constexpr int masterHeight = 66;

    // ---- Used by the timeline components
    Engine& getEngine() { return engine; }
    SyncViewport& getLaneViewport() { return laneViewport; }
    juce::Component& getLaneHolder() { return laneHolder; }
    Track* trackAtY (int yInLaneHolder) const;       // the track whose lane contains this y, or nullptr
    void setLoopRegion (double startSeconds, double endSeconds);   // also switches looping on
    void setPunchRegion (double startSeconds, double endSeconds);  // also switches punch on
    void openProject (const juce::File& file) { loadProject (file); }
    void openInstrumentEditor (Track&);   // the plugin window of the track's instrument (or first effect); the VST panel if none
    double getPixelsPerSecond() const { return pixelsPerSecond; }
    int getScrollX() const { return laneViewport.getViewPositionX(); }
    double beatSeconds() const;
    double barSeconds() const { return beatSeconds() * 4.0; }
    double snapTime (double seconds) const;

    bool isLiveNoteDown (int pitch) const   // a note being played right now on a MIDI keyboard or the computer keys
    {
        const auto& mask = pitch < 64 ? liveMaskLow : liveMaskHigh;
        return pitch >= 0 && pitch < 128 && ((mask.load() >> (pitch & 63)) & 1ull) != 0;
    }

    void selectClip (Track*, int clipId);
    // right-click menu on a clip (clipId >= 0) or on an empty spot of a lane (clipId < 0)
    void showClipMenu (Track&, int clipId, double atSeconds, juce::Component& source);
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
    void layoutMasterLane();                                    // keeps the master strip's lane scrolled with the arrangement
    std::unique_ptr<TrackHeader> makeHeader (Track&, bool master);
    void addTrackDialog();
    void addTrackMenu();                       // "+ Track": an empty audio track, or audio from files
    void addEmptyAudioTrack();
    void addBusTrack();                                      // a group / effects-return bus
    void openRouting (Track&, juce::Component& anchor);      // output (master or a bus) and sends of a track
    void openQuantize (Track&, int clipId);                  // quantize / swing / humanize the notes of a MIDI clip
    void armAudioTrack (Track&, bool armed);   // several audio tracks can be armed: all of them record at once
    void openInputMenu (Track&, juce::Component& anchor);   // pick the audio interface input(s) of an audio track
    void addMidiTrack (bool drums = false);
    void undo();
    void redo();
    void restoreSnapshot (const ProjectState&);
    void updateUndoButtons();
    void showRoll (Track*, int clipId);
    void hideRoll();

    // ---- Small interface settings that are remembered (pan snap)
    static juce::File uiSettingsFile();
    void loadUiSettings();
    void saveUiSettings();

    // ---- Audio device: choose driver / interface, remembered between runs
    void changeListenerCallback (juce::ChangeBroadcaster*) override;   // the audio device setup changed
    static juce::File audioSettingsFile();
    void openAudioSettings();
    void updateAudioInfo();

    // ---- Live MIDI (hardware keyboard or computer keys) -> synth of the armed track
    void enableAllMidiInputs();
    void updateArmedTrack();
    void liveNote (int pitch, bool on, float velocity);
    void finishMidiRecording();

    void toggleLoop();
    void togglePunch();
    void openTuner();
    void loopToSelection();
    void openPlugins (Track&, juce::Component& anchor);
    void openAutomation (Track&, juce::Component& anchor);
    juce::var pluginsToVar (const Track&) const;
    void writeEffectsAndAutomation (juce::DynamicObject&, Track&);                          // for tracks and the master channel
    void readEffectsAndAutomation (Track&, const juce::var&, juce::StringArray& problems);
    void pluginsFromVar (Track&, const juce::var& trackVar, juce::StringArray& problems);

    void startPlayback();
    void pausePlayback();
    void stopPlayback();
    void toggleRecording();
    void finishRecording();

    void splitAtPlayhead();
    void deleteSelected();
    void duplicateSelected();

    // clip clipboard: cut / copy / paste of audio and MIDI clips
    struct ClipboardItem
    {
        bool valid = false, isMidi = false;
        Clip audio;
        MidiClip midi;
    };
    ClipboardItem clipboard;
    void copySelected (bool cut);
    void pasteClip (Track*, double atSeconds);
    Track* pasteTargetTrack() const;

    void saveProject (const juce::File&);
    void saveProjectAs();
    void loadProject (const juce::File&);
    void openProjectDialog();
    void exportDialog();
    void exportStemsDialog();

    struct MidiForwarder : public juce::MidiInputCallback
    {
        explicit MidiForwarder (MainComponent& o) : owner (o) {}
        void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;   // MIDI thread
        MainComponent& owner;
    };

    PluginHost pluginHost;   // declared before the engine so it outlives the plugins the engine holds
    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer player;
    Engine engine;
    MidiForwarder midiForwarder { *this };
    juce::MidiDeviceListConnection midiListConnection;

    juce::TextButton addButton { "+ Track" }, addMidiButton { "+ MIDI" }, addDrumButton { "+ Drums" };
    std::unique_ptr<PianoRoll> pianoRoll;
    std::unique_ptr<DrumSequencer> drumRoll;
    RollPanel* roll = nullptr;   // whichever of the two editors is in use for the selected clip
    bool rollVisible = false;
    juce::TextButton undoButton { "Undo" }, redoButton { "Redo" };

    // The MIDI track that live notes go to: the selected MIDI clip's track, or the open piano roll's.
    // Read from the MIDI thread, so it is atomic; the pointer is only ever compared / passed by address.
    std::atomic<Track*> armedTrack { nullptr };
    std::atomic<unsigned long long> liveMaskLow { 0 }, liveMaskHigh { 0 };   // keys held down live, bit per MIDI note (for the piano roll)
    juce::Label statusLabel;
    juce::String midiInputText, audioInfoText;
    juce::TextButton audioButton { "Audio..." };
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
    struct PendingRecording   // one audio file being recorded: into an armed track, or into a new one (track == nullptr)
    {
        Track* track = nullptr;
        juce::File file;
    };
    std::vector<PendingRecording> recordings;
    std::vector<ProjectState> undoStack, redoStack;
    TransportButton rewindButton { TransportButton::Rewind, "Rewind (Enter)" },
                    playButton   { TransportButton::Play,   "Play (Space)" },
                    pauseButton  { TransportButton::Pause,  "Pause (Space)" },
                    stopButton   { TransportButton::Stop,   "Stop" },
                    recordButton { TransportButton::Record, "Record (R)" };
    juce::TextButton loopButton { "Loop" }, pluginsButton { "Plugins..." };
    juce::TextButton punchButton { "Punch" }, tunerButton { "Tuner" };
    std::unique_ptr<TunerWindow> tunerWindow;

    // what a loop/punch recording needs to know when it ends
    bool recLoopTakes = false;
    double recLoopStart = 0.0, recLoopEnd = 0.0;
    juce::TooltipWindow tooltipWindow { this };
    double playStartSeconds = 0.0;
    double recStartSeconds = 0.0;
    juce::TextButton openButton { "Open" }, saveButton { "Save" }, saveAsButton { "Save As" }, exportButton { "Export WAV" },
                     exportStemsButton { "Export stems" };
    juce::TextButton zoomOutButton { "-" }, zoomInButton { "+" };
    juce::ToggleButton snapButton { "Snap" };
    juce::ToggleButton panSnapButton { "Pan snap 5%" };
    juce::File currentProject;
    juce::ToggleButton metroButton { "Metronome" };
    juce::Label bpmLabel { {}, "BPM" }, timeLabel;
    juce::Slider bpmSlider { juce::Slider::LinearBar, juce::Slider::TextBoxLeft };
    // The master channel strip, pinned below the tracks: controls on the left, an automation lane on the right.
    std::unique_ptr<TrackHeader> masterHeader;
    std::unique_ptr<TrackLane> masterLane;
    juce::Component masterArea;
    std::vector<int> topSeparators;   // x positions of the dividers between the top bar's button groups

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
