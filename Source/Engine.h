#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <memory>
#include <vector>
#include "Synth.h"
#include "Fx.h"
#include "ParallelRunner.h"
#include "Plugin.h"
#include "DrumKit.h"
#include "ExternalMidi.h"
#include "TempoMap.h"
#include "Sampler.h"
#include "AudioExport.h"
#include "Loudness.h"

// A breakpoint envelope: linear between points, flat before the first and after the last.
// (Automation lanes use seconds on the timeline; a clip's volume curve uses seconds from the start of the clip.)
struct AutoPoint
{
    double time = 0.0;
    float value = 0.0f;
};

// Automation takes and comping. Every recorded pass (a ride of a fader or knob) is kept as a take; the comp says which take
// plays where. The envelope the audio thread follows (the lane's `points`) is always flattened from base + comp, with a
// short crossfade at every join.
struct AutoTake
{
    int id = 0;
    juce::String name;
    double start = 0.0, end = 0.0;   // the time range the pass covers (seconds on the timeline)
    std::vector<AutoPoint> points;   // absolute times
};

struct AutoCompSeg { double start = 0.0, end = 0.0; int takeId = 0; };

struct AutoData
{
    std::vector<AutoPoint> base;     // the hand-drawn curve under the takes
    std::vector<AutoTake> takes;
    std::vector<AutoCompSeg> comp;   // sorted, not overlapping
    int nextTakeId = 1;
    double crossfade = 0.03;         // seconds, at the start and end of every comp segment
};

enum class AutoRecMode { Touch, Latch, Write };

// The envelope with the takes comped into it (what the audio thread follows).
std::vector<AutoPoint> flattenAutomation (const AutoData&);

// A warp marker: this moment of the original audio (src, seconds from the start of the processed region) plays at `dest` (seconds from the
// start of the warped clip). Between two markers the audio is time-stretched so that it fits; the audio before the first marker is stretched
// to reach it, and after the last one plays at its own speed.
struct WarpMarker
{
    double src = 0.0, dest = 0.0;
};

// A piece of audio placed on the timeline. The sample data is immutable and shared, so
// splitting and duplicating clips is free; only start/offset/length differ.
struct Clip
{
    std::vector<WarpMarker> warps;   // sorted; when there are any, the clip's audio is the warped one (see Engine::warpClip)
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

    // Clip gain (linear, 1 = unchanged) and a volume curve: a multiplier (0..2) that varies over the clip, times in seconds from the
    // start of the clip, flat outside the first and last point. No points = no curve.
    float gain = 1.0f;
    std::vector<AutoPoint> envelope;

    // Time-stretch and pitch-shift. `audio` then holds the processed region; `source` is the original (the whole, unprocessed audio, shared)
    // so the clip can be processed again or put back, `processedFrom` / `processedLength` the part of it (seconds) that was processed, and
    // `stretch` (2 = twice as long) / `pitch` (semitones) what was done to it. `offset` and `length` then count in the processed audio.
    std::shared_ptr<const juce::AudioBuffer<float>> source;
    double processedFrom = 0.0, processedLength = 0.0;
    double stretch = 1.0, pitch = 0.0;
    bool formants = false;   // the pitch shift kept the formants (the voice character)

    // Clip launcher: a clip that was launched repeats its region every `loopLength` seconds (0 = plays once, as an ordinary clip) for as long as it lasts
    double loopLength = 0.0;
    int launchScene = -1;     // the scene (row) of the launcher it came from
    bool processed() const { return source != nullptr; }

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

// A controller value in a MIDI clip: a mod wheel move, the expression or sustain pedal, the pitch bend, the aftertouch.
// controller 0..127 = the MIDI CC number, kPitchBend, kAftertouch (channel pressure); value 0..1 (pitch bend: 0.5 = centre).
struct CcEvent
{
    static constexpr int kPitchBend = 128, kAftertouch = 129;

    double time = 0.0;       // seconds, relative to the start of the clip (like notes)
    int controller = 1;
    float value = 0.0f;
};

struct MidiClip
{
    int id = 0;
    double start = 0.0;      // position on the timeline (seconds)
    double length = 4.0;
    std::vector<Note> notes;
    std::vector<CcEvent> ccs;   // sorted by time
    double loopLength = 0.0;    // clip launcher: repeats every loopLength seconds (see Clip::loopLength)
    int launchScene = -1;

    double end() const { return start + length; }
};

struct Track;

// One cell of the clip launcher: a clip (audio on an audio track, MIDI on a MIDI track) that can be started at any time and then repeats.
struct LaunchSlot
{
    bool used = false;
    Clip audio;
    MidiClip midi;
    juce::String name;
};

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

// Automation is addressed by id: "volume" and "pan" (fader range 0..1.5 / -1..1), "fx.<name>" (effect parameters),
// "synth.<name>" (built-in synth) and "plugin:<slot key>:<parameter index>" (hosted plugins, normalised 0..1).
struct AutomationLane   // volume and pan: handled by the mixer's fader code
{
    std::vector<AutoPoint> points;       // sorted by time, guarded by Engine::lock (= the flattened `data`)
    AutoData data;
    std::atomic<bool> override { false };   // while a ride is being recorded the user's own value is used
    std::atomic<bool> active { true };   // when false the fader/pan knob is used as usual
};

struct ParamAutomation   // every other automatable parameter
{
    juce::String id, name;
    float lo = 0.0f, hi = 1.0f;
    std::vector<AutoPoint> points;       // (= the flattened `data`)
    AutoData data;
    std::atomic<bool> override { false };
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

// A sub-track of a MIDI track: it shows (and edits) the notes of one pitch, or a range of pitches, of the track's clips, with its own
// mute and solo. All sub-tracks play through the same instrument: a drum plugin gets kick on one lane, snare on the next...
struct SubLane
{
    juce::String name;
    int pitchLo = 36, pitchHi = 36;
    bool mute = false, solo = false;
    float volume = 1.0f;   // the sub-track's fader (0 - 1.5): scales the velocity of its notes
    bool covers (int pitch) const { return pitch >= pitchLo && pitch <= pitchHi; }
};

// A note (on or off) inside the block that is being rendered
struct NoteEvent
{
    int offset;     // sample offset inside the block
    bool on;
    int pitch;
    float velocity;
    int channel = 1;   // MIDI channel 1..16; clip notes use 1, notes from sequencer plugins keep theirs
};

// Controller events of the clips (and nothing else) in the block being rendered, like NoteEvent.
struct CcBlockEvent
{
    int offset;
    int controller;
    float value;
};

struct Track
{
    enum class Type { Audio, Midi };

