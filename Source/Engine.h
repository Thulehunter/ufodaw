#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <memory>
#include <vector>
#include "Synth.h"
#include "Fx.h"
#include "Plugin.h"
#include "DrumKit.h"

// A piece of audio placed on the timeline. The sample data is immutable and shared, so
// splitting and duplicating clips is free; only start/offset/length differ.
struct Clip
{
    int id = 0;
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    double fileRate = 44100.0;
    juce::File sourceFile;

    double start = 0.0;   // position on the timeline (seconds)
    double offset = 0.0;  // where in the source audio the clip begins (seconds)
    double length = 0.0;  // how long the clip plays (seconds)

    double end() const { return start + length; }
    double sourceLength() const { return audio != nullptr && fileRate > 0 ? audio->getNumSamples() / fileRate : 0.0; }
};

struct Note
{
    int pitch = 60;          // MIDI note number, 60 = C4
    double start = 0.0;      // seconds, relative to the start of the clip
    double length = 0.5;     // seconds
    float velocity = 0.8f;
};

struct MidiClip
{
    int id = 0;
    double start = 0.0;      // position on the timeline (seconds)
    double length = 4.0;
    std::vector<Note> notes;

    double end() const { return start + length; }
};

// A breakpoint envelope: linear between points, flat before the first and after the last.
struct AutoPoint
{
    double time = 0.0;   // seconds on the timeline
    float value = 0.0f;
};

// Automation is addressed by id: "volume" and "pan" (fader range 0..1.5 / -1..1), "fx.<name>" (effect parameters),
// "synth.<name>" (built-in synth) and "plugin:<slot key>:<parameter index>" (hosted plugins, normalised 0..1).
struct AutomationLane   // volume and pan: handled by the mixer's fader code
{
    std::vector<AutoPoint> points;       // sorted by time, guarded by Engine::lock
    std::atomic<bool> active { true };   // when false the fader/pan knob is used as usual
};

struct ParamAutomation   // every other automatable parameter
{
    juce::String id, name;
    float lo = 0.0f, hi = 1.0f;
    std::vector<AutoPoint> points;
    std::atomic<bool> active { true };

    std::atomic<float>* floatTarget = nullptr;   // built-in parameter, or
    std::shared_ptr<PluginSlot> plugin;          // hosted plugin parameter
    int pluginParam = -1;
    float lastApplied = -1.0e9f;                 // audio thread: only touch the target when the value changes
};

struct AutoInfo
{
    bool valid = false;
    juce::String name;
    float lo = 0.0f, hi = 1.0f, current = 0.0f;
};

struct AutoTarget   // something that can be automated, for menus
{
    juce::String id, name, group;
};

inline float evalAutomation (const std::vector<AutoPoint>& pts, double time, float fallback)
{
    if (pts.empty())
        return fallback;
    if (time <= pts.front().time)
        return pts.front().value;
    if (time >= pts.back().time)
        return pts.back().value;

    for (size_t i = 1; i < pts.size(); ++i)
        if (time < pts[i].time)
        {
            const auto& a = pts[i - 1];
            const auto& b = pts[i];
            const double span = b.time - a.time;
            return span <= 0.0 ? b.value : a.value + (float) ((time - a.time) / span) * (b.value - a.value);
        }
    return pts.back().value;
}

struct Track
{
    enum class Type { Audio, Midi };

    juce::String name;
    Type type = Type::Audio;
    std::vector<Clip> clips;            // audio tracks, guarded by Engine::lock
    std::vector<MidiClip> midiClips;    // MIDI tracks, guarded by Engine::lock

    SynthParams synth;                  // MIDI tracks
    std::unique_ptr<Synth> voices;      // audio-thread state, created for MIDI tracks
    std::shared_ptr<DrumKit> kit;       // set on drum tracks: a MIDI track that plays the drum kit instead of the synth

    FxParams fx;                        // effect chain settings
    std::unique_ptr<TrackFx> fxState;   // audio-thread state of the chain

