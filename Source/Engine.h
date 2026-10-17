#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <memory>
#include <vector>
#include "Synth.h"
#include "Fx.h"

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

struct Track
{
    enum class Type { Audio, Midi };

    juce::String name;
    Type type = Type::Audio;
    std::vector<Clip> clips;            // audio tracks, guarded by Engine::lock
    std::vector<MidiClip> midiClips;    // MIDI tracks, guarded by Engine::lock

    SynthParams synth;                  // MIDI tracks
    std::unique_ptr<Synth> voices;      // audio-thread state, created for MIDI tracks

    FxParams fx;                        // effect chain settings
    std::unique_ptr<TrackFx> fxState;   // audio-thread state of the chain

    std::atomic<float> gain { 0.8f };
    std::atomic<float> pan { 0.0f };
    std::atomic<bool> mute { false };
    std::atomic<bool> solo { false };
    std::atomic<float> level { 0.0f };
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

    // Undo/redo support. restoreState() replaces every Track object, so any Track* held
    // elsewhere is invalid afterwards (clip ids are kept).
    ProjectState captureState() const;
    void restoreState (const ProjectState&);

    // Audition a note on a MIDI track's synth (works while the transport is stopped).
    void previewNote (const Track&, int pitch, bool on);

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
    };

    // Caller must hold `lock`; adds into outL/outR (which must be pre-cleared).
    // `running` is false when only synth tails/previews should be rendered.
    void mixBlock (float* outL, float* outR, int numSamples, juce::int64 pos, bool withMetronome, bool running);
    void renderMidiTrack (Track&, int numSamples, juce::int64 pos, bool running, float* scratch);
    void drainPreviewEvents();
    bool anySynthActive() const;
    void silenceSynths();

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
    juce::AbstractFifo previewFifo { previewQueueSize };
    std::array<PreviewEvent, previewQueueSize> previewQueue;

    std::vector<NoteEvent> noteEvents;     // reused every block, reserved up front
    std::vector<float> synthScratch;       // mono synth output
    std::vector<float> trackL, trackR;     // one track's stereo signal before fader/pan
    juce::int64 lastBlockEnd = -1;
    bool wasRunning = false;

    std::atomic<bool> playing { false };
    std::atomic<juce::int64> positionSamples { 0 };
    double sampleRate = 44100.0;
};