    juce::String name;
    juce::uint32 colour = 0;            // the colour of the track in the headers, mixer and clips (ARGB); 0 = the colour of its type
    int groupId = 0;                    // the folder (track group) the track is in; 0 = none. The members of a group sit next to each other.
    // The key of the piano roll of this track (see Scales.h): root 0..11, scale 0 = none, snap = new and moved notes stay in the scale,
    // chord 0 / 1 / 2 = single notes / triads / sevenths when a note is added
    int scaleRoot = 0, scaleType = 0, scaleChord = 0;
    bool scaleSnap = false;
    // Sidechain: this track's compressor listens to the sound of sidechainSource instead of its own (it ducks when that one is loud). A track
    // that is used this way (sidechainTapped) is rendered first and leaves a copy of its sound in scL / scR for the others.
    std::atomic<Track*> sidechainSource { nullptr };
    // Freeze: the track was bounced to audio (the track named freezeTwin plays that); this one is switched off completely (no instrument, effects or
    // plugins run) until it is unfrozen, which saves the CPU.
    std::atomic<bool> frozen { false };
    Track* freezeTwin = nullptr;
    std::atomic<bool> sidechainTapped { false };
    std::vector<float> scL, scR;
    Type type = Type::Audio;
    std::vector<Clip> clips;            // audio tracks, guarded by Engine::lock
    std::vector<MidiClip> midiClips;    // MIDI tracks, guarded by Engine::lock
    std::vector<LaunchSlot> slots;      // clip launcher: one cell per scene (the message thread changes it under Engine::lock)
    std::vector<Clip> launchedAudio;    // what the launcher plays or has queued: looping copies of slot clips placed on the timeline, guarded by Engine::lock
    std::vector<MidiClip> launchedMidi;
    std::vector<SubLane> subLanes;      // MIDI tracks: sub-tracks by pitch (message thread; changed under Engine::lock)
    bool subLanesOpen = false;          // UI: the sub-tracks are shown under the track
    bool subAutoDone = false;           // the program already looked at this track's instrument (a drum plugin gets its sub-tracks once, by itself)
    std::atomic<std::uint64_t> audibleLo { ~0ull }, audibleHi { ~0ull };   // bit p set = notes of pitch p play (sub-track mute / solo)
    std::atomic<float> pitchVolume[128];    // sub-track fader per pitch (1 = as written)
    std::atomic<float> pitchActivity[128];  // loudest note-on (velocity) of a pitch since the UI looked: the sub-track level meters
    Track() { for (auto& v : pitchVolume) v.store (1.0f); for (auto& a : pitchActivity) a.store (0.0f); }
    void noteSeen (int p, float velocity) { if (p >= 0 && p < 128 && velocity > pitchActivity[p].load (std::memory_order_relaxed)) pitchActivity[p].store (velocity, std::memory_order_relaxed); }
    float pitchGain (int p) const { return p < 0 || p > 127 ? 1.0f : pitchVolume[p].load(); }
    bool pitchAudible (int p) const { return p < 0 || p > 127 || (((p < 64 ? audibleLo.load() : audibleHi.load()) >> (p & 63)) & 1ull) != 0; }

    SynthParams synth;                  // MIDI tracks
    std::unique_ptr<Synth> voices;      // audio-thread state, created for MIDI tracks
    std::shared_ptr<DrumKit> kit;       // set on drum tracks: a MIDI track that plays the drum kit instead of the synth
    std::shared_ptr<Sampler> sampler;   // set on sampler tracks: a MIDI track that plays a sample (Simpler-style) instead of the synth

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
    juce::String midiInputId;                               // MIDI tracks: the MIDI input (device identifier) this track listens to; empty = every input
    int midiInputChannel = 0;                               // ...and the MIDI channel (1..16), 0 = all
    bool midiArmed = false;                                 // MIDI tracks: live MIDI plays this track and is recorded into it (besides the selected track)
    bool sequencer = false;                                 // UI role: this track's plugin is a note source (Stochas...), not a sound
    std::vector<MidiRoute> midiOuts;                        // tracks that play the notes this track's plugin sends
    std::vector<ExternalRoute> externalOuts;                // hardware that plays them (or the track's own notes: it then makes no sound itself)
    bool routesElsewhere() const { return ! midiOuts.empty() || ! externalOuts.empty(); }
    juce::MidiBuffer routedMidi;                            // notes other tracks sent here during the current block

    // MIDI plugins (arpeggiators, chord tools...) that rework the notes before they reach the instrument
    std::vector<std::shared_ptr<PluginSlot>> midiFx;

    // Plugin delay compensation: this track is delayed so it lines up with tracks whose plugins add latency
    std::vector<float> pdcL, pdcR;   // sized by Engine::sizeDelayBuffers (two seconds)
    int pdcPos = 0, pdcDelay = 0;
    int pdcOldDelay = 0, pdcFade = 0;   // a change of the delay while it is in use crossfades from the old delay to the new one (audio thread)

