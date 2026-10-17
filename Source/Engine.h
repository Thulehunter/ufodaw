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
#include "ExternalMidi.h"
#include "TempoMap.h"

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

    // Fades (seconds, equal-power curves; an overlap fade-out + fade-in on two clips is a crossfade) and reverse (the clip's
    // region of the source plays backwards).
    double fadeIn = 0.0, fadeOut = 0.0;
    bool reverse = false;

    // Takes: clips of one group are alternatives for the same spot (a loop recording makes one per pass); only the
    // active one plays and shows. takeGroup 0 = an ordinary clip.
    int takeGroup = 0;
    int takeIndex = 0;    // 1, 2, 3 ... in recording order
    bool takeActive = true;
    bool audible() const { return takeGroup == 0 || takeActive; }

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

struct Track;

// Notes from a sequencer plugin go to `target`. channel 0 passes every MIDI channel through unchanged;
// 1..16 passes only that channel and delivers it to the target as channel 1 (what single-voice synths listen to).
struct MidiRoute
{
    Track* target = nullptr;
    int channel = 0;
};

// Notes go to a hardware MIDI output (an analog synth on the interface's MIDI OUT, a USB MIDI device).
// channel: which channel of the source to send (0 = every channel); outChannel: the channel the hardware gets
// (1..16), or 0 to keep each note's own channel.
struct ExternalRoute
{
    juce::String deviceId, deviceName;   // identifier of the output; the name is the fallback when the identifier changes
    int channel = 0;
    int outChannel = 1;
    int port = -1;                       // resolved by Engine::setExternalOuts; -1 while the device isn't available
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
    bool recordArmed = false;                               // audio tracks: the next audio recording goes into this track
    int inputFirst = 0;                                     // audio tracks: first of the device's enabled input channels this track listens to...
    int inputCount = 2;                                     // ...and how many (1 = mono, 2 = a stereo pair)
    bool monitor = false;                                   // armed audio tracks: also play the input through the track (not saved: off after loading)
    bool sequencer = false;                                 // UI role: this track's plugin is a note source (Stochas...), not a sound
    std::vector<MidiRoute> midiOuts;                        // tracks that play the notes this track's plugin sends
    std::vector<ExternalRoute> externalOuts;                // hardware that plays them (or the track's own notes: it then makes no sound itself)
    bool routesElsewhere() const { return ! midiOuts.empty() || ! externalOuts.empty(); }
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

    // Buses and sends. A bus track has no clips of its own: it mixes what other tracks send or route to it, runs that through its
    // own effects, inserts, fader and pan, and plays it into the master (a group channel / effect return).
    bool bus = false;
    Track* output = nullptr;                              // where this track plays: nullptr = the master, else a bus
    struct Send
    {
        Track* bus = nullptr;
        float level = 0.5f;                               // 0..1.5, like a fader
        bool preFader = false;                            // taken before the track's fader and pan (and kept when the fader is down)
    };
    std::vector<Send> sends;
    std::vector<float> busInL, busInR;                    // bus tracks: what arrived during the current block
    bool busFed = false;                                  // an audible track wrote into the bus during this block
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
    bool sequencer = false;
    bool recordArmed = false;
    int inputFirst = 0, inputCount = 2;
    std::vector<std::pair<int, int>> midiOuts;   // (index of the target track, channel) of each MIDI route
    bool bus = false;
    int outputIndex = -1;                        // index of the bus this track plays into (-1 = master)
    struct SendState { int bus = -1; float level = 0.5f; bool preFader = false; };
    std::vector<SendState> sends;
    std::vector<ExternalRoute> externalOuts;
    std::vector<std::shared_ptr<PluginSlot>> midiFx;
    std::shared_ptr<DrumKit> kit;
    std::shared_ptr<PluginSlot> instrument;                 // plugins are shared with the live project,
    std::vector<std::shared_ptr<PluginSlot>> inserts;       // so undo restores which ones are present, not their settings
};

struct ProjectState
{
    std::vector<TrackState> tracks;
    TempoMap tempo;
    TrackState master;   // the master channel: volume, effects, plugins, automation
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
    Track* addTrack (const juce::String& name, Track::Type type = Track::Type::Audio, bool asBus = false);
    Track* addBus (const juce::String& name) { return addTrack (name, Track::Type::Audio, true); }
    void removeTrack (Track*);

