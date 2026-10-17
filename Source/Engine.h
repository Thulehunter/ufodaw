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

enum AutoParam { AutoNone = 0, AutoVolume = 1, AutoPan = 2 };

struct AutomationLane
{
    std::vector<AutoPoint> points;       // sorted by time, guarded by Engine::lock
    std::atomic<bool> active { true };   // when false the fader/pan knob is used as usual
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

    std::atomic<float> gain { 0.8f };
    std::atomic<float> pan { 0.0f };
    std::atomic<bool> mute { false };
    std::atomic<bool> solo { false };
    std::atomic<float> level { 0.0f };

    AutomationLane autoVolume, autoPan;                   // fader range 0..1.5, pan -1..1
    std::atomic<float> effGain { 0.8f }, effPan { 0.0f }; // what the mixer last used (differs from gain/pan when automated)
    int autoEditParam = AutoNone;                         // UI only: which envelope the lane is currently editing
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
    std::vector<AutoPoint> autoVolume, autoPan;
    bool autoVolumeActive = true, autoPanActive = true;
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

    // ---- Message thread: automation envelopes (param is AutoVolume or AutoPan)
    std::vector<AutoPoint> getAutomation (const Track&, int param) const;
    void setAutomation (Track&, int param, std::vector<AutoPoint> points);   // sorted for you
    void setAutomationActive (Track&, int param, bool active);

    // ---- Message thread: hosted plugins
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
    void releaseAllNotes (Track&);                              // soft all-notes-off for the synth or plugin instrument
    void collectNoteEvents (Track&, int numSamples, juce::int64 pos, bool running);
    void renderDrumTrack (Track&, int numSamples, juce::int64 pos, bool running, float* scratch);
    void renderPluginInstrument (Track&, int numSamples, float* outL, float* outR);
    void processInsert (PluginSlot&, float* l, float* r, int numSamples);

    Clip* findClip (Track&, int clipId);
    const Clip* findClip (const Track&, int clipId) const;
    MidiClip* findMidiClip (Track&, int clipId);
    const MidiClip* findMidiClip (const Track&, int clipId) const;

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