    // VST3 hosting. Only the message thread changes these (under Engine::lock); the audio thread reads them.
    std::shared_ptr<PluginSlot> instrument;                 // MIDI tracks: replaces the built-in synth when set
    std::vector<std::shared_ptr<PluginSlot>> inserts;       // effects after the built-in chain, in order
    juce::MidiBuffer liveMidi;                              // notes played live, waiting for the plugin instrument
    double pluginTailSamples = 0.0;                         // keep running the instrument this long after the transport stops

    // MIDI routing: the notes a plugin instrument (a sequencer such as Stochas) sends out go to another
    // MIDI track instead of being heard from this one.
    std::vector<Track*> midiOuts;                           // tracks that play the notes this track's plugin sends
    juce::MidiBuffer routedMidi;                            // notes other tracks sent here during the current block

    // MIDI plugins (arpeggiators, chord tools...) that rework the notes before they reach the instrument
    std::vector<std::shared_ptr<PluginSlot>> midiFx;

    // Plugin delay compensation: this track is delayed so it lines up with tracks whose plugins add latency
    std::vector<float> pdcL, pdcR;   // sized by Engine::sizeDelayBuffers (half a second)
    int pdcPos = 0, pdcDelay = 0;

    std::atomic<float> gain { 0.8f };
    std::atomic<float> pan { 0.0f };
    std::atomic<bool> mute { false };
    std::atomic<bool> solo { false };
    std::atomic<float> level { 0.0f };

    AutomationLane autoVolume, autoPan;                   // fader range 0..1.5, pan -1..1
    std::vector<std::unique_ptr<ParamAutomation>> autoLanes;
    std::atomic<float> effGain { 0.8f }, effPan { 0.0f }; // what the mixer last used (differs from gain/pan when automated)
    juce::String autoEditId;                              // UI only: id of the envelope the lane is editing ("" = none)
};

// A copy of everything the user can edit, used for undo/redo. Audio sample data is shared,
// not copied, so snapshots are cheap.
struct TrackState
{
    juce::String name;
    Track::Type type = Track::Type::Audio;
    std::vector<Clip> clips;
    std::vector<MidiClip> midiClips;
    float gain = 0.8f, pan = 0.0f;
    bool mute = false, solo = false;
    int waveform = 0;
    float synthValues[5] = {};          // attack, decay, sustain, release, cutoff
    std::vector<float> fxFloats;        // FxParams::forEachFloat order
    std::vector<char> fxBools;          // FxParams::forEachBool order
    struct LaneState
    {
        juce::String id;
        std::vector<AutoPoint> points;
        bool active = true;
    };
    std::vector<LaneState> autoLanes;   // includes "volume" and "pan"
    std::vector<int> midiOutIndices;    // indices of the tracks this one sends MIDI to
    std::vector<std::shared_ptr<PluginSlot>> midiFx;
    std::shared_ptr<DrumKit> kit;
    std::shared_ptr<PluginSlot> instrument;                 // plugins are shared with the live project,
    std::vector<std::shared_ptr<PluginSlot>> inserts;       // so undo restores which ones are present, not their settings
};

struct ProjectState
{
    std::vector<TrackState> tracks;
    float bpm = 120.0f, masterGain = 0.9f;
};

// Mixes all tracks against a shared transport. The audio thread only ever try-locks,
// so editing on the message thread can never block audio.
class Engine : public juce::AudioSource
{
public:
    Engine();
    ~Engine() override;

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;

    // ---- Message thread: tracks
    Track* addTrack (const juce::String& name, Track::Type type = Track::Type::Audio);
    void removeTrack (Track*);
    void removeAllTracks();
    int getNumTracks() const;
    Track* getTrack (int index) const;
    int indexOfTrack (const Track*) const;

    // ---- Message thread: drum tracks (MIDI tracks with a drum kit; their clips are MIDI clips)
    Track* addDrumTrack (const juce::String& name);
    bool loadDrumSample (Track&, int pad, const juce::File&);
    void resetDrumPad (Track&, int pad);