    std::atomic<float> gain { 0.8f };
    std::atomic<float> pan { 0.0f };
    std::atomic<bool> mute { false };
    std::atomic<bool> solo { false };
    std::atomic<float> level { 0.0f };
    std::atomic<float> dspLoad { 0.0f };        // the share of the audio block's time this track needs (0..1, smoothed): its plugins and effects
    std::atomic<bool> clipped { false };        // the track's output reached full scale (latched until cleared)
    std::atomic<float> inputLevel { 0.0f };     // armed audio tracks: the loudest sample of the input in the last block
    std::atomic<bool> inputClipped { false };   // ...and whether the input reached full scale (latched until cleared)

    AutomationLane autoVolume, autoPan;                   // fader range 0..1.5, pan -1..1
    std::vector<std::unique_ptr<ParamAutomation>> autoLanes;
    std::atomic<float> effGain { 0.8f }, effPan { 0.0f }; // what the mixer last used (differs from gain/pan when automated)
    juce::String autoEditId;                              // UI only: id of the envelope the lane is editing ("" = none)
    bool autoCompView = false;                            // UI only: the lane shows every take in its own row (swipe to comp)
    bool audioCompView = false;                           // UI only: the lane shows the takes of a loop recording in rows (swipe to comp)
    double compFrom = 0.0, compTo = 0.0;                  // ...and the stretch of the timeline they cover

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

    // What rendering this track in one block needs and makes. Every track has its own, so tracks can be rendered at the same time on several threads.
    struct Work
    {
        std::vector<NoteEvent> noteEvents;      // the notes of the block (clips, routed notes, MIDI effects)
        std::vector<CcBlockEvent> ccEvents;
        juce::MidiBuffer midiScratch;
        std::vector<float> outL, outR;          // the track's stereo signal after its effects, before fader and pan
        bool rendered = false;                  // something was made this block
        bool audible = false;
        float peak = 0.0f;
        float gl0 = 1.0f, gr0 = 1.0f, gl1 = 1.0f, gr1 = 1.0f, g = 1.0f;   // the fader and pan at both ends of the block
    } work;
    bool busFed = false;                                  // an audible track wrote into the bus during this block
};

// A copy of everything the user can edit, used for undo/redo. Audio sample data is shared,
// not copied, so snapshots are cheap.
struct TrackState
{
    juce::String name;
    juce::uint32 colour = 0;
    int groupId = 0;
    int scaleRoot = 0, scaleType = 0, scaleChord = 0;
    bool scaleSnap = false;
    Track::Type type = Track::Type::Audio;
    std::vector<Clip> clips;
    std::vector<MidiClip> midiClips;
    std::vector<SubLane> subLanes;
    bool subLanesOpen = false;
    bool subAutoDone = false;
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
        AutoData data;
    };
    std::vector<LaneState> autoLanes;   // includes "volume" and "pan"
    bool hasSampler = false, hasKit = false;
    Sampler::State samplerState;                // the Simpler of a sampler track
    std::vector<DrumKit::PadState> padStates;   // the drum rack of a drum track
    bool sequencer = false;
    bool recordArmed = false;
    juce::String midiInputId;
    int midiInputChannel = 0;
    bool midiArmed = false;
    int inputFirst = 0, inputCount = 2;
    std::vector<std::pair<int, int>> midiOuts;   // (index of the target track, channel) of each MIDI route
    bool bus = false;
    int outputIndex = -1;                        // index of the bus this track plays into (-1 = master)
    bool frozen = false;
    int freezeTwinIndex = -1;
    int sidechainIndex = -1;                   // index of the track its compressor listens to (-1 = its own signal)
    struct SendState { int bus = -1; float level = 0.5f; bool preFader = false; };
    std::vector<SendState> sends;
    std::vector<ExternalRoute> externalOuts;
    std::vector<std::shared_ptr<PluginSlot>> midiFx;
    std::shared_ptr<DrumKit> kit;
    std::shared_ptr<Sampler> sampler;
    std::shared_ptr<PluginSlot> instrument;                 // plugins are shared with the live project,
    std::vector<std::shared_ptr<PluginSlot>> inserts;       // so undo restores which ones are present, not their settings
};

// A named part of the arrangement (verse, chorus...): from a bar, for a number of bars. Shown as a band on the ruler; sections can be looped,
// copied and swapped with their neighbour (the clips of the section go with it).
struct ArrSection
{
    std::string name;
    int startBar = 0;          // 0-based
    int bars = 4;
    juce::uint32 colour = 0;   // 0 = by position
};

// What plays in a stretch of the timeline, on every track: the clips (their start counted from the start of the stretch). Used to copy a
// section within a project and to take one to another project (tracks are found again by name and type).
struct RangeBundle
{
    struct Item
    {
        int trackIndex = 0;
        juce::String name;
        Track::Type type = Track::Type::Audio;
        std::vector<Clip> audio;
        std::vector<MidiClip> midi;
    };
    std::vector<Item> items;
    double length = 0.0;
};

// A folder of tracks: shown under one header with a name, that collapses and mutes or solos all of them (message thread only; the audio doesn't use it)
struct TrackGroup
{
    int id = 0;
    juce::String name;
    bool collapsed = false;
};

struct ProjectState
{
    std::vector<TrackGroup> groups;
    // The state of some plugins at that moment (only those whose own window was opened: what is done inside it is part of undo this way)
    std::vector<std::pair<std::shared_ptr<PluginSlot>, juce::MemoryBlock>> pluginStates;
    std::vector<ArrSection> sections;
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
    Track* duplicateTrack (const Track&);              // a copy right after the original: clips, settings, effects, sampler / drum kit (plugins are not copied here)
    bool moveTrack (Track&, int delta);                // -1 = one place up, +1 = one place down; false if it is already at the end