    // ---- Message thread: routing. A track plays into the master or into one bus, and can also send a share of its sound to buses.
    void setTrackOutput (Track&, Track* bus);                                  // nullptr = master; ignored for buses and for non-bus targets
    void setSend (Track&, Track& bus, float level, bool preFader);             // level <= 0 removes the send
    bool getSend (const Track&, const Track& bus, float& level, bool& preFader) const;
    std::vector<Track*> getBuses() const;
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

    // Fades, crossfades and reverse of audio clips.
    bool setClipFades (Track&, int clipId, double fadeIn, double fadeOut);   // clamped to the clip's length
    bool setClipReverse (Track&, int clipId, bool reverse);
    int crossfadeOverlaps (Track&, int clipId);                             // how many neighbours were crossfaded with this clip

    // Takes (see Clip::takeGroup). addTakes puts the clips in one group, the last one active, and returns its id.
    int addTakes (Track&, std::vector<Clip>);
    std::vector<Clip> getTakes (const Track&, int clipId) const;   // every take of the clip's group, in order (just itself without takes)
    bool setActiveTake (Track&, int clipId);                       // this one plays, the others of its group are silent
    bool deleteTake (Track&, int clipId);                          // removes only this take
    bool flattenTakes (Track&, int clipId);                        // keeps this take as an ordinary clip, drops the others
    void setClipTake (Track&, int clipId, int group, int index, bool active);   // project loading

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
    void setMidiOuts (Track& source, std::vector<MidiRoute> routes);   // empty = this track plays its own sound; only other MIDI tracks count
    // Hardware MIDI outputs: opens the devices and replaces the track's external routes. A route whose device
    // cannot be opened keeps port -1 (the caller can tell the user).
    void setExternalOuts (Track&, std::vector<ExternalRoute>);
    ExternalMidiSender& getExternalMidi() { return externalMidi; }
    void setOutputLatencyMs (double ms) { outputLatencyMs = ms; }   // MIDI to hardware leaves this much later, to line up with the audio
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

    // Mixes the project offline (faster than realtime) into a stereo WAV.
    struct RenderOptions
    {
        bool withMetronome = false;
        int bits = 24;                                  // 16, 24 or 32 (32 = floating point: never clips)
        const Track* onlyTrack = nullptr;               // set: render just this track (a stem)
        bool masterProcessing = true;                   // false: no master volume / limiter (stems)
        std::function<bool()> shouldCancel;             // polled between blocks; true stops the render
        std::function<void (double)> onProgress;        // 0..1, called from the rendering thread
    };
    bool renderToFile (const juce::File&, double lengthSeconds, bool withMetronome);
    bool renderToFile (const juce::File&, double lengthSeconds, const RenderOptions&);
    bool trackHasOutput (const Track&) const;           // whether the track would give a non-empty stem

    // Records the audio input (while the transport plays) straight to a 24-bit WAV on a background thread.
    // Several files can record at the same time, each from its own input channels (numbered among the device's
    // enabled input channels: 0 is the first enabled one).
    struct RecordTarget
    {
        int firstChannel = 0;
        int numChannels = 2;   // 1 or 2
        juce::File file;
    };
    bool startRecording (const std::vector<RecordTarget>&);   // false (nothing recording) if any file cannot be created
    void stopRecording();
    bool isRecording() const { return recordingActive.load(); }

    // Punch in/out: while enabled, a recording only captures between punchIn and punchOut (seconds). When the punch-out
    // has passed, punchFinished() turns true (the transport keeps running; the app then closes the recording).
    std::atomic<bool> punchEnabled { false };
    std::atomic<double> punchIn { 0.0 }, punchOut { 0.0 };
    bool punchFinished() const { return punchDone.load(); }
    double recordedStartSeconds() const   // where on the timeline the first recorded sample lies; -1 if nothing yet
    {
        const auto s = recFirstSample.load();
        return s < 0 ? -1.0 : (double) s / sampleRate;
    }

