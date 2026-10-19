#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "PluginHost.h"
#include "Theme.h"
#include "Tuner.h"
#include "RailButton.h"
#include "TourOverlay.h"
#include <map>
#include <set>

class SamplerWindow;   // (SamplerPanel.h)
class DrumRackWindow;  // (DrumRackPanel.h)
class MixerWindow;     // (MixerPanel.h)
class WarpWindow;      // (WarpEditor.h)
struct MidiMapping;    // (MidiLearn.h)
class MidiLearnWindow;
class MasterMeterWindow;   // (MasterMeter.h)
struct Command;        // (Commands.h)
class CommandPalette;
class ShortcutsWindow;
class VersionHistoryWindow;   // (VersionHistory.h)

// Settings shared by the track headers and the lanes
// The project file format: older files always open (a missing property takes its default); a file with a higher number was saved by a newer UFODAW.
constexpr int projectFormatVersion = 3;

namespace UiSettings
{
    inline bool panSnap = true;        // pan sliders and pan envelopes snap to panStep (Alt while dragging flips it)
    inline int countInBars = 0;        // bars of metronome clicks before a recording starts (0 = none)
    inline bool follow = true;         // the arrangement scrolls along with the playhead while playing
    inline bool autoUpdateCheck = true;     // look for a new version once a day at start-up (one small request to GitHub)
    inline juce::int64 lastUpdateCheck = 0; // when (milliseconds since 1970)
    inline bool tourDone = false;      // the first-run tour was shown (or skipped); not shown again for people who already had settings
    inline int cpuThreads = -1;        // helper threads for the audio engine: -1 = automatic, 0 = none, else that many
    inline int exportChoice = 0;      // the last audio format chosen for an export (an index into AudioExport::choices())

    // Recorded audio is placed this much earlier, to undo the round trip through the interface: 0 = the latency the driver
    // reports, 1 = what the latency test measured, 2 = none. Plus a manual extra offset.
    inline int latencyMode = 0;
    inline double latencyMeasuredMs = 0.0;
    inline double latencyExtraMs = 0.0;

    // MIDI outputs (identifiers) that get MIDI clock / start / stop, and a shift for the clock in milliseconds
    inline juce::StringArray clockDevices;
    inline double clockOffsetMs = 0.0;
    constexpr double panStep = 0.05;   // 5 % towards left or right
}