    // Track groups (folders). A track put in a group is moved next to its other members; empty groups disappear.
    std::vector<TrackGroup>& getGroups() { return groups; }
    const std::vector<TrackGroup>& getGroups() const { return groups; }
    int createGroup (const juce::String& name);                    // the new group's id
    void setTrackGroup (Track&, int groupId);                      // 0 = take it out of its group
    TrackGroup* findGroup (int id);
    std::vector<Track*> groupMembers (int id) const;
    // Sub-tracks of a MIDI track (see SubLane): replace the list; the mute / solo state decides which pitches are heard
    void setSubLanes (Track&, std::vector<SubLane>, bool open);
    void updateSubLaneMask (Track&);                  // after mute / solo changed (call with the engine lock)
    void setSubLaneVolume (Track&, int index, float volume);        // the sub-track's fader, 0 - 1.5 (velocity scale)
    void setSubLaneState (Track&, int index, int mute, int solo);   // -1 keeps a flag; 0 / 1 sets it

    // ---- Message thread: routing. A track plays into the master or into one bus, and can also send a share of its sound to buses.
    void setTrackOutput (Track&, Track* bus);                                  // nullptr = master; ignored for buses and for non-bus targets
    // ---- The master meter: loudness (LUFS) and spectrum of what is heard. They run only while analysisOn (the meter window is open).
    LoudnessMeter loudness;
    SpectrumTap spectrum;
    std::atomic<bool> analysisOn { false };

    // ---- A reference track: another piece of music (a finished song you want to sound like) that replaces the mix while you listen, with a
    // short crossfade, at a level you set (or matched to the loudness of your mix). It plays along with the transport. Not part of exports.
    bool loadReference (const juce::File&);
    void clearReference();
    bool hasReference() const { return referenceLoaded.load(); }
    juce::String getReferenceName() const;
    juce::File getReferenceFile() const;
    double getReferenceLufs() const { return referenceLufs.load(); }
    void setReferenceOn (bool on) { referenceOn = on && referenceLoaded.load(); }
    bool isReferenceOn() const { return referenceOn.load(); }
    void setReferenceGainDb (float db) { referenceGain = juce::Decibels::decibelsToGain (juce::jlimit (-30.0f, 20.0f, db)); referenceGainDb = juce::jlimit (-30.0f, 20.0f, db); }
    float getReferenceGainDb() const { return referenceGainDb.load(); }
    void setReferenceOffset (double seconds) { referenceOffset = seconds; }   // where in the project the reference starts (seconds)
    double getReferenceOffset() const { return referenceOffset.load(); }

    // A parameter by id ("volume", "pan", "mute", "solo", or an automation target id) set from a 0..1 position: what a MIDI controller does (message thread)
    bool setParameterNormalised (Track&, const juce::String& id, double position);
    void setSidechain (Track&, Track* source);           // the compressor of the track listens to source (null: its own signal)
    void updateSidechainTaps();                          // (callers hold the lock)
    void setSend (Track&, Track& bus, float level, bool preFader);             // level <= 0 removes the send
    bool getSend (const Track&, const Track& bus, float& level, bool& preFader) const;
    std::vector<Track*> getBuses() const;
    void removeAllTracks();
    int getNumTracks() const;
    Track* getTrack (int index) const;
    int indexOfTrack (const Track*) const;

    // ---- Message thread: drum tracks (MIDI tracks with a drum kit; their clips are MIDI clips)
    Track* addDrumTrack (const juce::String& name);

    // ---- Message thread: sampler tracks (MIDI tracks that play one sample, Simpler-style; their clips are ordinary MIDI clips)
    Track* addSamplerTrack (const juce::String& name);
    bool loadSamplerSample (Track&, const juce::File&);                      // reads an audio file into the track's sampler
    bool setSamplerSample (Track&, std::shared_ptr<const juce::AudioBuffer<float>>, double rate, const juce::String& name, const juce::File& file);   // e.g. from a clip
    bool loadDrumSample (Track&, int pad, const juce::File&);
    bool setSamplerStretch (Sampler&, double timeRatio);                    // time-stretch the sampler's sample (pitch kept); 1 = as loaded
    // The result of a stretch made in the background (Stretch::process on `original`); false if another sample was loaded meanwhile
    bool applySamplerStretch (Sampler&, std::shared_ptr<const juce::AudioBuffer<float>> original, std::shared_ptr<const juce::AudioBuffer<float>> result, double timeRatio);
    void resetDrumPad (Track&, int pad);
    bool loadDrumLayer (Track&, int pad, int layer, const juce::File&);   // an extra sample of a pad (velocity layer / round-robin), layer 0..2
    void clearDrumLayer (Track&, int pad, int layer);

    // ---- Message thread: audio clips (all return -1 / false when nothing was done)
    int addClipFromFile (Track&, const juce::File&, double startSeconds);   // new clip id
    int addClip (Track&, Clip);                                             // used when loading projects
    std::vector<Clip> getClips (const Track&) const;                        // snapshot, safe to read
    bool getClip (const Track&, int clipId, Clip& out) const;
    bool setClipRegion (Track&, int clipId, double start, double offset, double length);

    // Clip gain, volume curve and normalizing of audio clips.
    bool setClipGain (Track&, int clipId, float gain);
    bool setClipEnvelope (Track&, int clipId, std::vector<AutoPoint> points);   // sorted by time on the way in; empty = no curve
    bool normalizeClip (Track&, int clipId, float targetPeak = 0.97f);         // sets the gain so the loudest sample of the clip reaches targetPeak