    // Tuner: while a channel is set (numbered among the enabled input channels, like RecordTarget), the audio thread keeps
    // the latest input samples of that channel for readTunerSamples(). -1 = off.
    static constexpr int tunerRingSize = 8192;
    void setTunerChannel (int c) { tunerChannel = c; }
    int getTunerChannel() const { return tunerChannel.load(); }
    bool readTunerSamples (float* dest, int n) const;   // the newest n samples, oldest first; false when the tuner is off

    void play()  { playing = true; }
    void stop()  { playing = false; }
    bool isPlaying() const { return playing; }
    void setPositionSeconds (double s) { positionSamples = (juce::int64) juce::jmax (0.0, s * sampleRate); }
    double getPositionSeconds() const { return (double) positionSamples / sampleRate; }
    double getSampleRate() const { return sampleRate; }

    // Loop region in seconds; while enabled, playback jumps from loopEnd back to loopStart.
    std::atomic<bool> loopEnabled { false };
    std::atomic<double> loopStart { 0.0 }, loopEnd { 0.0 };

    // The tempo and time signature (see TempoMap). `bpm` mirrors the first section's tempo. Changing the map re-times the
    // MIDI clips, the loop and the punch region so they keep their place in the music; audio clips stay where they are in time.
    void setTempoMap (const TempoMap&, bool retimeMidi);
    TempoMap getTempoMap() const;
    std::atomic<float> bpm { 120.0f };
    std::atomic<bool> metronome { false };
    std::atomic<float> masterLevel { 0.0f };

    // The master channel: the last stage of the mix. It is a Track without clips, so it has a volume
    // (gain), built-in effects, plugin inserts and automation like any other channel; mixBlock runs the
    // summed tracks through it before the limiter.
    Track& getMaster() { return *master; }
    const Track& getMaster() const { return *master; }
    bool isMaster (const Track& t) const { return &t == master.get(); }
    void setMasterLevel (float v) { masterLevel = v; master->level = v; }
    void resetMaster();   // back to defaults (a new project)

private:
    struct NoteEvent
    {
        int offset;     // sample offset inside the block
        bool on;
        int pitch;
        float velocity;
        int channel = 1;   // MIDI channel 1..16; clip notes use 1, notes from sequencer plugins keep theirs
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
    TrackState captureTrack (const Track&) const;               // everything the user can edit on one track
    void applyTrackState (Track&, const TrackState&);           // ...and the way back (also used for the master channel)
    void forwardExternal (const Track&, const juce::MidiMessage&, int sampleOffset);   // to the track's hardware outputs
    void externalAllNotesOff (const Track&);
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
    TempoMap tempoMap;                              // guarded by `lock`
    std::unique_ptr<Track> master;
    int nextClipId = 1;

    juce::TimeSliceThread writerThread { "UFODAW audio writer" };
    struct Recorder
    {
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        int first = 0, count = 2;
    };
    std::vector<Recorder> recorders;                // guarded by recLock; the audio thread only try-locks it
    std::atomic<bool> recordingActive { false };
    std::atomic<bool> punchDone { false };
    std::atomic<juce::int64> recFirstSample { -1 };
    int nextTakeGroup = 1;

    std::array<float, tunerRingSize> tunerRing {};
    std::atomic<int> tunerChannel { -1 };
    std::atomic<juce::int64> tunerWritePos { 0 };

    // The device input of the block being mixed, copied before the output buffer is cleared (all channels): what
    // armed tracks with Monitor on play back through their effects and fader.
    juce::AudioBuffer<float> inputBuf;
    int inputChannelsCopied = 0;
    int inputOffset = 0;                            // where the sub-block being mixed starts inside the callback's block
    bool anyMonitoring() const;
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

    ExternalMidiSender externalMidi;
    std::atomic<double> outputLatencyMs { 0.0 };
    double blockStartMs = 0.0;                      // the millisecond clock at the start of the block being mixed

    std::atomic<bool> offlineRendering { false };   // an export owns the mixer; the audio callback stays silent
    const Track* renderOnly = nullptr;              // set during an offline render of a single track
    bool renderMaster = true;

    std::atomic<bool> playing { false };
    std::atomic<juce::int64> positionSamples { 0 };
    double sampleRate = 44100.0;
};