    // ---- Message thread: audio clips (all return -1 / false when nothing was done)
    int addClipFromFile (Track&, const juce::File&, double startSeconds);   // new clip id
    int addClip (Track&, Clip);                                             // used when loading projects
    std::vector<Clip> getClips (const Track&) const;                        // snapshot, safe to read
    bool getClip (const Track&, int clipId, Clip& out) const;
    bool setClipRegion (Track&, int clipId, double start, double offset, double length);

    // ---- Message thread: MIDI clips
    int addMidiClip (Track&, double startSeconds, double lengthSeconds);
    std::vector<MidiClip> getMidiClips (const Track&) const;
    bool getMidiClip (const Track&, int clipId, MidiClip& out) const;
    bool isMidiClip (const Track&, int clipId) const;
    bool setMidiNotes (Track&, int clipId, std::vector<Note> notes);
    bool setMidiClipLength (Track&, int clipId, double lengthSeconds);

    // ---- Message thread: editing that works on audio and MIDI clips alike
    bool moveClip (Track&, int clipId, double newStart);
    int splitClip (Track&, int clipId, double atSeconds);                   // id of the right-hand part
    int duplicateClip (Track&, int clipId);                                 // id of the copy, placed after the original
    bool deleteClip (Track&, int clipId);
    bool moveClipToTrack (Track& from, Track& to, int clipId);              // same type only, id is kept

    // ---- Message thread: automation envelopes, by id (see AutomationLane)
    std::vector<AutoTarget> listAutomationTargets (const Track&) const;      // everything that can be automated here
    AutoInfo getAutomationInfo (const Track&, const juce::String& id) const;
    std::vector<AutoPoint> getAutomation (const Track&, const juce::String& id) const;
    bool setAutomation (Track&, const juce::String& id, std::vector<AutoPoint> points);   // sorts; creates the lane if needed
    void setAutomationActive (Track&, const juce::String& id, bool active);
    bool isAutomationActive (const Track&, const juce::String& id) const;
    std::vector<juce::String> getAutomationIds (const Track&) const;         // lanes that have points

    // ---- Message thread: hosted plugins
    void setMidiOuts (Track& source, std::vector<Track*> targets);   // empty = this track plays its own sound; only other MIDI tracks count
    void addMidiFx (Track&, std::shared_ptr<PluginSlot>);
    void removeMidiFx (Track&, int index);
    void preparePlugin (PluginSlot&);                           // layout + prepareToPlay for the current audio setup
    void setInstrument (Track&, std::shared_ptr<PluginSlot>);   // null = built-in synth
    void addInsert (Track&, std::shared_ptr<PluginSlot>);
    void removeInsert (Track&, int index);
    int getMaxBlockSize() const { return maxBlock; }

    // Undo/redo support. restoreState() replaces every Track object, so any Track* held
    // elsewhere is invalid afterwards (clip ids are kept).
    ProjectState captureState() const;
    void restoreState (const ProjectState&);

    // Audition a note on a MIDI track's synth (works while the transport is stopped).
    // Safe to call from any thread (UI, MIDI input).
    void previewNote (const Track&, int pitch, bool on, float velocity = 0.8f);

    double getTotalLengthSeconds() const;
    double getRenderTailSeconds() const;   // extra time to render after the last clip (delay/reverb tails)

    // Mixes the whole project offline (faster than realtime) into a 24-bit stereo WAV.
    bool renderToFile (const juce::File&, double lengthSeconds, bool withMetronome);

    // Records the audio input (while the transport plays) straight to a 24-bit WAV on a background thread.
    bool startRecording (const juce::File&, int numChannels);
    void stopRecording();
    bool isRecording() const { return activeWriter.load() != nullptr; }

    void play()  { playing = true; }
    void stop()  { playing = false; }
    bool isPlaying() const { return playing; }
    void setPositionSeconds (double s) { positionSamples = (juce::int64) juce::jmax (0.0, s * sampleRate); }
    double getPositionSeconds() const { return (double) positionSamples / sampleRate; }
    double getSampleRate() const { return sampleRate; }

    // Loop region in seconds; while enabled, playback jumps from loopEnd back to loopStart.
    std::atomic<bool> loopEnabled { false };
    std::atomic<double> loopStart { 0.0 }, loopEnd { 0.0 };