    // Time-stretch and pitch-shift of an audio clip (Rubber Band, offline: this can take a while, so `progress` gets 0..1 and returns
    // false to cancel). stretch 2.0 = twice as long. The clip's length, fades and volume curve follow the new length. stretch 1 and pitch 0
    // put the original back. Results are cached on disk, so loading a project does not process them again.
    using ProgressFn = std::function<bool (double)>;
    bool processClip (Track&, int clipId, double stretch, double semitones, const ProgressFn& progress, bool formants = false);
    // Loading a project: processes the saved region of the clip's (just loaded, unprocessed) audio; the caller then sets offset and length.
    bool restoreProcessedClip (Track&, int clipId, double stretch, double semitones, double sourceStart, double sourceLength, const ProgressFn& progress,
                               const std::vector<WarpMarker>& warps = {}, bool formants = false);
    // Warps the clip: every marker pins a moment of the original audio to a place in the clip, the audio in between is time-stretched
    // (pitch kept). No markers put the original back. Like processClip this works on the clip's current region (or the region that was
    // processed before) of the original audio, and `progress` can cancel.
    bool warpClip (Track&, int clipId, std::vector<WarpMarker> markers, const ProgressFn& progress);
    // The original audio of a clip with the region that is warped / processed now: (audio, region start, region length in seconds)
    bool getWarpSource (const Track&, int clipId, std::shared_ptr<const juce::AudioBuffer<float>>& audio, double& regionStart, double& regionLength) const;

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
    bool compTakes (Track&, int takeIndex, double from, double to);   // swipe comping: take number `takeIndex` plays between the two times (the takes are split there)

    // ---- Message thread: MIDI clips
    int addMidiClip (Track&, double startSeconds, double lengthSeconds);
    std::vector<MidiClip> getMidiClips (const Track&) const;
    bool getMidiClip (const Track&, int clipId, MidiClip& out) const;
    bool isMidiClip (const Track&, int clipId) const;
    bool setMidiNotes (Track&, int clipId, std::vector<Note> notes);
    bool setMidiCcs (Track&, int clipId, std::vector<CcEvent> ccs);   // sorted by time on the way in
    bool setMidiClipLength (Track&, int clipId, double lengthSeconds);

    // ---- Message thread: the arrangement (sections, see ArrSection) and the editing of whole stretches of the timeline
    const std::vector<ArrSection>& getSections() const { return arrangement; }
    void setSections (std::vector<ArrSection> s) { arrangement = std::move (s); }
    void shiftClipsFrom (double fromSeconds, double deltaSeconds);                   // every clip that starts at or after `from` moves by `delta`
    int copyRange (double fromSeconds, double toSeconds, double destSeconds);        // copies what plays in [from, to) of every track to `dest`; the number of clips made
    RangeBundle captureRange (double fromSeconds, double toSeconds) const;           // what plays in [from, to), to keep or to take to another project
    // Puts a captured stretch at `dest`. Tracks are found by position (the same project) or by name and type, then by position, else a new
    // track is made (`created` counts those). Returns the number of clips made.
    int pasteRange (const RangeBundle&, double destSeconds, bool sameProject, int* created = nullptr);
    void swapRanges (double aFrom, double aTo, double bFrom, double bTo);            // adjacent stretches (a then b) change places, with their clips

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

    // Riding: arm an envelope, play, and move the fader / knob; every pass becomes a take that is comped into the envelope.
    //   Touch - records while you move the control (a take per touch), then the old automation takes over again
    //   Latch - starts at the first move and records until playback stops
    //   Write - records the control's value the whole time while playing
    void armAutomation (Track&, const juce::String& id, AutoRecMode mode);
    void disarmAutomation (Track&, const juce::String& id);
    bool isAutomationArmed (const Track&, const juce::String& id, AutoRecMode* mode = nullptr) const;
    bool isAutomationRecording (const Track&, const juce::String& id) const;   // a take is being captured right now
    void automationRecordTick();   // message thread, about 30 times a second
    int recordedTakeCounter = 0;      // grows with every take that riding creates (the program counts it as an edit)
    bool anyAutomationArmed() const { return ! autoRecs.empty(); }
    std::function<void()> beforeRecordedTake;   // called just before a ride becomes a take (the program takes its undo snapshot there)
    void finishAutomationRecording();   // closes the takes in progress (playback stopped)
    AutoData getAutomationData (const Track&, const juce::String& id) const;
    void setAutomationData (Track&, const juce::String& id, AutoData data);   // flattens into the envelope
    int addAutomationTake (Track&, const juce::String& id, double start, double end, std::vector<AutoPoint> points, const juce::String& name = {});
    void useAutomationTake (Track&, const juce::String& id, int takeId, double start, double end);   // that take plays in this range
    void deleteAutomationTake (Track&, const juce::String& id, int takeId);
    // Takes as blocks: move one in time (its comp segments go with it), trim it to a new range, or copy it
    void moveAutomationTake (Track&, const juce::String& id, int takeId, double deltaSeconds);
    void trimAutomationTake (Track&, const juce::String& id, int takeId, double newStart, double newEnd);
    int duplicateAutomationTake (Track&, const juce::String& id, int takeId, double offsetSeconds);

    // ---- Message thread: hosted plugins
    void setMidiOuts (Track& source, std::vector<MidiRoute> routes);   // empty = this track plays its own sound; only other MIDI tracks count
    // Hardware MIDI outputs: opens the devices and replaces the track's external routes. A route whose device
    // cannot be opened keeps port -1 (the caller can tell the user).
    void setExternalOuts (Track&, std::vector<ExternalRoute>);
    ExternalMidiSender& getExternalMidi() { return externalMidi; }
    void setOutputLatencyMs (double ms) { outputLatencyMs = ms; }   // MIDI to hardware leaves this much later, to line up with the audio