// The big "CLIP" lamp in the top bar: red while something has clipped (the master mix, a track, or an armed input), until clicked.
class ClipLamp : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void()> onClick;

    void setLit (bool on)
    {
        if (on != lit)
        {
            lit = on;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        if (lit)
        {
            g.setColour (Theme::redBright.withAlpha (0.3f));
            g.fillRoundedRectangle (r.expanded (1.0f), 7.0f);
            g.setColour (Theme::redBright);
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (Theme::text);
            g.drawRoundedRectangle (r, 6.0f, 1.5f);
        }
        else
        {
            g.setColour (Theme::redDim.withAlpha (0.35f));
            g.fillRoundedRectangle (r, 6.0f);
        }
        g.setColour (lit ? Theme::text : Theme::textFaint);
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText ("CLIP", getLocalBounds(), juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (onClick)
            onClick();
    }

private:
    bool lit = false;
};

class TrackHeader;
class TrackLane;
class SubLaneRow;
class SubLaneHeader;
class GroupHeader;
class SampleBrowserWindow;
class ClipLauncherWindow;
class SynthWindow;
class SubLaneFooter;
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
    friend class CollectJob;   // reports back when the audio has been copied
public:
    // Each open project is one MainComponent (see ProjectTabs.h); the slot names its recovery file. `devices` is the one audio device manager
    // that all tabs share (only the shown tab is attached to it); without it the component makes and opens its own.
    explicit MainComponent (int tabSlot = 0, std::shared_ptr<juce::AudioDeviceManager> devices = nullptr);
    static void initialiseDevices (juce::AudioDeviceManager&);   // the audio setup of the last run (driver, device, rate, buffer, channels)
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;
    void focusLost (FocusChangeType) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override { importFiles (files, nullptr, 0.0); }

    static constexpr int railWidth = 52;        // the icon rail on the left
    static constexpr int controlsWidth = 270;
    static constexpr int stripHeight = 110;
    static constexpr int topBarHeight = 56;
    static constexpr int rulerHeight = 26;
    static constexpr int bottomBarHeight = 60;
    static constexpr int rollHeight = 320;
    static constexpr int masterHeight = 66;

    // ---- Used by the timeline components
    Engine& getEngine() { return engine; }
    void openSynthPanel (Track&);                    // every setting of a track's built-in synth

    // ---- Scripting (see Scripting.cpp): what a script needs that only the window can do
    Track* scriptAddTrack (const juce::String& kind, const juce::String& name);   // kind: "audio", "midi" (the built-in synth) or "drums"
    void scriptBeginEdit() { beginEdit(); }          // one undo step for the whole script
    void scriptSetBpm (double bpm);
    void scriptDone();                               // the project changed: show it
    std::pair<Track*, int> scriptSelection() const { return { selTrack, selClip }; }
    void runScriptFile (const juce::File&);          // asks for the script's parameters, runs it, tells the result
    void showScriptMenu();                           // a menu of the scripts in the scripts folder
    SyncViewport& getLaneViewport() { return laneViewport; }
    juce::Component& getLaneHolder() { return laneHolder; }
    Track* trackAtY (int yInLaneHolder) const;       // the track whose lane contains this y, or nullptr
    void setLoopRegion (double startSeconds, double endSeconds);   // also switches looping on
    void setPunchRegion (double startSeconds, double endSeconds);  // also switches punch on
    void openProject (const juce::File& file) { loadProject (file); }
    void offerRecovery();   // at start-up: if an autosave from an unfinished session exists, ask whether to restore it

    // ---- Project tabs
    void closePluginEditors() { pluginHost.closeAllEditors(); }
    void suspendAudio();    // another tab takes the audio device: stop, close editors, let go of the device and the MIDI inputs
    void resumeAudio();      // this tab is shown again and takes the device back
    juce::String getTitle() const { return currentProject != juce::File() ? currentProject.getFileNameWithoutExtension() : untitledTitle.isNotEmpty() ? untitledTitle : juce::String ("Untitled"); }
    bool hasUnsavedChanges() const { return editCounter != savedEdit; }
    int getTabSlot() const { return tabSlot; }
    void markClean() { savedEdit = editCounter; }   // closing a project without saving: it leaves no recovery file behind
    bool isEmptyUntitled() const { return currentProject == juce::File() && engine.getNumTracks() == 0 && ! isDirty(); }
    bool saveForClose();     // true when saved (or nothing to save); an untitled project opens the Save As dialog and returns false
    std::function<void()> requestNewTab, requestCloseTab, onTitleChanged;
    std::function<void (int)> requestSwitchTab;   // Ctrl+Tab / Ctrl+Shift+Tab: +1 or -1
    std::function<void (const juce::File&)> requestOpenInTab;
    void openFileHere (const juce::File& f) { loadProject (f); }
    // Command line "--selftest": open a project without any dialogs and report what came out (see Main.cpp)
    static inline bool selfTesting = false;
    static inline juce::StringArray selfTestProblems;
    juce::String selfTestReport (bool& ok, bool expectWarning);
    bool renderForCommandLine (const juce::File& target);   // "--render": the whole project to an audio file (the format from the extension)
    void openTemplateHere (const juce::File& templateFile);   // a project from the starter pack: opened as a new, unsaved project (it is named after the template)
    std::function<void (const juce::File&)> requestOpenTemplate;
    juce::String untitledTitle;
    void openInstrumentEditor (Track&);   // the plugin window of the track's instrument (or first effect); the VST panel if none
    double getPixelsPerSecond() const { return pixelsPerSecond; }

    // While recording: the peaks of what an armed track has recorded so far, and where on the timeline they start (for drawing the waveform as it forms)
    std::shared_ptr<const Engine::RecPeaks> liveRecordingFor (const Track& t, double& startSeconds, double& secondsPerPeak) const
    {
        if (! engine.isRecording() || recLoopTakes)
            return nullptr;
        const double start = engine.recordedStartSeconds();
        if (start < 0.0)
            return nullptr;
        for (size_t i = 0; i < recordings.size(); ++i)
            if (recordings[i].track == &t)
                if (auto peaks = engine.getRecordingPeaks ((int) i))
                {
                    startSeconds = juce::jmax (0.0, start - recordLatencySeconds());
                    secondsPerPeak = (double) peaks->bucket / engine.getSampleRate();
                    return peaks;
                }
        return nullptr;
    }
    int getScrollX() const { return laneViewport.getViewPositionX(); }
    const TempoMap& getTempoMap() const { return tempoMap; }   // tempo and time signature sections: grid, snapping, ruler
    void applyTempoMap (const TempoMap&);                       // also re-times MIDI, the loop and the punch region
    void openTempoPanel();
    void openSectionsPanel();                                   // the sections of the arrangement (verse, chorus...)
    void sectionGoTo (int index);
    void sectionLoop (int index);
    void sectionCopy (int index);
    void sectionSwap (int index);                               // section `index` and the one after it change places
    void openWarpEditor (Track&, int clipId);                   // markers on the hits of an audio clip, dragged to the grid
    void slicesToMidi (Track&);                                // the sampler's slices as a MIDI pattern at the playhead
    void setFirstTempo (double bpm, bool withUndo = true);                           // the tempo of the first section (one undo step)
    double snapTime (double seconds) const;

    bool isLiveNoteDown (int pitch) const   // a note being played right now on a MIDI keyboard or the computer keys
    {
        const auto& mask = pitch < 64 ? liveMaskLow : liveMaskHigh;
        return pitch >= 0 && pitch < 128 && ((mask.load() >> (pitch & 63)) & 1ull) != 0;
    }

    void selectClip (Track*, int clipId);

    // The clip whose volume curve is being edited (the points are shown and can be dragged); none by default.
    int getEnvelopeClip() const { return envelopeClipId; }
    Track* getEnvelopeTrack() const { return envelopeTrack; }
    int envelopeClipId = -1;
    Track* envelopeTrack = nullptr;
    void openClipPanel (Track&, int clipId);   // gain, stretch and pitch of an audio clip
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
    void freezeTrack (Track&);     // bounce the track to audio (on a new track right after it) and switch the original off: saves the CPU
    void unfreezeTrack (Track&);   // remove the bounce, the track plays itself again
    void clearTracksUI();
    void layoutContent();
    void layoutMasterLane();                                    // keeps the master strip's lane scrolled with the arrangement
    std::unique_ptr<TrackHeader> makeHeader (Track&, bool master);
    std::unique_ptr<TrackHeader> makeHeaderRaw (Track&, bool master);
    void dragTrackHeader (Track&, int yInHolder, bool finished);   // moving a track by its header: the drop line, then the move
    void addTrackDialog();
    void addTrackMenu();                       // "+ Track": an empty audio track, or audio from files
    void addEmptyAudioTrack();
    void addBusTrack();                                      // a group / effects-return bus
    void addSamplerTrack();                                  // a MIDI track that plays one sample (Simpler-style)
    void openSampler (Track&);                               // the sampler's window: sample, markers, mode, envelope, filter
    void sampleClip (Track&, int clipId);                    // an audio clip's audio becomes the sample of a new sampler track
    void startSamplerStretch (std::shared_ptr<Sampler>, double ratio, std::function<void (bool)> done);   // in the background, with a progress window
    std::unique_ptr<MixerWindow> mixerWindow;
    void openMixer();                                      // channel strips of every track and the master
    void showTrackMenu (Track&);                          // right-click on a track header
public:
    static constexpr int subLaneHeight = 46;
    static constexpr int subFooterHeight = 28;             // the row under a track's sub-tracks: how many, with - and +
    void changeSubLaneCount (Track&, int delta);        // + adds a sub-track (the next pitch up), - takes the last one away
    void renameSubLane (Track&, int sub, const juce::String& name);
    // Track groups (folders): a header row above the tracks of a group, that collapses them and mutes / solos them together
    static constexpr int groupRowHeight = 30;
    void syncGroupUI();                                  // builds the group header rows to match the groups
    void toggleGroupCollapsed (int groupId);
    void muteSoloGroup (int groupId, bool solo);
    void renameGroup (int groupId);
    void showGroupMenu (int groupId);
    void groupChanged();                                 // the order or the groups changed: headers and lanes again
    void showTrackGroupMenuItems (juce::PopupMenu&, const Track&);
    void showSubLaneMenu (Track&, int sub);            // right-click on a sub-track: rename, pitch, remove
    void applySubLaneChoice (Track&, int choice);       // 1 show / hide, 2 from the notes, 3 drum preset, 4 add one, 5 remove all
    void toggleSubLanes (Track&);                       // the + / - button of a track header
private:
    void autoSubLanes();                                // a drum plugin as the instrument: its sub-tracks appear by themselves
    int subAutoTicks = 0;
public:
    void syncSubLaneUI();                                // builds / removes the sub-track rows to match the tracks
private:
    void rebuildTrackUI();                                 // headers and lanes again, in the engine's order
public:
    void startTour();          // the quick tour of the window (first start, and File > Quick tour)
    void maybeStartTour();     // ...only if it was not shown before
private:
    std::unique_ptr<TourOverlay> tour;
    std::unique_ptr<SamplerWindow> samplerWindow;
    // A track as the tool windows know it: after an undo the tracks are rebuilt, and the windows that stay open are pointed at the new ones.
    struct TrackRef
    {
        std::shared_ptr<Track*> p;
        TrackRef() = default;
        explicit TrackRef (Track* t) : p (std::make_shared<Track*> (t)) {}
        Track& operator*() const { return **p; }
        Track* operator->() const { return *p; }
        operator Track*() const { return p != nullptr ? *p : nullptr; }
    };
    TrackRef samplerRef, padRef, rackRef;
    int padWindowPad = -1;
    bool handleUndoKey (const juce::KeyPress&);   // Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z in a tool window
public:
    void openDrumRack (Track&, int selectPad = -1);          // the drum rack: 4 x 4 pads, level / pan / choke per pad, each pad a Simpler
private:
    void openPadSampler (Track&, int pad);
    std::unique_ptr<SamplerWindow> padSamplerWindow;
    std::unique_ptr<WarpWindow> warpWindow;
    std::unique_ptr<MasterMeterWindow> meterWindow;
    std::unique_ptr<SampleBrowserWindow> browserWindow;
    std::unique_ptr<ClipLauncherWindow> launcherWindow;
    std::unique_ptr<SynthWindow> synthWindow;
    // Updates: the latest release on GitHub. A button in the top bar appears when a newer version exists.
    juce::TextButton updateButton { "Update available" };
    static inline juce::String updateVersion, updateNotes, updateDownload, updateSha;   // (empty version: none; shared by all the tabs)
    void checkForUpdates (bool userAsked);                      // File > Check for updates; also once a day at start-up
    void showUpdateInfo();
    void openClipLauncher();                          // the grid of clips that are started live
    void openSampleBrowser();                         // the starter pack and your own folders, with preview
    void openMasterMeter();                           // loudness, spectrum and the reference track
    std::unique_ptr<VersionHistoryWindow> historyWindow;
    std::unique_ptr<DrumRackWindow> drumRackWindow;
    void openRouting (Track&, juce::Component& anchor);      // output (master or a bus) and sends of a track
    void openQuantize (Track&, int clipId);                  // quantize / swing / humanize the notes of a MIDI clip
    void armAudioTrack (Track&, bool armed);   // several audio tracks can be armed: all of them record at once
    void armTrack (Track&);                    // toggles the red dot: audio tracks record, MIDI tracks listen to their MIDI input and record
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
    void applyCpuThreads();

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
    static inline ClipboardItem clipboard;   // (one for all projects: copy in one tab, paste in another)
    struct SectionClipboard { bool valid = false; RangeBundle bundle; ArrSection section; };
    static inline SectionClipboard sectionClipboard;
    void sectionToClipboard (int index);
    void sectionPaste();                                      // the section on the clipboard at the playhead (to a bar)
    void copySelected (bool cut);
    void pasteClip (Track*, double atSeconds);
    Track* pasteTargetTrack() const;

    enum class SaveKind { Normal, Autosave, Copy };   // an autosave or a copy leaves the project's own file and name alone
    void saveProject (const juce::File&, SaveKind kind = SaveKind::Normal);
    void saveProjectAs();
    void saveMenu();                                  // the "Save As" button: save as, new version, collect, backups
    void saveNewVersion();
    void collectProject();                            // copies all audio into <folder>/Audio and saves the project there
    void finishCollect (const juce::File& projectFile, const std::map<juce::String, juce::String>& paths,
                        const juce::StringArray& failed, juce::int64 bytes, bool cancelled);
    void writeBackup (const juce::File& project);     // every save leaves a time-stamped copy (the newest twenty are kept)
    static juce::File backupFolder (const juce::File& project);
    std::function<juce::String (const juce::File&)> projectFilePath;   // set while collecting: where a file lives in the project

    // Autosave: every two minutes, when something changed, the project goes to a recovery file. A normal save (or a normal exit
    // without unsaved changes) removes it; if the program dies, or is closed with unsaved changes, it is offered at the next start.
    void openVersionHistory();                        // File > Version history...
    // MIDI learn: controllers of a hardware MIDI controller tied to parameters (see MidiLearn.h)
    std::vector<MidiMapping> midiMappings;
    std::unique_ptr<MidiLearnWindow> learnWindow;
    std::array<std::atomic<int>, 16 * 128> ccValue;           // the last value of every controller on every channel (MIDI thread writes)
    std::array<std::atomic<unsigned>, 16 * 128> ccStamp;      // counts the messages
    std::atomic<bool> ccLearning { false };
    std::atomic<int> ccLearnedNumber { -1 }, ccLearnedChannel { 0 };
    std::function<void (int, int)> onCcLearned;               // (message thread) the controller that was learned
    void openMidiLearn();
    void applyMidiMappings();                                 // (message thread, 30 times a second)
    struct PluginOpen { juce::MemoryBlock before; size_t undoSize = 0; };
    std::map<PluginSlot*, PluginOpen> pluginOpen;     // plugin windows that are open: the state they had when they opened
    void pluginEditorOpened (PluginSlot&);            // (what is done inside a plugin's window is part of undo)
    void pluginEditorClosed (PluginSlot&);
    std::vector<Command> commands;                    // everything the program can do (the palette and the shortcuts come from it)
    std::unique_ptr<CommandPalette> palette;
    std::unique_ptr<ShortcutsWindow> shortcutsWindow;
    void buildCommands();
    void openPalette();                               // Ctrl+K
    void openShortcuts();                             // see and change the keyboard shortcuts
    static juce::File templatesFolder();              // track templates (one track saved as a small project file)
    void saveTrackTemplate (Track&);
    void addFromTemplate (const juce::File&);
    const Track* templateOnly = nullptr;              // while a template is saved: the one track that goes into the file
    juce::uint32 lastTimedBackupMs = juce::Time::getMillisecondCounter();
    int backedUpEdit = -1;
    int editCounter = 0, savedEdit = 0, autosavedEdit = 0;   // bumped by every undoable edit; compared to what was saved / autosaved
    juce::uint32 lastAutosaveMs = 0;
    bool isDirty() const { return editCounter != savedEdit; }
    juce::File autosaveFile() const;
    int tabSlot = 0;
    juce::File autosaveInfoFile() const;
    void autosaveNow();
    void clearAutosave();
    void loadProject (const juce::File&, bool asTemplate = false, bool relinking = false);
    static inline std::map<juce::String, juce::File> relinkMap;   // missing file name -> where "Find files" found it (only while a project is re-opened)
    void showProjectCheck (const juce::File& project, bool asTemplate, bool relinking, const juce::StringArray& missing, const juce::StringArray& pluginProblems, const juce::String& futureNote);
    void openProjectDialog();
    void newProject();                                // an empty project (asks first when there are unsaved changes)
    void fileMenu();                                  // the "File" button: new, open, recent, save, export
    void resetToEmptyProject();
    static juce::File recentListFile();
    juce::StringArray loadRecentProjects() const;
    void addRecentProject (const juce::File&);
    void exportDialog();
    void exportStemsDialog();

    struct MidiForwarder : public juce::MidiInputCallback
    {
        explicit MidiForwarder (MainComponent& o) : owner (o) {}
        void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;   // MIDI thread
        MainComponent& owner;
    };

    PluginHost pluginHost;   // declared before the engine so it outlives the plugins the engine holds
    std::shared_ptr<juce::AudioDeviceManager> sharedDevices;   // one audio device for all project tabs
    juce::AudioDeviceManager& deviceManager;
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
    struct MidiRec   // what one MIDI track is recording; times are absolute timeline seconds until the clip is created
    {
        Track* track = nullptr;
        std::vector<HeldNote> held;
        std::vector<Note> notes;
        std::vector<CcEvent> ccs;
        std::map<int, std::pair<double, float>> lastCc;   // per controller: when and what was last kept (thins out dense streams)
    };
    juce::CriticalSection midiRecLock;
    std::atomic<bool> midiRecording { false };
    std::vector<MidiRec> midiRecs;   // guarded by midiRecLock

    // Where live MIDI goes: the selected MIDI track and every MIDI track armed with its red dot, each with the input it listens to.
    // Rebuilt on the message thread, read on the MIDI thread.
    struct LiveTarget { Track* track = nullptr; juce::String deviceId; int channel = 0; };
    juce::CriticalSection liveTargetsLock;
    std::vector<LiveTarget> liveTargets;
    void refreshLiveTargets();
    void routeLiveMidi (const juce::String& sourceId, const juce::MidiMessage&);   // MIDI thread
    void liveEvent (Track*, const juce::MidiMessage&);                              // plays it on the track and records it
    bool beginMidiRecording (bool includeSelected);   // sets up midiRecs for the MIDI tracks that record: those armed with their dot, and
                                                      // the selected one if asked; false (and not recording) when there is none
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
    ClipLamp clipLamp;
    juce::String clipLampText;   // what the lamp last said (so it is only touched when it changes)

    // bookkeeping of the 30 Hz timer: it only repaints what changed
    int timerTick = 0, lastPlayheadX = -1000, cpuPercentShown = -1;
    double lastTimerPosition = -1.0;
    unsigned long long lastMaskLow = 0, lastMaskHigh = 0;
    void updateClipLamp();
    std::unique_ptr<TunerWindow> tunerWindow;

    // what a loop/punch recording needs to know when it ends
    bool recLoopTakes = false;
    double recLoopStart = 0.0, recLoopEnd = 0.0;
    juce::TooltipWindow tooltipWindow { this };
    double playStartSeconds = 0.0;
    double recStartSeconds = 0.0;
    juce::Rectangle<int> transportPill;
    int statusCompact = 0;
    juce::TextButton fileButton { "File" }, stopAllButton { "STOP ALL" }, followButton { "Follow" };
    void followPlayhead (double seconds);
    RailButton railFile { RailButton::Icon::Menu, "File: new project, open, save, export" },
               railAdd { RailButton::Icon::Add, "Add a track", true },
               railPlugins { RailButton::Icon::Plugins, "Plugins: scan and manage VST3 plugins" },
               railTuner { RailButton::Icon::Tuner, "Tuner" },
               railMixer { RailButton::Icon::Mixer, "Mixer: faders, pan, mute, solo and meters of every track" },
               railAudio { RailButton::Icon::Audio, "Audio and MIDI settings" };
    juce::TextButton openButton { "Open" }, saveButton { "Save" }, saveAsButton { "Save As" }, exportButton { "Export WAV" },
                     exportStemsButton { "Export stems" };
    juce::TextButton zoomOutButton { "-" }, zoomInButton { "+" };
    juce::ToggleButton snapButton { "Snap" };
    juce::ToggleButton panSnapButton { "Pan snap 5%" };
    juce::File currentProject;
    juce::TextButton metroButton { "Click" };       // the metronome on / off: in the transport bar, works while playing and recording
    juce::TextButton countInButton { "Count-in" };   // top bar: how many bars of clicks come before a recording
    void updateCountInButton();
    double recordLatencySeconds() const;   // how much earlier a recording is placed on the timeline (the round trip through the interface)
    TempoMap tempoMap;   // the same as the engine's; only the message thread changes either, always together
    juce::TextButton tempoButton { "Tempo" }, sectionsButton { "Sections" }, tapButton { "Tap" };
    std::vector<juce::uint32> tapTimes;                 // tap tempo: when the button was tapped (ms)
    void tapTempo();
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
    juce::OwnedArray<SubLaneRow> subRows;          // the sub-tracks shown under MIDI tracks (see Engine.h: SubLane), in arrangement order
    juce::OwnedArray<SubLaneHeader> subHeaders;
    juce::OwnedArray<SubLaneFooter> subFooters;
    juce::OwnedArray<GroupHeader> groupHeaders;     // one per group that has tracks, in the order they appear
    juce::OwnedArray<juce::Component> groupBands;   // (the matching strip in the lane area)
    juce::String groupSignature;
    juce::String subSignature;                    // what they were built for: they are made again when this changes
    struct DropLine : juce::Component
    {
        DropLine() { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override { g.setColour (juce::Colour (0xfff28c28)); g.fillRoundedRectangle (getLocalBounds().toFloat(), 2.0f); }
    } dropLine;
    int trackRowsEnd = 0;                           // where the last track (with its sub-tracks) ends
    std::vector<int> trackTops;                    // the top of every track's row in the arrangement (sub-tracks make the rows uneven)
    SyncViewport laneViewport;
    juce::Viewport headerViewport;
    juce::Component laneHolder, headerHolder;
    std::unique_ptr<TimeRuler> ruler;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