    std::atomic<float> bpm { 120.0f };
    std::atomic<bool> metronome { false };
    std::atomic<float> masterGain { 0.9f };
    std::atomic<float> masterLevel { 0.0f };

private:
    struct NoteEvent
    {
        int offset;     // sample offset inside the block
        bool on;
        int pitch;
        float velocity;
    };

    struct PreviewEvent
    {
        const Track* track = nullptr;
        int pitch = 0;
        bool on = false;
        float velocity = 0.8f;
    };

    // Caller must hold `lock`; adds into outL/outR (which must be pre-cleared).
    // `running` is false when only synth tails/previews should be rendered.
    void mixBlock (float* outL, float* outR, int numSamples, juce::int64 pos, bool withMetronome, bool running);
    void renderMidiTrack (Track&, int numSamples, juce::int64 pos, bool running, float* scratch);
    void drainPreviewEvents();
    bool anySynthActive() const;
    void silenceSynths();
    ParamAutomation* findLane (const Track&, const juce::String& id) const;
    ParamAutomation* ensureLane (Track&, const juce::String& id);   // null if the id doesn't resolve to a parameter
    void applyAutomation (Track&, double timeSeconds);
    void releaseAllNotes (Track&);                              // soft all-notes-off for the synth or plugin instrument
    void collectNoteEvents (Track&, int numSamples, juce::int64 pos, bool running);
    void runMidiSource (Track&, int numSamples);                // plugin sequencer track: its MIDI output goes to Track::midiOuts
    void applyMidiFx (Track&, int numSamples);                  // runs noteEvents through the track's MIDI plugins
    static int trackLatency (const Track&);                     // samples of delay its plugins add
    void applyDelay (Track&, float* l, float* r, int numSamples, int delaySamples);
    void renderDrumTrack (Track&, int numSamples, juce::int64 pos, bool running, float* scratch);
    void renderPluginInstrument (Track&, int numSamples, float* outL, float* outR);
    void processInsert (PluginSlot&, float* l, float* r, int numSamples);

    Clip* findClip (Track&, int clipId);
    const Clip* findClip (const Track&, int clipId) const;
    MidiClip* findMidiClip (Track&, int clipId);
    const MidiClip* findMidiClip (const Track&, int clipId) const;

    // Tells hosted plugins the tempo, position and whether the transport runs (sequencers like Stochas need it).
    // Declared before `tracks` so it outlives the plugins that point at it.
    struct PlayHead : public juce::AudioPlayHead
    {
        explicit PlayHead (Engine& e) : engine (e) {}
        juce::Optional<PositionInfo> getPosition() const override;
        Engine& engine;
        std::atomic<juce::int64> samples { 0 };
        std::atomic<bool> playing { false };
    };
    PlayHead playHead { *this };

    juce::AudioFormatManager formats;
    mutable juce::CriticalSection lock;
    std::vector<std::unique_ptr<Track>> tracks;
    int nextClipId = 1;

    juce::TimeSliceThread writerThread { "MiniDAW audio writer" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter { nullptr };
    juce::SpinLock recLock;

    // UI -> audio thread queue for note previews
    static constexpr int previewQueueSize = 64;
    juce::SpinLock previewWriteLock;   // the fifo has one writer at a time: UI thread or MIDI thread
    juce::AbstractFifo previewFifo { previewQueueSize };
    std::array<PreviewEvent, previewQueueSize> previewQueue;

    juce::MidiBuffer midiScratch, emptyMidi;
    int maxBlock = 4096;                   // largest block hosted plugins are prepared for

    std::vector<NoteEvent> noteEvents;     // reused every block, reserved up front
    std::vector<float> synthScratch;       // mono synth output
    std::vector<float> trackL, trackR;     // one track's stereo signal before fader/pan
    juce::int64 lastBlockEnd = -1;
    bool wasRunning = false;

    std::atomic<bool> playing { false };
    std::atomic<juce::int64> positionSamples { 0 };
    double sampleRate = 44100.0;
};