    // MIDI clock to hardware: while the transport runs, 24 clock ticks per quarter note (following the tempo map) go to these
    // outputs, with Start / Continue + Song Position when playback begins or jumps (a loop, a click on the ruler) and Stop when it
    // ends. `deviceIds` are MIDI output identifiers; an empty list switches the clock off.
    void setClockOutputs (const juce::StringArray& deviceIds);
    void setClockOffsetMs (double ms) { clockOffsetMs = ms; }       // shifts the clock earlier (-) or later (+) to line up with hardware
    void setClockPorts (std::vector<int> ports)                      // for tests: use these port numbers directly (see ExternalMidiSender::testSink)
    {
        const juce::ScopedLock sl (lock);
        clockPorts = std::move (ports);
        clockRunning = false;
    }

    // Round-trip latency measurement: plays a short burst on the first two outputs and listens on one input channel (the
    // enabled inputs are numbered from 0, as for recording). Connect an output to that input with a cable, or point a
    // microphone at the speakers. Poll latencyTestState(): 1 = running, 2 = recorded and ready for analyseLatencyTest().
    bool startLatencyTest (int inputChannel);
    int latencyTestState() const { return latencyState.load(); }
    int analyseLatencyTest();                                       // delay in samples; -1 if the burst was not heard. Ends the test.
    void cancelLatencyTest() { latencyState = 0; }
    void addMidiFx (Track&, std::shared_ptr<PluginSlot>);
    void removeMidiFx (Track&, int index);
    void preparePlugin (PluginSlot&);                           // layout + prepareToPlay for the current audio setup
    void setInstrument (Track&, std::shared_ptr<PluginSlot>);   // null = built-in synth
    void addInsert (Track&, std::shared_ptr<PluginSlot>);
    void removeInsert (Track&, int index);
    int getMaxBlockSize() const { return maxBlock; }

    // Undo/redo support. restoreState() replaces every Track object, so any Track* held
    // elsewhere is invalid afterwards (clip ids are kept).
    ProjectState captureState (const std::vector<std::shared_ptr<PluginSlot>>& pluginsToKeep = {}) const;
    std::shared_ptr<PluginSlot> findPluginSlot (const PluginSlot*) const;
    void restoreState (const ProjectState&);

    // Audition a note on a MIDI track's synth (works while the transport is stopped).
    // Safe to call from any thread (UI, MIDI input).
    void previewNote (const Track&, int pitch, bool on, float velocity = 0.8f);
    // A controller moved live (mod wheel, pedal, pitch bend...): goes to the track's plugin instrument or hardware.
    void previewController (const Track&, int controller, float value01);

    double getTotalLengthSeconds() const;
    double getRenderTailSeconds() const;   // extra time to render after the last clip (delay/reverb tails)

    // Mixes the project offline (faster than realtime) into a stereo WAV.
    struct RenderOptions
    {
        bool withMetronome = false;
        int bits = 24;                                  // 16, 24 or 32 (32 = floating point: never clips)
        int formatChoice = -1;                          // an index into AudioExport::choices() (WAV, FLAC, AIFF, MP3, Ogg); -1 = WAV with `bits`
        const Track* onlyTrack = nullptr;               // set: render just this track (a stem)
        bool masterProcessing = true;                   // false: no master volume / limiter (stems)
        bool noDelayCompensation = false;               // true: the track is not delayed to line up with plugins on other tracks (freeze)
        std::function<bool()> shouldCancel;             // polled between blocks; true stops the render
        std::function<void (double)> onProgress;        // 0..1, called from the rendering thread
    };
    bool renderToFile (const juce::File&, double lengthSeconds, bool withMetronome);
    bool renderToFile (const juce::File&, double lengthSeconds, const RenderOptions&);
    static int latencyOf (const Track& t) { return trackLatency (t); }   // samples of delay the plugins of the track add
    // Preview (the sample browser): a sound is played through the master output, apart from the project (not in exports). A new one replaces the old.
    void startPreview (std::shared_ptr<const juce::AudioBuffer<float>> buffer, double bufferRate, float gain = 0.8f);
    void stopPreview();
    bool isPreviewing() const { return previewActive.load(); }
    double getPreviewPosition() const { return previewPosition.load(); }   // seconds into the sound

    // Several CPU cores: the tracks of a block are rendered at the same time by this many worker threads (and the audio thread itself). 0 = all on the
    // audio thread. It is only used when the tracks together take a noticeable part of the block time (or when rendering to a file).
    void setWorkerThreads (int numWorkers);
    int getWorkerThreads() const { return workerThreads.load(); }
    std::atomic<bool> forceParallel { false };   // (tests) always use the workers when there are some

    // Message thread: true once when a plugin crashed while it was playing (it is switched off), with its name
    bool takeCrashedPlugin (juce::String& name);
    bool trackHasOutput (const Track&) const;       // whether the track would give a non-empty stem

    // Records the audio input (while the transport plays) straight to a 24-bit WAV on a background thread.
    // Several files can record at the same time, each from its own input channels (numbered among the device's
    // enabled input channels: 0 is the first enabled one).
    struct RecordTarget
    {
        int firstChannel = 0;
        int numChannels = 2;   // 1 or 2
        juce::File file;
    };
    // What has been recorded so far as peaks, for drawing the waveform while it forms: one value (the loudest sample) per `bucket` samples.
    // The audio thread appends; `count` says how many are valid.
    struct RecPeaks
    {
        std::vector<float> data;
        std::atomic<int> count { 0 };
        int bucket = 480;
        float cur = 0.0f;   // (audio thread)
        int fill = 0;
    };
    std::shared_ptr<const RecPeaks> getRecordingPeaks (int targetIndex) const   // message thread; null when there is no such recording
    {
        return juce::isPositiveAndBelow (targetIndex, (int) livePeaks.size()) ? livePeaks[(size_t) targetIndex] : nullptr;
    }
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


    // ---- Clip launcher (see EngineLauncher.cpp). Every track has one cell per scene; a cell holds a clip that is started with launchSlot()
    // and then repeats until another cell of the track is launched or the track is stopped. A launch waits for the next beat or bar
    // (the quantum), so everything stays in time. A launch also starts the transport when it is stopped. Stopping the transport ends all clips.
    enum class LaunchQuantum { None, Beat, Bar };
    std::atomic<int> launchQuantum { (int) LaunchQuantum::Bar };
    int getNumScenes() const;
    void setNumScenes (int);
    // fill / empty a cell. A clip from the timeline is copied into the cell (removeOriginal: moved).
    bool setSlotFromClip (Track&, int scene, int clipId, bool removeOriginal);
    bool setSlotFromFile (Track&, int scene, const juce::File&);
    bool newSlotMidi (Track&, int scene, double lengthSeconds);   // an empty MIDI pattern of that length
    bool moveSlotToTimeline (Track&, int scene, double atSeconds);   // the cell is emptied; the clip is on the timeline for editing
    void clearSlot (Track&, int scene);
    void renameSlot (Track&, int scene, const juce::String&);
    void launchSlot (Track&, int scene);
    void launchScene (int scene);
    void stopTrackSlots (Track&);
    void stopAllSlots();
    struct SlotState { int playing = -1; int queued = -1; bool stopQueued = false; double startsInSeconds = 0.0; double loopPosition = 0.0; };
    SlotState getSlotState (const Track&) const;
    std::vector<LaunchSlot> getSlots (const Track&) const;   // a copy, one per scene
    void play()  { playing = true; }
    void stop()  { playing = false; countInRemaining = 0; launcherClear = true; }
    void silenceEverything();   // transport stops, every note of every instrument is cut at once (the "STOP ALL" button)
    bool isPlaying() const { return playing; }

    // Count-in: call before play(). The transport then waits at the playhead while the metronome clicks `bars` bars (in the
    // meter and tempo at the playhead, always audible), and only then starts to run. Nothing is recorded during the count-in.
    void startCountIn (int bars);
    bool isCountingIn() const { return playing && countInRemaining.load() > 0; }
    int countInBeatsLeft() const;   // counted beats still to come (for the display)
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
    // (master->clipped: the mix went over 0 dBFS before the master limiter; latched until cleared)
    void clearClipIndicators()
    {
        master->clipped = false;
        const juce::ScopedLock sl (lock);
        for (auto& t : tracks)
        {
            t->clipped = false;
            t->inputClipped = false;
        }
    }

    // The master channel: the last stage of the mix. It is a Track without clips, so it has a volume
    // (gain), built-in effects, plugin inserts and automation like any other channel; mixBlock runs the
    // summed tracks through it before the limiter.
    Track& getMaster() { return *master; }
    const Track& getMaster() const { return *master; }
    bool isMaster (const Track& t) const { return &t == master.get(); }
    void setMasterLevel (float v) { masterLevel = v; master->level = v; }
    void resetMaster();   // back to defaults (a new project)

private:
    std::shared_ptr<juce::AudioBuffer<float>> processedAudio (const Clip& c, const juce::AudioBuffer<float>& src, double regionStart,
                                                              double regionLength, double stretch, double semitones, const ProgressFn&, bool formants);

    struct PreviewEvent
    {
        const Track* track = nullptr;
        int pitch = 0;
        bool on = false;
        float velocity = 0.8f;
        int controller = -1;   // >= 0: a controller message (CcEvent numbering) with `velocity` as its value, not a note
    };

    static juce::MidiMessage controllerMessage (int channel, int controller, float value01);

    // Caller must hold `lock`; adds into outL/outR (which must be pre-cleared).
    // `running` is false when only synth tails/previews should be rendered.
    void mixBlock (float* outL, float* outR, int numSamples, juce::int64 pos, bool withMetronome, bool running);
    void renderMidiTrack (Track&, int numSamples, juce::int64 pos, bool running, float* scratch);
    // The mix of one block, track by track (see mixBlock): stage one makes a track's sound in its own buffers, stage two adds it to the master or a bus.
    void renderTrackSignal (Track&, int numSamples, juce::int64 pos, bool running, bool anySolo, int maxLatency);
    void sumTrack (Track&, int numSamples, float* outL, float* outR);
    void renderGroup (std::vector<Track*>&, bool mayRunInParallel, int numSamples, juce::int64 pos, bool running, bool anySolo, int maxLatency, float* outL, float* outR);
    std::vector<Track*> groupTapped, groupPlain, groupBuses;
    ParallelRunner runner;
    std::atomic<int> workerThreads { 0 };
    std::vector<int> jobOrder;            // (the tracks of a group, the heaviest first)
    bool parallelOn = false;              // (audio thread) the workers were used in the last block   // (the tracks of the block, by group; reserved up front)
    void drainPreviewEvents();
    bool anySynthActive() const;
    void silenceSynths();
    struct AutoRec
    {
        Track* track = nullptr;
        juce::String id;
        AutoRecMode mode = AutoRecMode::Touch;
        bool touching = false, haveLast = false;
        float lastValue = 0.0f;
        double lastPos = 0.0, lastMoveMs = 0.0;
        std::vector<AutoPoint> pts;
    };
    std::vector<AutoRec> autoRecs;   // message thread only
    void finishAutoRec (AutoRec&);
    void setAutoOverride (Track&, const juce::String& id, bool on);
    AutoData* autoDataFor (Track&, const juce::String& id, std::vector<AutoPoint>** points);
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
    void renderDrumTrack (Track&, int numSamples, juce::int64 pos, bool running, float* outL, float* outR);
    void renderSamplerTrack (Track&, int numSamples, juce::int64 pos, bool running, float* outL, float* outR);
    void renderPluginInstrument (Track&, int numSamples, float* outL, float* outR);
    void processInsert (PluginSlot&, float* l, float* r, int numSamples);
    bool processPlugin (PluginSlot&, juce::AudioBuffer<float>&, juce::MidiBuffer&);   // processBlock behind the crash guard; false = the plugin crashed (now or before)
    std::atomic<PluginSlot*> crashedPlugin { nullptr };
    std::shared_ptr<const juce::AudioBuffer<float>> previewBuffer;   // (guarded by `lock`)
    double previewPhase = 0.0, previewRate = 44100.0;
    float previewGain = 0.8f;
    std::atomic<bool> previewActive { false };
    std::atomic<double> previewPosition { 0.0 };                                // the first plugin that crashed and was not reported yet

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
    int numScenes = 8;                              // clip launcher rows, guarded by `lock`
    std::atomic<bool> launcherClear { false };      // the transport was stopped: the audio thread drops what the launcher was playing before it next runs
    double launchBoundary (double now, LaunchQuantum q) const;   // lock held: the time (seconds) of the next beat / bar / now
    void endLaunchedAt (Track&, double at);        // lock held: what plays on the track ends at `at`, what was queued after it is dropped

    juce::TimeSliceThread writerThread { "UFODAW audio writer" };
    struct Recorder
    {
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        int first = 0, count = 2;
        std::shared_ptr<RecPeaks> peaks;
    };
    std::vector<std::shared_ptr<RecPeaks>> livePeaks;   // the same peaks, for the message thread (in the order of the targets)
    std::vector<Recorder> recorders;               // guarded by recLock; the audio thread only try-locks it
    std::atomic<bool> recordingActive { false };
    // MIDI clock (guarded by `lock`; the audio thread reads it while it holds it)
    std::vector<int> clockPorts;
    std::atomic<double> clockOffsetMs { 0.0 };
    bool clockRunning = false;
    juce::int64 clockLastEnd = 0;
    void emitClock (juce::int64 pos, int n, bool running);

    // latency test
    std::atomic<int> latencyState { 0 };               // 0 idle, 1 running, 2 recorded
    std::vector<float> latencyCapture;
    juce::int64 latencyPos = 0, latencyTotal = 0, latencyBurstStart = 0;
    int latencyChannel = 0;
    static constexpr int latencyBurstLength = 192;
    static float latencyBurstSample (int i);
    void runLatencyTest (const juce::AudioSourceChannelInfo&);

    std::atomic<juce::int64> countInRemaining { 0 };   // samples of count-in still to play
    juce::int64 countInTotal = 0;
    double countInUnitSamples = 1.0;                   // one counted beat
    int countInNumerator = 4;                          // beats per bar (the first of each is the high click)
    void addCountInClicks (float* outL, float* outR, int n);

    std::atomic<bool> punchDone { false };
    std::atomic<juce::int64> recFirstSample { -1 };
    int nextTakeGroup = 1;
    std::vector<TrackGroup> groups;
    int nextGroupId = 1;

    std::vector<ArrSection> arrangement;
    std::array<float, tunerRingSize> tunerRing {};
    std::atomic<int> tunerChannel { -1 };
    std::atomic<juce::int64> tunerWritePos { 0 };

    // The device input of the block being mixed, copied before the output buffer is cleared (all channels): what
    // armed tracks with Monitor on play back through their effects and fader.
    juce::AudioBuffer<float> inputBuf;
    int inputChannelsCopied = 0;
    int inputOffset = 0;                            // where the sub-block being mixed starts inside the callback's block
    int inputBlockSize = 0;                         // samples in the callback's block
    void updateInputMeters();
    bool anyMonitoring() const;
    juce::SpinLock recLock;

    // UI -> audio thread queue for note previews
    static constexpr int previewQueueSize = 64;
    juce::SpinLock previewWriteLock;   // the fifo has one writer at a time: UI thread or MIDI thread
    juce::AbstractFifo previewFifo { previewQueueSize };
    std::array<PreviewEvent, previewQueueSize> previewQueue;

    int maxBlock = 4096;                   // largest block hosted plugins are prepared for

    std::vector<float> synthScratch;       // mono synth output
    std::vector<float> trackL, trackR;     // one track's stereo signal before fader/pan
    juce::int64 lastBlockEnd = -1;
    bool wasRunning = false;

    ExternalMidiSender externalMidi;
    std::atomic<double> outputLatencyMs { 0.0 };
    double blockStartMs = 0.0;                      // the millisecond clock at the start of the block being mixed

    std::atomic<bool> offlineRendering { false };   // an export owns the mixer; the audio callback stays silent
    std::shared_ptr<const juce::AudioBuffer<float>> reference;   // (guarded by lock)
    double referenceRate = 44100.0;
    juce::String referenceName;
    juce::File referenceFile;
    std::atomic<bool> referenceLoaded { false }, referenceOn { false };
    std::atomic<double> referenceLufs { -120.0 }, referenceOffset { 0.0 };
    std::atomic<float> referenceGain { 1.0f }, referenceGainDb { 0.0f };
    float referenceMix = 0.0f;   // (audio thread) 0 = the mix, 1 = the reference
    void applyReference (float* l, float* r, int n, juce::int64 pos, bool running);
    const Track* renderOnly = nullptr;              // set during an offline render of a single track
    bool renderMaster = true;
    bool renderNoPdc = false;

    std::atomic<bool> playing { false };
    std::atomic<juce::int64> positionSamples { 0 };
    double sampleRate = 44100.0;
};
