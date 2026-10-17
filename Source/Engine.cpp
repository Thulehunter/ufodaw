#include "Engine.h"
#include <algorithm>
#include <cmath>

namespace
{
    // Room for half a second of plugin delay compensation per track
    void sizeDelayBuffers (Track& t, double rate)
    {
        const size_t size = (size_t) (rate * 0.5) + 2;
        if (t.pdcL.size() != size)
        {
            t.pdcL.assign (size, 0.0f);
            t.pdcR.assign (size, 0.0f);
            t.pdcPos = 0;
            t.pdcDelay = 0;
        }
    }
}

Engine::Engine()
{
    formats.registerBasicFormats();
    writerThread.startThread();
    noteEvents.reserve (2048);
    midiScratch.ensureSize (16384);
    emptyMidi.ensureSize (64);
    synthScratch.assign (16384, 0.0f);
    trackL.assign (16384, 0.0f);
    trackR.assign (16384, 0.0f);
    inputBuf.setSize (64, 16384);   // room for the input channels of the biggest interfaces
    inputBuf.clear();

    master = std::make_unique<Track>();
    master->name = "Master";
    master->gain = 0.9f;
    master->fxState = std::make_unique<TrackFx>();
    master->fxState->prepare (sampleRate);
}

void Engine::resetMaster()
{
    const juce::ScopedLock sl (lock);
    TrackState defaults;
    defaults.name = "Master";
    defaults.gain = 0.9f;
    defaults.autoLanes.push_back ({ "volume", {}, true });
    defaults.autoLanes.push_back ({ "pan", {}, true });
    FxParams blank;   // default effect settings, all off
    blank.forEachFloat ([&] (const char*, std::atomic<float>& v) { defaults.fxFloats.push_back (v.load()); });
    blank.forEachBool ([&] (const char*, std::atomic<bool>& v) { defaults.fxBools.push_back (v.load() ? 1 : 0); });
    applyTrackState (*master, defaults);
}

Engine::~Engine()
{
    stopRecording();
    writerThread.stopThread (2000);
}

void Engine::prepareToPlay (int samplesPerBlock, double newRate)
{
    const juce::ScopedLock sl (lock);
    sampleRate = newRate;
    maxBlock = juce::jmax (4096, samplesPerBlock * 2);
    if (inputBuf.getNumSamples() < samplesPerBlock * 2)
    {
        inputBuf.setSize (64, samplesPerBlock * 2);
        inputBuf.clear();
    }
    if ((int) synthScratch.size() < samplesPerBlock * 2)
    {
        synthScratch.assign ((size_t) samplesPerBlock * 2, 0.0f);
        trackL.assign ((size_t) samplesPerBlock * 2, 0.0f);
        trackR.assign ((size_t) samplesPerBlock * 2, 0.0f);
    }
    master->fxState->prepare (newRate);
    for (auto& p : master->inserts)
        preparePlugin (*p);

    for (auto& t : tracks)
    {
        if (t->bus && t->busInL.size() != trackL.size())
        {
            t->busInL.assign (trackL.size(), 0.0f);
            t->busInR.assign (trackL.size(), 0.0f);
        }
        if (t->voices != nullptr)
            t->voices->prepare (newRate);
        if (t->kit != nullptr)
            t->kit->prepare (newRate);
        sizeDelayBuffers (*t, newRate);
        t->fxState->prepare (newRate);
        if (t->instrument != nullptr)
            preparePlugin (*t->instrument);
        for (auto& p : t->inserts)
            preparePlugin (*p);
        for (auto& p : t->midiFx)
            preparePlugin (*p);
    }
}

//==============================================================================
// Hosted plugins

juce::Optional<juce::AudioPlayHead::PositionInfo> Engine::PlayHead::getPosition() const
{
    const double rate = juce::jmax (1.0, engine.sampleRate);
    const auto samples = this->samples.load();
    const double seconds = (double) samples / rate;

    const juce::ScopedLock sl (engine.lock);   // (the audio thread already holds it while it mixes; the tempo map is edited under it)
    const auto& map = engine.tempoMap;
    const auto& section = map.sectionAt (seconds);
    const double beats = map.secondsToBeats (seconds);
    double intoBar = 0.0;
    map.barAtBeat (beats, &intoBar);

    PositionInfo info;
    info.setBpm (section.bpm);
    info.setTimeSignature (TimeSignature { section.numerator, section.denominator });
    info.setTimeInSamples (samples);
    info.setTimeInSeconds (seconds);
    info.setPpqPosition (beats);
    info.setPpqPositionOfLastBarStart (beats - intoBar);
    info.setIsPlaying (playing.load());
    info.setIsRecording (false);
    info.setIsLooping (engine.loopEnabled.load());
    if (engine.loopEnabled && engine.loopEnd > engine.loopStart)
        info.setLoopPoints (LoopPoints { map.secondsToBeats (engine.loopStart), map.secondsToBeats (engine.loopEnd) });
    return info;
}

void Engine::setMidiOuts (Track& source, std::vector<MidiRoute> routes)
{
    const juce::ScopedLock sl (lock);

    // one route per target track, only to other MIDI tracks
    std::vector<MidiRoute> valid;
    for (auto& r : routes)
        if (r.target != nullptr && r.target != &source && r.target->type == Track::Type::Midi
            && std::none_of (valid.begin(), valid.end(), [&] (const MidiRoute& v) { return v.target == r.target; }))
            valid.push_back ({ r.target, juce::jlimit (0, 16, r.channel) });

    // whatever was playing over the old routes fades out
    for (auto& old : source.midiOuts)
        releaseAllNotes (*old.target);
    releaseAllNotes (source);
    source.midiOuts = std::move (valid);
    source.routedMidi.clear();
}

//==============================================================================
// Hardware MIDI outputs

void Engine::setExternalOuts (Track& t, std::vector<ExternalRoute> routes)
{
    for (auto& r : routes)   // open the devices (message thread, before the lock: this can take a moment)
    {
        r.port = externalMidi.openDevice (r.deviceId);
        if (r.port < 0 && r.deviceName.isNotEmpty())
            r.port = externalMidi.openDeviceByName (r.deviceName);
        r.channel = juce::jlimit (0, 16, r.channel);
        r.outChannel = juce::jlimit (0, 16, r.outChannel);
    }

    const juce::ScopedLock sl (lock);
    externalAllNotesOff (t);   // anything still sounding on the old routes is released
    t.externalOuts = std::move (routes);
}

// Sends one note message (or any short message) to the track's hardware routes, `sampleOffset` samples into the current block.
void Engine::forwardExternal (const Track& t, const juce::MidiMessage& m, int sampleOffset)
{
    if (t.externalOuts.empty() || offlineRendering)   // exports never play the hardware
        return;

    const double at = blockStartMs + 1000.0 * (double) sampleOffset / sampleRate + outputLatencyMs;
    for (auto& route : t.externalOuts)
    {
        if (route.port < 0 || (route.channel != 0 && m.getChannel() != route.channel))
            continue;

        auto copy = m;
        if (route.outChannel > 0)
            copy.setChannel (route.outChannel);
        externalMidi.send (route.port, copy, at);
    }
}

// "All notes off" on the channels a track sends to, so a stopped sequencer never leaves a hardware note hanging.
void Engine::externalAllNotesOff (const Track& t)
{
    // Stamped just before the current block starts, so at the start of playback it goes out ahead of the first note
    // of that block (a later stamp would cut that note off again).
    const double now = juce::jmin (juce::Time::getMillisecondCounterHiRes(), blockStartMs) - 0.5;
    for (auto& route : t.externalOuts)
    {
        if (route.port < 0)
            continue;
        for (int ch = route.outChannel > 0 ? route.outChannel : 1; ch <= (route.outChannel > 0 ? route.outChannel : 16); ++ch)
            externalMidi.send (route.port, juce::MidiMessage::allNotesOff (ch), now);
    }
}

void Engine::addMidiFx (Track& t, std::shared_ptr<PluginSlot> slot)
{
    const juce::ScopedLock sl (lock);
    t.midiFx.push_back (std::move (slot));
}

void Engine::removeMidiFx (Track& t, int index)
{
    std::shared_ptr<PluginSlot> removed;   // released after the lock
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (index, (int) t.midiFx.size()))
            return;
        removed = std::move (t.midiFx[(size_t) index]);
        t.midiFx.erase (t.midiFx.begin() + index);
        t.autoLanes.erase (std::remove_if (t.autoLanes.begin(), t.autoLanes.end(),
                                           [&] (auto& lane) { return lane->plugin == removed; }),
                           t.autoLanes.end());
        releaseAllNotes (t);
    }
}

// Makes a plugin stereo where it can, prepares it for the current sample rate/block size and sizes its buffer.
void Engine::preparePlugin (PluginSlot& s)
{
    auto& p = *s.instance;
    p.setPlayHead (&playHead);

    auto layout = p.getBusesLayout();
    if (p.getBusCount (true) > 0)  layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    if (p.getBusCount (false) > 0) layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    if (p.checkBusesLayoutSupported (layout))
        p.setBusesLayout (layout);

    p.prepareToPlay (sampleRate, maxBlock);
    s.scratch.setSize (juce::jmax (2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()), maxBlock);
    s.scratch.clear();
}

void Engine::setInstrument (Track& t, std::shared_ptr<PluginSlot> slot)
{
    const juce::ScopedLock sl (lock);
    if (t.type != Track::Type::Midi)
        return;
    if (t.instrument != nullptr)   // automation of the old plugin's parameters goes with it
        t.autoLanes.erase (std::remove_if (t.autoLanes.begin(), t.autoLanes.end(),
                                           [&] (auto& lane) { return lane->plugin == t.instrument; }),
                           t.autoLanes.end());
    t.instrument = std::move (slot);
    t.liveMidi.clear();
    t.pluginTailSamples = 0.0;
    if (t.voices != nullptr)
        t.voices->reset();
}

void Engine::addInsert (Track& t, std::shared_ptr<PluginSlot> slot)
{
    const juce::ScopedLock sl (lock);
    t.inserts.push_back (std::move (slot));
}

void Engine::removeInsert (Track& t, int index)
{
    std::shared_ptr<PluginSlot> removed;   // released after the lock, so a plugin is never destroyed under it
    {
        const juce::ScopedLock sl (lock);
        if (! juce::isPositiveAndBelow (index, (int) t.inserts.size()))
            return;
        removed = std::move (t.inserts[(size_t) index]);
        t.inserts.erase (t.inserts.begin() + index);
        t.autoLanes.erase (std::remove_if (t.autoLanes.begin(), t.autoLanes.end(),
                                           [&] (auto& lane) { return lane->plugin == removed; }),
                           t.autoLanes.end());
    }
}

//==============================================================================
// Recording

bool Engine::startRecording (const std::vector<RecordTarget>& targets)
{
    stopRecording();

    std::vector<Recorder> fresh;
    for (auto& target : targets)
    {
        target.file.getParentDirectory().createDirectory();
        target.file.deleteFile();

        std::unique_ptr<juce::FileOutputStream> stream (target.file.createOutputStream());
        if (stream == nullptr)
            return false;   // (the recorders made so far are closed again when `fresh` goes out of scope)

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate,
                                                                             (unsigned int) juce::jlimit (1, 2, target.numChannels), 24, {}, 0));
        if (writer == nullptr)
            return false;
        stream.release(); // the writer owns the stream now

        Recorder r;
        r.writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), writerThread, 32768);
        r.first = juce::jmax (0, target.firstChannel);
        r.count = juce::jlimit (1, 2, target.numChannels);
        fresh.push_back (std::move (r));
    }

    punchDone = false;
    recFirstSample = -1;
    {
        const juce::SpinLock::ScopedLockType sl (recLock);
        recorders.swap (fresh);
    }
    recordingActive = ! targets.empty();
    return ! targets.empty();
}

void Engine::stopRecording()
{
    recordingActive = false;
    std::vector<Recorder> old;
    {
        const juce::SpinLock::ScopedLockType sl (recLock);
        old.swap (recorders);
    }
    old.clear();   // flushes what is left and closes the files
}

bool Engine::readTunerSamples (float* dest, int n) const
{
    if (tunerChannel.load() < 0 || n <= 0 || n > tunerRingSize)
        return false;
    const auto end = tunerWritePos.load();
    for (int i = 0; i < n; ++i)
        dest[i] = tunerRing[(size_t) ((end - n + i + 8 * (juce::int64) tunerRingSize) % tunerRingSize)];
    return true;
}

bool Engine::anyMonitoring() const
{
    for (auto& t : tracks)
        if (t->type == Track::Type::Audio && t->recordArmed && t->monitor)
            return true;
    return false;
}

//==============================================================================
// Tracks

Track* Engine::addTrack (const juce::String& name, Track::Type type, bool asBus)
{
    auto track = std::make_unique<Track>();
    track->name = name;
    track->type = type;
    track->bus = asBus && type == Track::Type::Audio;
    if (track->bus)
    {
        track->busInL.assign (trackL.size(), 0.0f);
        track->busInR.assign (trackL.size(), 0.0f);
    }
    track->routedMidi.ensureSize (4096);
    sizeDelayBuffers (*track, sampleRate);
    track->fxState = std::make_unique<TrackFx>();
    track->fxState->prepare (sampleRate);
    if (type == Track::Type::Midi)
    {
        track->voices = std::make_unique<Synth>();
        track->voices->prepare (sampleRate);
    }

    auto* raw = track.get();
    const juce::ScopedLock sl (lock);
    tracks.push_back (std::move (track));
    return raw;
}

Track* Engine::addDrumTrack (const juce::String& name)
{
    auto* t = addTrack (name, Track::Type::Midi);
    auto kit = std::make_shared<DrumKit>();
    kit->prepare (sampleRate);

    const juce::ScopedLock sl (lock);
    t->kit = std::move (kit);
    return t;
}

bool Engine::loadDrumSample (Track& t, int pad, const juce::File& file)
{
    if (t.kit == nullptr || ! juce::isPositiveAndBelow (pad, DrumKit::numPads))
        return false;

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return false;

    const int numChannels = (int) juce::jmin<juce::uint32> (reader->numChannels, 2);
    const int length = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 30.0));   // one-shots: 30 s is plenty
    auto buffer = std::make_shared<juce::AudioBuffer<float>> (numChannels, length);
    reader->read (buffer.get(), 0, length, 0, true, true);

    const juce::ScopedLock sl (lock);
    t.kit->setSample (pad, std::move (buffer), reader->sampleRate, file);
    return true;
}

void Engine::resetDrumPad (Track& t, int pad)
{
    if (t.kit == nullptr || ! juce::isPositiveAndBelow (pad, DrumKit::numPads))
        return;
    const juce::ScopedLock sl (lock);
    t.kit->resetPad (pad);
}

void Engine::setTrackOutput (Track& t, Track* bus)
{
    const juce::ScopedLock sl (lock);
    if (t.bus || (bus != nullptr && (! bus->bus || bus == &t)))
        return;
    t.output = bus;
}

void Engine::setSend (Track& t, Track& bus, float level, bool preFader)
{
    const juce::ScopedLock sl (lock);
    if (t.bus || ! bus.bus || &bus == &t)
        return;

    for (auto it = t.sends.begin(); it != t.sends.end(); ++it)
        if (it->bus == &bus)
        {
            if (level <= 0.0f)
                t.sends.erase (it);
            else
            {
                it->level = level;
                it->preFader = preFader;
            }
            return;
        }
    if (level > 0.0f)
        t.sends.push_back ({ &bus, level, preFader });
}

bool Engine::getSend (const Track& t, const Track& bus, float& level, bool& preFader) const
{
    const juce::ScopedLock sl (lock);
    for (auto& s : t.sends)
        if (s.bus == &bus)
        {
            level = s.level;
            preFader = s.preFader;
            return true;
        }
    return false;
}

std::vector<Track*> Engine::getBuses() const
{
    const juce::ScopedLock sl (lock);
    std::vector<Track*> out;
    for (auto& t : tracks)
        if (t->bus)
            out.push_back (t.get());
    return out;
}

void Engine::removeTrack (Track* t)
{
    const juce::ScopedLock sl (lock);
    for (auto& other : tracks)
    {
        other->midiOuts.erase (std::remove_if (other->midiOuts.begin(), other->midiOuts.end(),
                                               [t] (const MidiRoute& r) { return r.target == t; }),
                               other->midiOuts.end());   // nobody keeps sending to a track that is going away
        other->sends.erase (std::remove_if (other->sends.begin(), other->sends.end(),
                                            [t] (const Track::Send& s) { return s.bus == t; }),
                            other->sends.end());
        if (other->output == t)
            other->output = nullptr;   // what played into a removed bus goes to the master again
    }
    tracks.erase (std::remove_if (tracks.begin(), tracks.end(),
                                  [t] (auto& p) { return p.get() == t; }),
                  tracks.end());
}

void Engine::removeAllTracks()
{
    const juce::ScopedLock sl (lock);
    tracks.clear();
}

int Engine::getNumTracks() const
{
    const juce::ScopedLock sl (lock);
    return (int) tracks.size();
}

Track* Engine::getTrack (int index) const
{
    const juce::ScopedLock sl (lock);
    return juce::isPositiveAndBelow (index, (int) tracks.size()) ? tracks[(size_t) index].get() : nullptr;
}

int Engine::indexOfTrack (const Track* t) const
{
    const juce::ScopedLock sl (lock);
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].get() == t)
            return (int) i;
    return -1;
}

//==============================================================================
// Clips

Clip* Engine::findClip (Track& t, int id)
{
    for (auto& c : t.clips)
        if (c.id == id)
            return &c;
    return nullptr;
}

const Clip* Engine::findClip (const Track& t, int id) const
{
    for (auto& c : t.clips)
        if (c.id == id)
            return &c;
    return nullptr;
}

MidiClip* Engine::findMidiClip (Track& t, int id)
{
    for (auto& c : t.midiClips)
        if (c.id == id)
            return &c;
    return nullptr;
}

const MidiClip* Engine::findMidiClip (const Track& t, int id) const
{
    for (auto& c : t.midiClips)
        if (c.id == id)
            return &c;
    return nullptr;
}

int Engine::addClipFromFile (Track& track, const juce::File& file, double startSeconds)
{
    if (track.type != Track::Type::Audio)
        return -1;

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return -1;

    const int numChannels = (int) juce::jmin<juce::uint32> (reader->numChannels, 2);
    auto buffer = std::make_shared<juce::AudioBuffer<float>> (numChannels, (int) reader->lengthInSamples);
    reader->read (buffer.get(), 0, (int) reader->lengthInSamples, 0, true, true);

    Clip c;
    c.audio = std::move (buffer);
    c.fileRate = reader->sampleRate;
    c.sourceFile = file;
    c.start = juce::jmax (0.0, startSeconds);
    c.offset = 0.0;
    c.length = c.sourceLength();
    return addClip (track, std::move (c));
}

int Engine::addClip (Track& track, Clip c)
{
    const juce::ScopedLock sl (lock);
    c.id = nextClipId++;
    track.clips.push_back (std::move (c));
    return track.clips.back().id;
}

bool Engine::setClipFades (Track& t, int id, double fadeIn, double fadeOut)
{
    const juce::ScopedLock sl (lock);
    auto* c = findClip (t, id);
    if (c == nullptr)
        return false;
    c->fadeIn = juce::jlimit (0.0, c->length, fadeIn);
    c->fadeOut = juce::jlimit (0.0, juce::jmax (0.0, c->length - c->fadeIn), fadeOut);
    return true;
}

bool Engine::setClipReverse (Track& t, int id, bool reverse)
{
    const juce::ScopedLock sl (lock);
    auto* c = findClip (t, id);
    if (c == nullptr)
        return false;
    c->reverse = reverse;
    return true;
}

// Every clip that partly overlaps this one gets a crossfade over the overlap: the earlier clip fades out, the later one fades in.
int Engine::crossfadeOverlaps (Track& t, int id)
{
    const juce::ScopedLock sl (lock);
    auto* c = findClip (t, id);
    if (c == nullptr)
        return 0;

    int count = 0;
    for (auto& other : t.clips)
    {
        if (other.id == c->id || ! other.audible() || ! c->audible())
            continue;
        auto& a = other.start <= c->start ? other : *c;   // the earlier clip
        auto& b = other.start <= c->start ? *c : other;
        const double overlap = a.end() - b.start;
        if (overlap < 0.002 || b.end() <= a.end() + 0.001 || a.start >= b.start)
            continue;   // not overlapping, or one clip lies inside the other
        a.fadeOut = juce::jmin (overlap, a.length - a.fadeIn);
        b.fadeIn = juce::jmin (overlap, b.length - b.fadeOut);
        ++count;
    }
    return count;
}

int Engine::addTakes (Track& track, std::vector<Clip> clips)
{
    if (clips.empty())
        return -1;
    if (clips.size() == 1)
        return addClip (track, std::move (clips[0]));

    const juce::ScopedLock sl (lock);
    const int group = nextTakeGroup++;
    int activeId = -1;
    for (size_t i = 0; i < clips.size(); ++i)
    {
        auto c = std::move (clips[i]);
        c.id = nextClipId++;
        c.takeGroup = group;
        c.takeIndex = (int) i + 1;
        c.takeActive = i + 1 == clips.size();   // the newest take plays
        if (c.takeActive)
            activeId = c.id;
        track.clips.push_back (std::move (c));
    }
    return activeId;
}

std::vector<Clip> Engine::getTakes (const Track& t, int id) const
{
    const juce::ScopedLock sl (lock);
    std::vector<Clip> out;
    const auto* c = findClip (t, id);
    if (c == nullptr)
        return out;
    if (c->takeGroup == 0)
        return { *c };
    for (auto& other : t.clips)
        if (other.takeGroup == c->takeGroup)
            out.push_back (other);
    std::sort (out.begin(), out.end(), [] (auto& a, auto& b) { return a.takeIndex < b.takeIndex; });
    return out;
}

bool Engine::setActiveTake (Track& t, int id)
{
    const juce::ScopedLock sl (lock);
    auto* c = findClip (t, id);
    if (c == nullptr || c->takeGroup == 0)
        return false;
    for (auto& other : t.clips)
        if (other.takeGroup == c->takeGroup)
            other.takeActive = other.id == id;
    return true;
}

bool Engine::deleteTake (Track& t, int id)
{
    const juce::ScopedLock sl (lock);
    auto* c = findClip (t, id);
    if (c == nullptr)
        return false;
    const int group = c->takeGroup;
    const bool wasActive = c->takeActive;
    t.clips.erase (t.clips.begin() + (c - t.clips.data()));
    if (group == 0)
        return true;

    Clip* last = nullptr;
    int count = 0;
    for (auto& other : t.clips)
        if (other.takeGroup == group)
        {
            ++count;
            if (last == nullptr || other.takeIndex > last->takeIndex)
                last = &other;
        }
    if (count == 1)
    {
        last->takeGroup = 0;
        last->takeIndex = 0;
        last->takeActive = true;
    }
    else if (wasActive && last != nullptr)
    {
        last->takeActive = true;
    }
    return true;
}

bool Engine::flattenTakes (Track& t, int id)
{
    const juce::ScopedLock sl (lock);
    auto* c = findClip (t, id);
    if (c == nullptr || c->takeGroup == 0)
        return false;
    const int group = c->takeGroup;
    t.clips.erase (std::remove_if (t.clips.begin(), t.clips.end(),
                                   [id, group] (auto& o) { return o.takeGroup == group && o.id != id; }),
                   t.clips.end());
    if (auto* keep = findClip (t, id))
    {
        keep->takeGroup = 0;
        keep->takeIndex = 0;
        keep->takeActive = true;
    }
    return true;
}

void Engine::setClipTake (Track& t, int id, int group, int index, bool active)
{
    const juce::ScopedLock sl (lock);
    if (auto* c = findClip (t, id))
    {
        c->takeGroup = group;
        c->takeIndex = index;
        c->takeActive = active;
        nextTakeGroup = juce::jmax (nextTakeGroup, group + 1);
    }
}

std::vector<Clip> Engine::getClips (const Track& t) const
{
    const juce::ScopedLock sl (lock);
    return t.clips;
}

bool Engine::getClip (const Track& t, int id, Clip& out) const
{
    const juce::ScopedLock sl (lock);
    if (auto* c = findClip (t, id))
    {
        out = *c;
        return true;
    }
    return false;
}

bool Engine::setClipRegion (Track& t, int id, double start, double offset, double length)
{
    const juce::ScopedLock sl (lock);
    auto* c = findClip (t, id);
    if (c == nullptr)
        return false;

    c->start = juce::jmax (0.0, start);
    c->offset = juce::jlimit (0.0, c->sourceLength(), offset);
    c->length = juce::jlimit (0.0, c->sourceLength() - c->offset, length);
    return true;
}

//==============================================================================
// MIDI clips

int Engine::addMidiClip (Track& t, double start, double length)
{
    if (t.type != Track::Type::Midi)
        return -1;

    MidiClip c;
    c.start = juce::jmax (0.0, start);
    c.length = juce::jmax (0.1, length);

    const juce::ScopedLock sl (lock);
    c.id = nextClipId++;
    t.midiClips.push_back (std::move (c));
    return t.midiClips.back().id;
}

std::vector<MidiClip> Engine::getMidiClips (const Track& t) const
{
    const juce::ScopedLock sl (lock);
    return t.midiClips;
}

bool Engine::getMidiClip (const Track& t, int id, MidiClip& out) const
{
    const juce::ScopedLock sl (lock);
    if (auto* c = findMidiClip (t, id))
    {
        out = *c;
        return true;
    }
    return false;
}

bool Engine::isMidiClip (const Track& t, int id) const
{
    const juce::ScopedLock sl (lock);
    return findMidiClip (t, id) != nullptr;
}

bool Engine::setMidiNotes (Track& t, int id, std::vector<Note> notes)
{
    const juce::ScopedLock sl (lock);
    auto* c = findMidiClip (t, id);
    if (c == nullptr)
        return false;
    c->notes = std::move (notes);
    return true;
}

bool Engine::setMidiClipLength (Track& t, int id, double length)
{
    const juce::ScopedLock sl (lock);
    auto* c = findMidiClip (t, id);
    if (c == nullptr)
        return false;
    c->length = juce::jmax (0.1, length);
    return true;
}

//==============================================================================
// Editing shared by audio and MIDI clips

bool Engine::moveClip (Track& t, int id, double newStart)
{
    const juce::ScopedLock sl (lock);
    if (auto* c = findClip (t, id))
    {
        // the takes of a group move together
        const double delta = juce::jmax (0.0, newStart) - c->start;
        const int group = c->takeGroup;
        c->start += delta;
        if (group != 0)
            for (auto& other : t.clips)
                if (other.takeGroup == group && other.id != id)
                    other.start = juce::jmax (0.0, other.start + delta);
        return true;
    }
    if (auto* m = findMidiClip (t, id))
    {
        m->start = juce::jmax (0.0, newStart);
        return true;
    }
    return false;
}

int Engine::splitClip (Track& t, int id, double at)
{
    constexpr double minPart = 0.01;
    const juce::ScopedLock sl (lock);

    if (auto* c0 = findClip (t, id))
    {
        if (at <= c0->start + minPart || at >= c0->end() - minPart)
            return -1;

        // Every take of the group that spans the cut is split, and the right-hand parts form a new group: each
        // stretch of a comped recording can then pick its own take.
        std::vector<int> ids { id };
        const int oldGroup = c0->takeGroup;
        if (oldGroup != 0)
            for (auto& other : t.clips)
                if (other.takeGroup == oldGroup && other.id != id)
                    ids.push_back (other.id);
        const int newGroup = oldGroup != 0 ? nextTakeGroup++ : 0;

        int result = -1;
        for (const int cid : ids)
        {
            auto* c = findClip (t, cid);
            if (c == nullptr || at <= c->start + minPart || at >= c->end() - minPart)
                continue;

            Clip right = *c;
            right.id = nextClipId++;
            right.start = at;
            right.length = c->end() - at;
            if (! c->reverse)
            {
                right.offset = c->offset + (at - c->start);
            }
            else
            {
                // a reversed clip plays its source backwards: the right part is the *start* of the region
                right.offset = c->offset;
                c->offset += c->end() - at;
            }
            right.takeGroup = newGroup;
            right.fadeIn = 0.0;                                      // the cut gets no fade of its own
            right.fadeOut = juce::jmin (right.fadeOut, right.length);
            c->length = at - c->start;   // `c` is not used after the push_back below
            c->fadeOut = 0.0;
            c->fadeIn = juce::jmin (c->fadeIn, c->length);

            if (cid == id)
                result = right.id;
            t.clips.push_back (std::move (right));
        }
        return result;
    }

    if (auto* m = findMidiClip (t, id))
    {
        if (at <= m->start + minPart || at >= m->end() - minPart)
            return -1;

        const double cut = at - m->start;
        MidiClip right;
        right.id = nextClipId++;
        right.start = at;
        right.length = m->end() - at;

        // Notes that straddle the cut are split in two so nothing is lost.
        std::vector<Note> left;
        for (auto n : m->notes)
        {
            const double noteEnd = n.start + n.length;
            if (noteEnd <= cut)
            {
                left.push_back (n);
            }
            else if (n.start >= cut)
            {
                n.start -= cut;
                right.notes.push_back (n);
            }
            else
            {
                Note a = n, b = n;
                a.length = cut - n.start;
                b.start = 0.0;
                b.length = noteEnd - cut;
                left.push_back (a);
                right.notes.push_back (b);
            }
        }
        m->notes = std::move (left);
        m->length = cut;   // `m` is not used after the push_back below

        const int newId = right.id;
        t.midiClips.push_back (std::move (right));
        return newId;
    }
    return -1;
}

int Engine::duplicateClip (Track& t, int id)
{
    const juce::ScopedLock sl (lock);

    if (auto* c = findClip (t, id))
    {
        Clip copy = *c;
        copy.id = nextClipId++;
        copy.start = c->end();
        copy.takeGroup = 0;   // a copy is an ordinary clip
        copy.takeIndex = 0;
        copy.takeActive = true;
        const int newId = copy.id;
        t.clips.push_back (std::move (copy));
        return newId;
    }
    if (auto* m = findMidiClip (t, id))
    {
        MidiClip copy = *m;
        copy.id = nextClipId++;
        copy.start = m->end();
        const int newId = copy.id;
        t.midiClips.push_back (std::move (copy));
        return newId;
    }
    return -1;
}

bool Engine::deleteClip (Track& t, int id)
{
    const juce::ScopedLock sl (lock);
    const auto before = t.clips.size() + t.midiClips.size();
    const int group = findClip (t, id) != nullptr ? findClip (t, id)->takeGroup : 0;   // deleting a take set removes all its takes
    t.clips.erase (std::remove_if (t.clips.begin(), t.clips.end(),
                                   [id, group] (auto& c) { return c.id == id || (group != 0 && c.takeGroup == group); }),
                   t.clips.end());
    t.midiClips.erase (std::remove_if (t.midiClips.begin(), t.midiClips.end(), [id] (auto& c) { return c.id == id; }),
                       t.midiClips.end());
    return t.clips.size() + t.midiClips.size() != before;
}

bool Engine::moveClipToTrack (Track& from, Track& to, int id)
{
    if (&from == &to || from.type != to.type)
        return false;

    const juce::ScopedLock sl (lock);
    if (auto* c0 = findClip (from, id))
    {
        std::vector<int> ids { id };   // the takes of a group travel together
        if (c0->takeGroup != 0)
            for (auto& other : from.clips)
                if (other.takeGroup == c0->takeGroup && other.id != id)
                    ids.push_back (other.id);
        for (const int cid : ids)
            if (auto* c = findClip (from, cid))
            {
                to.clips.push_back (std::move (*c));
                from.clips.erase (from.clips.begin() + (c - from.clips.data()));
            }
        return true;
    }
    if (auto* m = findMidiClip (from, id))
    {
        to.midiClips.push_back (std::move (*m));
        from.midiClips.erase (from.midiClips.begin() + (m - from.midiClips.data()));
        return true;
    }
    return false;
}

TempoMap Engine::getTempoMap() const
{
    const juce::ScopedLock sl (lock);
    return tempoMap;
}

void Engine::setTempoMap (const TempoMap& newMap, bool retimeMidi)
{
    const juce::ScopedLock sl (lock);
    const TempoMap oldMap = tempoMap;

    if (retimeMidi)
    {
        // MIDI keeps its place in the music (same bar, same distance into the bar), so it follows the new tempo; the notes
        // of a clip are stored relative to it, so start and end of every note are moved as absolute times.
        auto move = [&] (double s) { return TempoMap::retime (oldMap, newMap, s); };
        for (auto& t : tracks)
            for (auto& clip : t->midiClips)
            {
                const double oldStart = clip.start, oldEnd = clip.end();
                const double newStart = move (oldStart), newEnd = move (oldEnd);
                for (auto& n : clip.notes)
                {
                    const double a = move (oldStart + n.start), b = move (oldStart + n.start + n.length);
                    n.start = juce::jmax (0.0, a - newStart);
                    n.length = juce::jmax (0.01, b - a);
                }
                clip.start = newStart;
                clip.length = juce::jmax (0.1, newEnd - newStart);
            }

        if (loopEnd > loopStart)
        {
            const double a = move (loopStart), b = move (loopEnd);
            loopStart = a;
            loopEnd = b;
        }
        if (punchOut > punchIn)
        {
            const double a = move (punchIn), b = move (punchOut);
            punchIn = a;
            punchOut = b;
        }
    }

    tempoMap = newMap;
    bpm = (float) tempoMap.getSections().front().bpm;
}

double Engine::getTotalLengthSeconds() const
{
    const juce::ScopedLock sl (lock);
    double end = 0.0;
    for (auto& t : tracks)
    {
        for (auto& c : t->clips)
            end = juce::jmax (end, c.end());
        for (auto& m : t->midiClips)
            end = juce::jmax (end, m.end());
    }
    return end;
}

//==============================================================================
// Automation

ParamAutomation* Engine::findLane (const Track& t, const juce::String& id) const
{
    for (auto& lane : t.autoLanes)
        if (lane->id == id)
            return lane.get();
    return nullptr;
}

// Turns an id into a lane bound to the right parameter, creating it on first use.
ParamAutomation* Engine::ensureLane (Track& t, const juce::String& id)
{
    if (auto* existing = findLane (t, id))
        return existing;

    auto lane = std::make_unique<ParamAutomation>();
    lane->id = id;

    if (id.startsWith ("fx."))
    {
        const auto name = id.substring (3);
        for (auto& spec : fxParamSpecs())
            if (name == spec.id)
            {
                lane->name = spec.label;
                lane->lo = spec.lo;
                lane->hi = spec.hi;
                lane->floatTarget = t.fx.findFloat (name);
            }
    }
    else if (id.startsWith ("synth."))
    {
        struct S { const char* name; std::atomic<float>* target; const char* label; float lo, hi; };
        const S synthParams[] = {
            { "attack",  &t.synth.attack,  "Synth attack",  0.001f, 2.0f },
            { "decay",   &t.synth.decay,   "Synth decay",   0.001f, 2.0f },
            { "sustain", &t.synth.sustain, "Synth sustain", 0.0f,   1.0f },
            { "release", &t.synth.release, "Synth release", 0.01f,  3.0f },
            { "cutoff",  &t.synth.cutoff,  "Synth cutoff",  100.0f, 18000.0f },
        };
        for (auto& s : synthParams)
            if (id.substring (6) == s.name)
            {
                lane->name = s.label;
                lane->lo = s.lo;
                lane->hi = s.hi;
                lane->floatTarget = s.target;
            }
    }
    else if (id.startsWith ("plugin:"))
    {
        const auto parts = juce::StringArray::fromTokens (id.substring (7), ":", "");
        if (parts.size() == 2)
        {
            const int key = parts[0].getIntValue(), index = parts[1].getIntValue();
            std::shared_ptr<PluginSlot> slot;
            if (t.instrument != nullptr && t.instrument->key == key)
                slot = t.instrument;
            for (auto& p : t.inserts)
                if (p->key == key)
                    slot = p;
            for (auto& p : t.midiFx)
                if (p->key == key)
                    slot = p;

            if (slot != nullptr && juce::isPositiveAndBelow (index, slot->instance->getParameters().size()))
            {
                lane->plugin = slot;
                lane->pluginParam = index;
                lane->lo = 0.0f;
                lane->hi = 1.0f;
                lane->name = slot->description.name + ": " + slot->instance->getParameters()[index]->getName (60);
            }
        }
    }

    if (lane->floatTarget == nullptr && lane->plugin == nullptr)
        return nullptr;

    t.autoLanes.push_back (std::move (lane));
    return t.autoLanes.back().get();
}

std::vector<AutoTarget> Engine::listAutomationTargets (const Track& t) const
{
    const juce::ScopedLock sl (lock);
    std::vector<AutoTarget> out;

    for (auto& spec : fxParamSpecs())
        out.push_back ({ juce::String ("fx.") + spec.id, spec.label, "Effects" });

    if (t.type == Track::Type::Midi && t.instrument == nullptr && t.kit == nullptr)
        for (auto* name : { "cutoff", "attack", "release" })
            out.push_back ({ juce::String ("synth.") + name, "Synth " + juce::String (name), "Synth" });

    auto addPlugin = [&] (const PluginSlot& slot, const juce::String& group)
    {
        auto& params = slot.instance->getParameters();
        for (int i = 0; i < params.size() && i < 600; ++i)
            if (params[i]->isAutomatable())
                out.push_back ({ "plugin:" + juce::String (slot.key) + ":" + juce::String (i), params[i]->getName (60), group });
    };
    if (t.instrument != nullptr)
        addPlugin (*t.instrument, t.instrument->description.name + " (instrument)");
    for (auto& p : t.inserts)
        addPlugin (*p, p->description.name);
    for (auto& p : t.midiFx)
        addPlugin (*p, p->description.name + " (MIDI effect)");
    return out;
}

AutoInfo Engine::getAutomationInfo (const Track& t, const juce::String& id) const
{
    const juce::ScopedLock sl (lock);
    AutoInfo info;
    if (id == "volume" || id == "pan")
    {
        info.valid = true;
        info.name = id == "volume" ? "Volume" : "Pan";
        info.lo = id == "volume" ? 0.0f : -1.0f;
        info.hi = id == "volume" ? 1.5f : 1.0f;
        info.current = id == "volume" ? t.gain.load() : t.pan.load();
        return info;
    }

    // a lane that doesn't exist yet is described by creating it, which is harmless (empty lanes do nothing)
    auto* lane = findLane (t, id);
    if (lane == nullptr)
        lane = const_cast<Engine*> (this)->ensureLane (const_cast<Track&> (t), id);
    if (lane == nullptr)
        return info;

    info.valid = true;
    info.name = lane->name;
    info.lo = lane->lo;
    info.hi = lane->hi;
    info.current = lane->floatTarget != nullptr ? lane->floatTarget->load()
                                                : lane->plugin->instance->getParameters()[lane->pluginParam]->getValue();
    return info;
}

std::vector<AutoPoint> Engine::getAutomation (const Track& t, const juce::String& id) const
{
    const juce::ScopedLock sl (lock);
    if (id == "volume") return t.autoVolume.points;
    if (id == "pan")    return t.autoPan.points;
    if (auto* lane = findLane (t, id))
        return lane->points;
    return {};
}

bool Engine::setAutomation (Track& t, const juce::String& id, std::vector<AutoPoint> points)
{
    std::stable_sort (points.begin(), points.end(), [] (const AutoPoint& a, const AutoPoint& b) { return a.time < b.time; });
    const juce::ScopedLock sl (lock);
    if (id == "volume") { t.autoVolume.points = std::move (points); return true; }
    if (id == "pan")    { t.autoPan.points = std::move (points); return true; }
    if (auto* lane = ensureLane (t, id))
    {
        lane->points = std::move (points);
        return true;
    }
    return false;
}

void Engine::setAutomationActive (Track& t, const juce::String& id, bool active)
{
    const juce::ScopedLock sl (lock);
    if (id == "volume")      t.autoVolume.active = active;
    else if (id == "pan")    t.autoPan.active = active;
    else if (auto* lane = findLane (t, id))
        lane->active = active;
}

bool Engine::isAutomationActive (const Track& t, const juce::String& id) const
{
    if (id == "volume") return t.autoVolume.active;
    if (id == "pan")    return t.autoPan.active;
    auto* lane = findLane (t, id);
    return lane != nullptr && lane->active;
}

std::vector<juce::String> Engine::getAutomationIds (const Track& t) const
{
    const juce::ScopedLock sl (lock);
    std::vector<juce::String> ids;
    if (! t.autoVolume.points.empty()) ids.push_back ("volume");
    if (! t.autoPan.points.empty())    ids.push_back ("pan");
    for (auto& lane : t.autoLanes)
        if (! lane->points.empty())
            ids.push_back (lane->id);
    return ids;
}

// Audio thread: moves every automated built-in/plugin parameter to its envelope value at this time.
void Engine::applyAutomation (Track& t, double time)
{
    for (auto& lane : t.autoLanes)
    {
        if (! lane->active || lane->points.empty())
            continue;

        const float v = evalAutomation (lane->points, time, lane->lastApplied);
        if (v == lane->lastApplied)
            continue;
        lane->lastApplied = v;

        if (lane->floatTarget != nullptr)
        {
            lane->floatTarget->store (v);
        }
        else if (lane->plugin != nullptr)
        {
            auto& params = lane->plugin->instance->getParameters();
            if (lane->pluginParam < params.size())
                params[lane->pluginParam]->setValue (v);
        }
    }
}

//==============================================================================
// Undo/redo snapshots

// Copies what the user can edit on one track into a TrackState (MIDI routes are added by captureState: they need indices).
TrackState Engine::captureTrack (const Track& t) const
{
    TrackState ts;
    ts.name = t.name;
    ts.type = t.type;
    ts.bus = t.bus;
    ts.clips = t.clips;
    ts.midiClips = t.midiClips;
    ts.gain = t.gain;
    ts.pan = t.pan;
    ts.mute = t.mute;
    ts.solo = t.solo;
    ts.waveform = t.synth.waveform;
    ts.synthValues[0] = t.synth.attack;
    ts.synthValues[1] = t.synth.decay;
    ts.synthValues[2] = t.synth.sustain;
    ts.synthValues[3] = t.synth.release;
    ts.synthValues[4] = t.synth.cutoff;
    ts.instrument = t.instrument;
    ts.inserts = t.inserts;
    ts.midiFx = t.midiFx;
    ts.sequencer = t.sequencer;
    ts.recordArmed = t.recordArmed;
    ts.externalOuts = t.externalOuts;
    ts.inputFirst = t.inputFirst;
    ts.inputCount = t.inputCount;
    ts.autoLanes.push_back ({ "volume", t.autoVolume.points, t.autoVolume.active.load() });
    ts.autoLanes.push_back ({ "pan", t.autoPan.points, t.autoPan.active.load() });
    for (auto& lane : t.autoLanes)
        ts.autoLanes.push_back ({ lane->id, lane->points, lane->active.load() });
    ts.kit = t.kit;
    const_cast<Track&> (t).fx.forEachFloat ([&ts] (const char*, std::atomic<float>& v) { ts.fxFloats.push_back (v.load()); });
    const_cast<Track&> (t).fx.forEachBool ([&ts] (const char*, std::atomic<bool>& v) { ts.fxBools.push_back (v.load() ? 1 : 0); });
    return ts;
}

// The way back: writes a TrackState into an existing track (caller holds the lock if the track is live).
void Engine::applyTrackState (Track& t, const TrackState& ts)
{
    t.name = ts.name;
    t.bus = ts.bus;
    t.clips = ts.clips;
    t.midiClips = ts.midiClips;
    t.gain = ts.gain;
    t.pan = ts.pan;
    t.mute = ts.mute;
    t.solo = ts.solo;
    t.synth.waveform = ts.waveform;
    t.synth.attack = ts.synthValues[0];
    t.synth.decay = ts.synthValues[1];
    t.synth.sustain = ts.synthValues[2];
    t.synth.release = ts.synthValues[3];
    t.synth.cutoff = ts.synthValues[4];

    size_t fi = 0, bi = 0;
    t.fx.forEachFloat ([&] (const char*, std::atomic<float>& v) { if (fi < ts.fxFloats.size()) v = ts.fxFloats[fi]; ++fi; });
    t.fx.forEachBool ([&] (const char*, std::atomic<bool>& v) { if (bi < ts.fxBools.size()) v = ts.fxBools[bi] != 0; ++bi; });

    t.instrument = ts.instrument;
    t.inserts = ts.inserts;
    t.midiFx = ts.midiFx;
    t.kit = ts.kit;
    t.sequencer = ts.sequencer;
    t.recordArmed = ts.recordArmed;
    t.externalOuts = ts.externalOuts;
    t.inputFirst = ts.inputFirst;
    t.inputCount = ts.inputCount;

    // lanes last: they bind to the effect/synth parameters and plugins assigned above
    t.autoLanes.clear();
    t.autoVolume.points.clear();
    t.autoPan.points.clear();
    t.autoVolume.active = true;
    t.autoPan.active = true;
    for (auto& ls : ts.autoLanes)
    {
        if (ls.id == "volume")
        {
            t.autoVolume.points = ls.points;
            t.autoVolume.active = ls.active;
        }
        else if (ls.id == "pan")
        {
            t.autoPan.points = ls.points;
            t.autoPan.active = ls.active;
        }
        else if (auto* lane = ensureLane (t, ls.id))
        {
            lane->points = ls.points;
            lane->active = ls.active;
        }
    }
}

ProjectState Engine::captureState() const
{
    const juce::ScopedLock sl (lock);
    ProjectState s;
    s.tempo = tempoMap;
    s.master = captureTrack (*master);

    for (auto& t : tracks)
    {
        TrackState ts = captureTrack (*t);
        for (auto& route : t->midiOuts)
            for (size_t k = 0; k < tracks.size(); ++k)
                if (tracks[k].get() == route.target)
                    ts.midiOuts.push_back ({ (int) k, route.channel });
        for (size_t k = 0; k < tracks.size(); ++k)
        {
            if (tracks[k].get() == t->output)
                ts.outputIndex = (int) k;
            for (auto& s : t->sends)
                if (tracks[k].get() == s.bus)
                    ts.sends.push_back ({ (int) k, s.level, s.preFader });
        }
        s.tracks.push_back (std::move (ts));
    }
    return s;
}

void Engine::restoreState (const ProjectState& s)
{
    std::vector<std::unique_ptr<Track>> fresh;
    for (auto& ts : s.tracks)
    {
        auto t = std::make_unique<Track>();
        t->type = ts.type;
        applyTrackState (*t, ts);

        t->fxState = std::make_unique<TrackFx>();
        t->fxState->prepare (sampleRate);
        if (t->type == Track::Type::Midi)
        {
            t->voices = std::make_unique<Synth>();
            t->voices->prepare (sampleRate);
        }
        t->routedMidi.ensureSize (4096);
        sizeDelayBuffers (*t, sampleRate);
        if (t->bus)
        {
            t->busInL.assign (trackL.size(), 0.0f);
            t->busInR.assign (trackL.size(), 0.0f);
        }
        fresh.push_back (std::move (t));
    }

    for (size_t i = 0; i < s.tracks.size(); ++i)   // buses and sends point at tracks by index, too
    {
        const auto& ts = s.tracks[i];
        const auto isBus = [&] (int k) { return k >= 0 && k < (int) fresh.size() && k != (int) i && fresh[(size_t) k]->bus; };
        if (! fresh[i]->bus && isBus (ts.outputIndex))
            fresh[i]->output = fresh[(size_t) ts.outputIndex].get();
        for (auto& snd : ts.sends)
            if (! fresh[i]->bus && isBus (snd.bus))
                fresh[i]->sends.push_back ({ fresh[(size_t) snd.bus].get(), snd.level, snd.preFader });
    }

    for (size_t i = 0; i < s.tracks.size(); ++i)   // MIDI routes point at tracks by index
    {
        fresh[i]->sequencer = s.tracks[i].sequencer;
        fresh[i]->recordArmed = s.tracks[i].recordArmed;
        for (auto& [to, channel] : s.tracks[i].midiOuts)
            if (to >= 0 && to < (int) fresh.size() && to != (int) i)
                fresh[i]->midiOuts.push_back ({ fresh[(size_t) to].get(), channel });
    }

    {
        const juce::ScopedLock sl (lock);
        tempoMap = s.tempo;   // (the MIDI of the snapshot already matches it)
        bpm = (float) tempoMap.getSections().front().bpm;
        tracks.swap (fresh);
        applyTrackState (*master, s.master);   // master channel: volume, effects, plugins, automation
    }
    // `fresh` now holds the old tracks and is destroyed here, outside the lock
}

//==============================================================================
// Note preview (UI thread -> audio thread)

void Engine::previewNote (const Track& t, int pitch, bool on, float velocity)
{
    const juce::SpinLock::ScopedLockType sl (previewWriteLock);
    int s1, n1, s2, n2;
    previewFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0)
        previewQueue[(size_t) s1] = { &t, pitch, on, velocity };
    else if (n2 > 0)
        previewQueue[(size_t) s2] = { &t, pitch, on, velocity };
    previewFifo.finishedWrite (n1 + n2);
}

void Engine::drainPreviewEvents()
{
    int s1, n1, s2, n2;
    previewFifo.prepareToRead (previewFifo.getNumReady(), s1, n1, s2, n2);

    auto apply = [this] (const PreviewEvent& e)
    {
        for (auto& t : tracks)
            if (t.get() == e.track)
            {
                if (! t->externalOuts.empty() && t->instrument == nullptr)   // notes played live go to the hardware
                {
                    forwardExternal (*t, e.on ? juce::MidiMessage::noteOn (1, e.pitch, e.velocity)
                                              : juce::MidiMessage::noteOff (1, e.pitch), 0);
                    return;   // (a track that sends to hardware makes no sound of its own)
                }

                if (t->instrument != nullptr)   // hosted instrument: queue a MIDI message for its next block
                {
                    t->liveMidi.addEvent (e.on ? juce::MidiMessage::noteOn (1, e.pitch, e.velocity)
                                               : juce::MidiMessage::noteOff (1, e.pitch), 0);
                    t->pluginTailSamples = 3.0 * sampleRate;
                }
                else if (t->kit != nullptr)
                {
                    if (e.on) t->kit->noteOn (e.pitch, e.velocity);   // one-shots: note-offs don't matter
                }
                else if (t->voices != nullptr)
                {
                    if (e.on) t->voices->noteOn (e.pitch, e.velocity); else t->voices->noteOff (e.pitch);
                }
                return;
            }
    };

    for (int i = 0; i < n1; ++i) apply (previewQueue[(size_t) (s1 + i)]);
    for (int i = 0; i < n2; ++i) apply (previewQueue[(size_t) (s2 + i)]);
    previewFifo.finishedRead (n1 + n2);
}

bool Engine::anySynthActive() const
{
    for (auto& t : tracks)
        if ((t->voices != nullptr && t->voices->isActive())
            || (t->kit != nullptr && t->kit->isActive())
            || (t->instrument != nullptr && t->pluginTailSamples > 0.0))
            return true;
    return false;
}

void Engine::silenceSynths()
{
    master->fxState->reset();
    for (auto& p : master->inserts)
        p->instance->reset();

    for (auto& t : tracks)
    {
        if (t->voices != nullptr)
            t->voices->reset();
        if (t->kit != nullptr)
            t->kit->reset();
        t->fxState->reset();
        if (t->instrument != nullptr)
            t->instrument->instance->reset();
        for (auto& p : t->inserts)
            p->instance->reset();
        for (auto& p : t->midiFx)
            p->instance->reset();
        std::fill (t->pdcL.begin(), t->pdcL.end(), 0.0f);
        std::fill (t->pdcR.begin(), t->pdcR.end(), 0.0f);
        t->routedMidi.clear();
        t->liveMidi.clear();
        t->pluginTailSamples = 0.0;
    }
}

double Engine::getRenderTailSeconds() const
{
    const juce::ScopedLock sl (lock);
    if (master->fx.hasTail() || ! master->inserts.empty())
        return 3.0;
    for (auto& t : tracks)
        if (t->fx.hasTail() || t->instrument != nullptr || ! t->inserts.empty())
            return 3.0;
    return 0.5;
}

// All notes of a track's synth or hosted instrument fade out (used when playback starts, stops or jumps).
void Engine::releaseAllNotes (Track& t)
{
    externalAllNotesOff (t);
    if (t.voices != nullptr)
        t.voices->allNotesOff();
    if (t.kit != nullptr)
        t.kit->stopAll();
    if (t.instrument != nullptr)
    {
        t.liveMidi.addEvent (juce::MidiMessage::allNotesOff (1), 0);
        t.pluginTailSamples = juce::jmax (t.pluginTailSamples, 2.0 * sampleRate);
    }
}

//==============================================================================
// Audio

void Engine::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    // The buffer still holds the device input here, so take it before clearing it for output: record it, and keep
    // a copy of every input channel for tracks that monitor.
    const int bufferChannels = info.buffer->getNumChannels();
    if (playing && recordingActive && countInRemaining.load() <= 0)   // (nothing is recorded during a count-in)
    {
        // With punch on, only the part of this block inside [punchIn, punchOut) is written.
        const auto blockPos = positionSamples.load();
        int from = 0, to = info.numSamples;
        if (punchEnabled)
        {
            const auto inS = (juce::int64) (punchIn * sampleRate), outS = (juce::int64) (punchOut * sampleRate);
            from = (int) juce::jlimit<juce::int64> (0, info.numSamples, inS - blockPos);
            to = (int) juce::jlimit<juce::int64> (0, info.numSamples, outS - blockPos);
            if (blockPos + info.numSamples >= outS)
                punchDone = true;
        }

        if (to > from)
        {
            const juce::SpinLock::ScopedTryLockType rl (recLock);
            if (rl.isLocked())
            {
                if (recFirstSample.load() < 0)
                    recFirstSample = blockPos + from;
                for (auto& r : recorders)
                {
                    const float* in[2];
                    in[0] = info.buffer->getReadPointer (juce::jmin (r.first, bufferChannels - 1), info.startSample + from);
                    in[1] = r.count > 1 ? info.buffer->getReadPointer (juce::jmin (r.first + 1, bufferChannels - 1), info.startSample + from) : in[0];
                    r.writer->write (in, to - from);
                }
            }
        }
    }

    if (const int tc = tunerChannel.load(); tc >= 0 && tc < bufferChannels)
    {
        const float* src = info.buffer->getReadPointer (tc, info.startSample);
        auto w = tunerWritePos.load();
        for (int i = 0; i < info.numSamples; ++i, ++w)
            tunerRing[(size_t) (w % tunerRingSize)] = src[i];
        tunerWritePos = w;
    }

    inputChannelsCopied = juce::jmin (bufferChannels, inputBuf.getNumChannels());
    if (info.numSamples <= inputBuf.getNumSamples())
        for (int c = 0; c < inputChannelsCopied; ++c)
            juce::FloatVectorOperations::copy (inputBuf.getWritePointer (c), info.buffer->getReadPointer (c, info.startSample), info.numSamples);
    else
        inputChannelsCopied = 0;
    inputOffset = 0;

    info.clearActiveBufferRegion();

    if (offlineRendering)   // an export is using the mixer: stay silent and out of its way
    {
        setMasterLevel (0.0f);
        return;
    }

    const double callbackMs = juce::Time::getMillisecondCounterHiRes();   // when this block started: MIDI to hardware is timed from it
    blockStartMs = callbackMs;

    const juce::ScopedTryLock sl (lock);
    if (! sl.isLocked())
    {
        setMasterLevel (0.0f);
        return;
    }

    drainPreviewEvents();

    const bool counting = playing && countInRemaining.load() > 0;   // the count-in plays: the transport waits
    const bool running = playing && ! counting;
    const auto pos = positionSamples.load();

    if (! running)
    {
        if (wasRunning)
        {
            for (auto& t : tracks)
                releaseAllNotes (*t);
            wasRunning = false;
        }
        if (! anySynthActive() && ! anyMonitoring() && ! counting)   // (monitoring keeps the mixer running while stopped)
        {
            for (auto& t : tracks) t->level = 0.0f;
            setMasterLevel (0.0f);
            return;
        }
    }
    else
    {
        if (! wasRunning || pos != lastBlockEnd)   // playback started or the playhead jumped
            for (auto& t : tracks)
            {
                releaseAllNotes (*t);
                if (t->fx.hasTail())
                    t->fxState->reset();   // don't replay the tail of wherever we were before
                if (t->pdcDelay != 0)
                {
                    std::fill (t->pdcL.begin(), t->pdcL.end(), 0.0f);
                    std::fill (t->pdcR.begin(), t->pdcR.end(), 0.0f);
                }
            }
        wasRunning = true;
    }

    auto* outL = info.buffer->getWritePointer (0, info.startSample);
    auto* outR = info.buffer->getNumChannels() > 1 ? info.buffer->getWritePointer (1, info.startSample) : nullptr;

    const bool withClick = running && metronome;
    const double loopStartSec = loopStart, loopEndSec = loopEnd;

    if (running && loopEnabled && loopEndSec > loopStartSec + 0.05)
    {
        // Looping: render up to the loop end, jump back, and render the rest of the block from the loop start,
        // so the wrap is sample-accurate.
        const auto loopStartS = (juce::int64) (loopStartSec * sampleRate);
        const auto loopEndS = (juce::int64) (loopEndSec * sampleRate);
        auto p = pos;
        int done = 0;

        while (done < info.numSamples)
        {
            const int remaining = info.numSamples - done;
            int len = remaining;
            bool wrap = false;
            if (p <= loopEndS && p + remaining >= loopEndS)
            {
                len = (int) (loopEndS - p);
                wrap = true;
            }

            if (len > 0)
            {
                blockStartMs = callbackMs + 1000.0 * (double) done / sampleRate;
                inputOffset = done;
                mixBlock (outL + done, outR != nullptr ? outR + done : nullptr, len, p, withClick, true);
            }
            done += len;
            p += len;

            if (wrap)
            {
                p = loopStartS;
                for (auto& t : tracks)
                    releaseAllNotes (*t);   // notes crossing the loop end are released (effect tails carry on)
            }
        }
        positionSamples = p;
        lastBlockEnd = p;
        return;
    }

    mixBlock (outL, outR, info.numSamples, pos, withClick, running);

    if (counting)
        addCountInClicks (outL, outR, info.numSamples);

    if (running)
    {
        positionSamples += info.numSamples;
        lastBlockEnd = pos + info.numSamples;
    }
}

void Engine::startCountIn (int bars)
{
    if (bars <= 0)
    {
        countInRemaining = 0;
        return;
    }

    const juce::ScopedLock sl (lock);
    const double position = (double) positionSamples.load() / sampleRate;
    const auto& section = tempoMap.sectionAt (position);
    countInUnitSamples = section.beatUnit() * 60.0 / section.bpm * sampleRate;
    countInNumerator = section.numerator;
    countInTotal = (juce::int64) std::llround ((double) (bars * section.numerator) * countInUnitSamples);
    countInRemaining = countInTotal;
}

int Engine::countInBeatsLeft() const
{
    const auto left = countInRemaining.load();
    return left <= 0 ? 0 : (int) std::ceil ((double) left / juce::jmax (1.0, countInUnitSamples));
}

void Engine::addCountInClicks (float* outL, float* outR, int n)
{
    const auto remaining = countInRemaining.load();
    for (int i = 0; i < n; ++i)
    {
        const auto elapsed = countInTotal - remaining + i;
        if (elapsed >= countInTotal)
            break;
        const double k = std::floor ((double) elapsed / countInUnitSamples);
        const double t = ((double) elapsed - k * countInUnitSamples) / sampleRate;   // seconds since this click began
        if (t > 0.05)
            continue;
        const double freq = std::fmod (k, (double) countInNumerator) < 0.5 ? 1500.0 : 1000.0;
        const auto s = (float) (std::sin (juce::MathConstants<double>::twoPi * freq * t) * std::exp (-t * 80.0) * 0.5);
        outL[i] += s;
        if (outR != nullptr)
            outR[i] += s;
    }
    countInRemaining = juce::jmax<juce::int64> (0, remaining - n);
}

// Fills noteEvents with the note-ons/offs of a track's MIDI clips that fall inside [pos, pos + n).
void Engine::collectNoteEvents (Track& t, int n, juce::int64 pos, bool running)
{
    noteEvents.clear();

    if (running)
    {
        const double blockEnd = (double) (pos + n);
        for (auto& clip : t.midiClips)
            for (auto& note : clip.notes)
            {
                if (note.start >= clip.length)
                    continue;

                const double on = (clip.start + note.start) * sampleRate;
                const double off = on + note.length * sampleRate;
                if (on >= (double) pos && on < blockEnd)
                    noteEvents.push_back ({ (int) (on - (double) pos), true, note.pitch, note.velocity });
                if (off >= (double) pos && off < blockEnd)
                    noteEvents.push_back ({ (int) (off - (double) pos), false, note.pitch, 0.0f });
            }
    }

    // notes that other tracks (a plugin sequencer) routed here during this block
    for (const auto meta : t.routedMidi)
    {
        const auto m = meta.getMessage();
        const int offset = juce::jlimit (0, juce::jmax (0, n - 1), meta.samplePosition);
        if (m.isNoteOn())       noteEvents.push_back ({ offset, true, m.getNoteNumber(), m.getFloatVelocity(), m.getChannel() });
        else if (m.isNoteOff()) noteEvents.push_back ({ offset, false, m.getNoteNumber(), 0.0f, m.getChannel() });
    }

    // note-offs go first at the same sample so a repeated pitch retriggers cleanly
    std::sort (noteEvents.begin(), noteEvents.end(), [] (const NoteEvent& a, const NoteEvent& b)
               { return a.offset != b.offset ? a.offset < b.offset : (! a.on && b.on); });

    if (! t.midiFx.empty() && (running || ! noteEvents.empty()))
        applyMidiFx (t, n);

    // A track without a plugin instrument that sends to hardware: its notes (after the MIDI effects) go out here.
    // (With a plugin instrument it is the plugin's output that goes out, see runMidiSource.)
    if (t.instrument == nullptr && ! t.externalOuts.empty())
        for (auto& e : noteEvents)
            forwardExternal (t, e.on ? juce::MidiMessage::noteOn (e.channel, e.pitch, e.velocity)
                                     : juce::MidiMessage::noteOff (e.channel, e.pitch), e.offset);
}

// Samples of delay the plugins in a track add (reported by the plugins themselves).
int Engine::trackLatency (const Track& t)
{
    int latency = 0;
    if (t.instrument != nullptr && ! t.routesElsewhere())
        latency += t.instrument->instance->getLatencySamples();
    for (auto& p : t.inserts)
        if (! p->bypass)
            latency += p->instance->getLatencySamples();
    return juce::jmax (0, latency);
}

// Delays a track's stereo signal by `delay` samples (at most the length of its delay buffer).
void Engine::applyDelay (Track& t, float* l, float* r, int n, int delay)
{
    const int size = (int) t.pdcL.size();
    if (size == 0)
        return;

    delay = juce::jlimit (0, size - 1, delay);
    if (delay != t.pdcDelay)   // a change would otherwise replay whatever is left in the buffer
    {
        std::fill (t.pdcL.begin(), t.pdcL.end(), 0.0f);
        std::fill (t.pdcR.begin(), t.pdcR.end(), 0.0f);
        t.pdcDelay = delay;
    }

    int pos = t.pdcPos;
    for (int i = 0; i < n; ++i)
    {
        t.pdcL[(size_t) pos] = l[i];
        t.pdcR[(size_t) pos] = r[i];
        int read = pos - delay;
        if (read < 0)
            read += size;
        l[i] = t.pdcL[(size_t) read];
        r[i] = t.pdcR[(size_t) read];
        if (++pos >= size)
            pos = 0;
    }
    t.pdcPos = pos;
}

// MIDI plugins in a track (arpeggiators, chord generators...): the notes pass through each of them in turn.
void Engine::applyMidiFx (Track& t, int n)
{
    midiScratch.clear();
    for (auto& e : noteEvents)
        midiScratch.addEvent (e.on ? juce::MidiMessage::noteOn (e.channel, e.pitch, e.velocity)
                                   : juce::MidiMessage::noteOff (e.channel, e.pitch), e.offset);

    for (auto& slot : t.midiFx)
    {
        auto& buf = slot->scratch;
        const int channels = buf.getNumChannels();
        if (slot->bypass || channels < 1 || n > buf.getNumSamples())
            continue;

        buf.clear (0, n);
        juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), channels, n);
        slot->instance->processBlock (view, midiScratch);   // notes in, notes out (in place)
    }

    noteEvents.clear();
    for (const auto meta : midiScratch)
    {
        const auto m = meta.getMessage();
        const int offset = juce::jlimit (0, juce::jmax (0, n - 1), meta.samplePosition);
        if (m.isNoteOn())       noteEvents.push_back ({ offset, true, m.getNoteNumber(), m.getFloatVelocity(), m.getChannel() });
        else if (m.isNoteOff()) noteEvents.push_back ({ offset, false, m.getNoteNumber(), 0.0f, m.getChannel() });
    }
    std::sort (noteEvents.begin(), noteEvents.end(), [] (const NoteEvent& a, const NoteEvent& b)
               { return a.offset != b.offset ? a.offset < b.offset : (! a.on && b.on); });
}

// Hosted VST3 instrument: feed it this block's MIDI and take its stereo output.
void Engine::renderPluginInstrument (Track& t, int n, float* outL, float* outR)
{
    auto& slot = *t.instrument;
    auto& buf = slot.scratch;
    const int channels = buf.getNumChannels();
    if (channels < 1 || n > buf.getNumSamples())
        return;

    midiScratch.clear();
    midiScratch.addEvents (t.liveMidi, 0, n, 0);
    t.liveMidi.clear();
    for (auto& e : noteEvents)
        midiScratch.addEvent (e.on ? juce::MidiMessage::noteOn (e.channel, e.pitch, e.velocity)
                                   : juce::MidiMessage::noteOff (e.channel, e.pitch), e.offset);

    buf.clear (0, n);
    juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), channels, n);   // wraps, no allocation
    slot.instance->processBlock (view, midiScratch);

    std::copy (buf.getReadPointer (0), buf.getReadPointer (0) + n, outL);
    std::copy (buf.getReadPointer (channels > 1 ? 1 : 0), buf.getReadPointer (channels > 1 ? 1 : 0) + n, outR);
}

// A plugin sequencer track (Stochas and the like): run the plugin, throw its audio away and hand the notes it
// produced to the track it is routed to.
void Engine::runMidiSource (Track& t, int n)
{
    auto& slot = *t.instrument;
    auto& buf = slot.scratch;
    const int channels = buf.getNumChannels();
    if (channels < 1 || n > buf.getNumSamples())
        return;

    midiScratch.clear();
    midiScratch.addEvents (t.liveMidi, 0, n, 0);
    t.liveMidi.clear();
    for (auto& e : noteEvents)
        midiScratch.addEvent (e.on ? juce::MidiMessage::noteOn (e.channel, e.pitch, e.velocity)
                                   : juce::MidiMessage::noteOff (e.channel, e.pitch), e.offset);

    buf.clear (0, n);
    juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), channels, n);
    slot.instance->processBlock (view, midiScratch);   // the MIDI buffer now holds what the plugin sent out

    for (const auto meta : midiScratch)
    {
        const auto m = meta.getMessage();
        if (! m.isNoteOnOrOff())
            continue;

        const int position = juce::jlimit (0, juce::jmax (0, n - 1), meta.samplePosition);
        forwardExternal (t, m, position);   // hardware first: the analog synth is the point of an external sequencer
        for (auto& route : t.midiOuts)
        {
            if (route.channel == 0)                    // everything, channels untouched
            {
                route.target->routedMidi.addEvent (m, position);
            }
            else if (m.getChannel() == route.channel)  // one channel (e.g. one Stochas track), played as channel 1
            {
                auto copy = m;
                copy.setChannel (1);
                route.target->routedMidi.addEvent (copy, position);
            }
        }
    }
}

// Hosted VST3 effect, in place on the track's stereo buffer.
void Engine::processInsert (PluginSlot& slot, float* l, float* r, int n)
{
    auto& buf = slot.scratch;
    const int channels = buf.getNumChannels();
    if (channels < 2 || n > buf.getNumSamples())
        return;

    buf.clear (0, n);
    std::copy (l, l + n, buf.getWritePointer (0));
    std::copy (r, r + n, buf.getWritePointer (1));

    emptyMidi.clear();
    juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), channels, n);
    slot.instance->processBlock (view, emptyMidi);

    std::copy (buf.getReadPointer (0), buf.getReadPointer (0) + n, l);
    std::copy (buf.getReadPointer (1), buf.getReadPointer (1) + n, r);
}

void Engine::renderDrumTrack (Track& t, int n, juce::int64 pos, bool running, float* scratch)
{
    std::fill (scratch, scratch + n, 0.0f);
    collectNoteEvents (t, n, pos, running);

    int done = 0;
    for (auto& e : noteEvents)
    {
        if (! e.on)
            continue;   // drum hits are one-shots
        if (e.offset > done)
        {
            t.kit->render (scratch + done, e.offset - done);
            done = e.offset;
        }
        t.kit->noteOn (e.pitch, e.velocity);
    }
    if (done < n)
        t.kit->render (scratch + done, n - done);
}

void Engine::renderMidiTrack (Track& t, int n, juce::int64 pos, bool running, float* scratch)
{
    std::fill (scratch, scratch + n, 0.0f);
    collectNoteEvents (t, n, pos, running);

    int done = 0;
    for (auto& e : noteEvents)
    {
        if (e.offset > done)
        {
            t.voices->render (scratch + done, e.offset - done, t.synth);
            done = e.offset;
        }
        if (e.on) t.voices->noteOn (e.pitch, e.velocity); else t.voices->noteOff (e.pitch);
    }
    if (done < n)
        t.voices->render (scratch + done, n - done, t.synth);
}

void Engine::mixBlock (float* outL, float* outR, int n, juce::int64 pos, bool withMetronome, bool running)
{
    bool anySolo = false;
    for (auto& t : tracks)
        anySolo = anySolo || t->solo;

    constexpr double fadeSamples = 48.0;   // tiny fade at clip edges to avoid clicks

    playHead.samples = pos;
    playHead.playing = running;

    int maxLatency = 0;
    for (auto& t : tracks)
        if (! t->bus)   // a bus adds its own latency to what goes through it; it is not compensated
            maxLatency = juce::jmax (maxLatency, trackLatency (*t));

    // buses start every block empty
    {
        const int clearN = juce::jmin (n, (int) trackL.size());
        for (auto& t : tracks)
            if (t->bus)
            {
                if ((int) t->busInL.size() >= clearN)
                {
                    std::fill (t->busInL.begin(), t->busInL.begin() + clearN, 0.0f);
                    std::fill (t->busInR.begin(), t->busInR.begin() + clearN, 0.0f);
                }
                t->busFed = false;
            }
    }

    // MIDI routing first: sequencer tracks run and fill the routedMidi of the tracks they play,
    // so those tracks find their notes waiting when they are rendered below.
    for (auto& t : tracks)
        t->routedMidi.clear();
    const int routeN = juce::jmin (n, (int) trackL.size());
    for (auto& t : tracks)
        if (t->type == Track::Type::Midi && t->instrument != nullptr && t->routesElsewhere())
        {
            if (running)
                t->pluginTailSamples = 3.0 * sampleRate;
            if (t->pluginTailSamples > 0.0)
            {
                collectNoteEvents (*t, routeN, pos, running);
                runMidiSource (*t, routeN);
                if (! running)
                    t->pluginTailSamples -= routeN;
            }
        }

    // Two passes: ordinary tracks first (they fill the buses), then the buses, which play into the master.
    for (int pass = 0; pass < 2; ++pass)
    for (auto& t : tracks)
    {
        if ((t->bus ? 1 : 0) != pass)
            continue;

        if (renderOnly != nullptr && t.get() != renderOnly)   // exporting one stem: the other tracks stay out of it
        {
            t->level = 0.0f;
            continue;
        }

        float peak = 0.0f;
        // (a bus is heard when something audible feeds it, so soloing a track also lets its bus through)
        const bool audible = renderOnly != nullptr || (! t->mute && (! anySolo || t->solo || (t->bus && t->busFed)));

        // Fader and pan, possibly automated: evaluated at both ends of the block and ramped in between.
        const double t0 = (double) pos / sampleRate, t1 = (double) (pos + n) / sampleRate;
        const bool autoVol = t->autoVolume.active && ! t->autoVolume.points.empty();
        const bool autoPan = t->autoPan.active && ! t->autoPan.points.empty();
        const float gain0 = autoVol ? evalAutomation (t->autoVolume.points, t0, t->gain) : t->gain.load();
        const float gain1 = autoVol ? evalAutomation (t->autoVolume.points, t1, t->gain) : gain0;
        const float pan0 = autoPan ? evalAutomation (t->autoPan.points, t0, t->pan) : t->pan.load();
        const float pan1 = autoPan ? evalAutomation (t->autoPan.points, t1, t->pan) : pan0;
        t->effGain = gain0;
        t->effPan = pan0;
        if (running)
            applyAutomation (*t, t0);

        auto panGains = [] (float g, float p, float& l, float& r)
        {
            const float angle = (p + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
            l = std::cos (angle) * juce::MathConstants<float>::sqrt2 * g;
            r = std::sin (angle) * juce::MathConstants<float>::sqrt2 * g;
        };
        float gl0, gr0, gl1, gr1;
        panGains (gain0, pan0, gl0, gr0);
        panGains (gain1, pan1, gl1, gr1);
        const float g = gain1;

        // Each track is built in its own stereo buffer, then goes through its effects
        // and the fader/pan on the way into the master mix.
        float* const bufL = trackL.data();
        float* const bufR = trackR.data();
        const int nn = juce::jmin (n, (int) trackL.size());
        std::fill (bufL, bufL + nn, 0.0f);
        std::fill (bufR, bufR + nn, 0.0f);

        if (t->bus && (int) t->busInL.size() >= nn)   // a bus starts from what was sent and routed to it
        {
            std::copy (t->busInL.begin(), t->busInL.begin() + nn, bufL);
            std::copy (t->busInR.begin(), t->busInR.begin() + nn, bufR);
        }

        const bool monitoring = t->type == Track::Type::Audio && t->recordArmed && t->monitor
                                && ! offlineRendering && inputChannelsCopied > 0;

        if (t->type == Track::Type::Midi)
        {
            if (t->instrument != nullptr && t->routesElsewhere())
            {
                // sequencer track: it makes no sound itself, its notes were routed to another track above
            }
            else if (t->instrument != nullptr)
            {
                // the instrument keeps running for a while after the transport stops, so its tail can ring out
                if (running)
                    t->pluginTailSamples = 3.0 * sampleRate;
                if (t->pluginTailSamples > 0.0)
                {
                    collectNoteEvents (*t, nn, pos, running);
                    renderPluginInstrument (*t, nn, bufL, bufR);
                    if (! running)
                        t->pluginTailSamples -= nn;
                }
            }
            else if (! t->externalOuts.empty())
            {
                // plays hardware instead of the built-in sound: just send this block's notes out
                collectNoteEvents (*t, nn, pos, running);
            }
            else if (t->kit != nullptr)
            {
                renderDrumTrack (*t, nn, pos, running, bufL);
                std::copy (bufL, bufL + nn, bufR);
            }
            else if (t->voices != nullptr)
            {
                renderMidiTrack (*t, nn, pos, running, bufL);
                std::copy (bufL, bufL + nn, bufR);
            }
        }
        else if (! running && ! monitoring && ! (t->bus && t->busFed))
        {
            t->level = 0.0f;
            continue;
        }

        for (auto& c : t->clips)
        {
            if (! running)
                break;   // stopped: only a monitored input is heard
            if (! c.audible())
                continue;   // an inactive take
            const double clipStart = c.start * sampleRate;
            const double clipEnd = c.end() * sampleRate;
            if ((double) (pos + n) <= clipStart || (double) pos >= clipEnd || c.audio == nullptr)
                continue;

            const double ratio = c.fileRate / sampleRate;
            const double offsetSamples = c.offset * c.fileRate;
            const double regionSamples = c.length * c.fileRate;   // where the clip's region of the source ends (for reverse)
            const double fadeInSamples = juce::jmin (c.fadeIn, c.length) * sampleRate;
            const double fadeOutSamples = juce::jmin (c.fadeOut, c.length) * sampleRate;
            const int len = c.audio->getNumSamples();
            const auto* srcL = c.audio->getReadPointer (0);
            const auto* srcR = c.audio->getNumChannels() > 1 ? c.audio->getReadPointer (1) : srcL;

            for (int i = 0; i < nn; ++i)
            {
                const double tl = (double) (pos + i);
                if (tl < clipStart || tl >= clipEnd)
                    continue;

                const double sp = c.reverse ? offsetSamples + regionSamples - (tl - clipStart) * ratio
                                            : (tl - clipStart) * ratio + offsetSamples;
                if (sp < 0.0 || sp >= len - 1)
                    continue;

                const int i0 = (int) sp;
                const float frac = (float) (sp - i0);
                float edge = (float) juce::jmin (1.0, juce::jmin ((tl - clipStart) / fadeSamples,
                                                                  (clipEnd - tl) / fadeSamples));
                if (fadeInSamples > 0.0 && tl - clipStart < fadeInSamples)   // equal-power fades: a crossfade keeps the level
                    edge *= (float) std::sin ((tl - clipStart) / fadeInSamples * juce::MathConstants<double>::halfPi);
                if (fadeOutSamples > 0.0 && clipEnd - tl < fadeOutSamples)
                    edge *= (float) std::sin ((clipEnd - tl) / fadeOutSamples * juce::MathConstants<double>::halfPi);
                const float l = (srcL[i0] + frac * (srcL[i0 + 1] - srcL[i0])) * edge;
                const float r = (srcR[i0] + frac * (srcR[i0 + 1] - srcR[i0])) * edge;
                bufL[i] += l;
                bufR[i] += r;
            }
        }

        // Input monitoring: the armed track's input channels play live, through its effects, fader and pan.
        if (monitoring)
        {
            const int c0 = juce::jmin (t->inputFirst, inputChannelsCopied - 1);
            const int c1 = t->inputCount > 1 ? juce::jmin (t->inputFirst + 1, inputChannelsCopied - 1) : c0;   // mono: left = right
            const float* inL = inputBuf.getReadPointer (c0) + inputOffset;
            const float* inR = inputBuf.getReadPointer (c1) + inputOffset;
            for (int i = 0; i < nn; ++i)
            {
                bufL[i] += inL[i];
                bufR[i] += inR[i];
            }
        }

        // Effects and inserts run in slices of 64 samples when this track has automation on them, so the
        // envelope is followed smoothly instead of once per audio block.
        bool sliced = false;
        if (running)
            for (auto& lane : t->autoLanes)
                sliced = sliced || (lane->active && ! lane->points.empty());
        const int slice = sliced ? 64 : juce::jmax (1, nn);
        for (int s = 0; s < nn; s += slice)
        {
            const int len = juce::jmin (slice, nn - s);
            if (sliced)
                applyAutomation (*t, (double) (pos + s) / sampleRate);
            if (t->fx.anyOn())
                t->fxState->process (bufL + s, bufR + s, len, t->fx);
            for (auto& p : t->inserts)
                if (! p->bypass)
                    processInsert (*p, bufL + s, bufR + s, len);
        }

        // plugin delay compensation: tracks without (or with less) plugin latency wait for the slowest one
        const int delay = maxLatency - trackLatency (*t);
        if (! t->bus && (delay > 0 || t->pdcDelay != 0))
            applyDelay (*t, bufL, bufR, nn, delay);

        // Where it goes: the master, or the bus this track plays into (stems always go straight to the master).
        Track* const target = renderOnly == nullptr && ! t->bus && t->output != nullptr && t->output->bus
                              && (int) t->output->busInL.size() >= nn ? t->output : nullptr;
        float* const dstL = target != nullptr ? target->busInL.data() : outL;
        float* const dstR = target != nullptr ? target->busInR.data() : outR;

        const float ramp = nn > 0 ? 1.0f / (float) nn : 0.0f;
        for (int i = 0; i < nn; ++i)
        {
            peak = juce::jmax (peak, std::abs (bufL[i]), std::abs (bufR[i]));
            if (audible)
            {
                const float f = (float) i * ramp;
                dstL[i] += bufL[i] * (gl0 + (gl1 - gl0) * f);
                if (dstR) dstR[i] += bufR[i] * (gr0 + (gr1 - gr0) * f);
            }
        }
        if (audible && target != nullptr)
            target->busFed = true;

        // sends: a share of the sound goes to buses as well (muted tracks send nothing; stems are dry)
        if (audible && renderOnly == nullptr && ! t->bus)
            for (auto& send : t->sends)
            {
                auto* b = send.bus;
                if (b == nullptr || ! b->bus || b == t.get() || (int) b->busInL.size() < nn)
                    continue;
                float* sl = b->busInL.data();
                float* sr = b->busInR.data();
                const float lv = send.level;
                if (send.preFader)
                    for (int i = 0; i < nn; ++i)
                    {
                        sl[i] += bufL[i] * lv;
                        sr[i] += bufR[i] * lv;
                    }
                else
                    for (int i = 0; i < nn; ++i)
                    {
                        const float f = (float) i * ramp;
                        sl[i] += bufL[i] * (gl0 + (gl1 - gl0) * f) * lv;
                        sr[i] += bufR[i] * (gr0 + (gr1 - gr0) * f) * lv;
                    }
                b->busFed = true;
            }
        t->level = peak * g;
    }

    // ---- Master channel: effects and plugin inserts on the summed tracks, run in slices while its parameters
    // are automated. Stems skip it (renderMaster == false) so they add up to the mix.
    if (renderMaster && outR != nullptr)
    {
        auto& m = *master;
        bool sliced = false;
        if (running)
            for (auto& lane : m.autoLanes)
                sliced = sliced || (lane->active && ! lane->points.empty());
        const int slice = sliced ? 64 : juce::jmax (1, n);
        for (int s = 0; s < n; s += slice)
        {
            const int len = juce::jmin (slice, n - s);
            if (sliced)
                applyAutomation (m, (double) (pos + s) / sampleRate);
            if (m.fx.anyOn())
                m.fxState->process (outL + s, outR + s, len, m.fx);
            for (auto& p : m.inserts)
                if (! p->bypass)
                    processInsert (*p, outL + s, outR + s, len);
        }
    }

    // master volume, possibly automated: evaluated at both ends of the block and ramped in between
    float masterGain0 = 1.0f, masterGain1 = 1.0f;
    if (renderMaster)
    {
        auto& m = *master;
        const bool autoVol = m.autoVolume.active && ! m.autoVolume.points.empty();
        masterGain0 = autoVol ? evalAutomation (m.autoVolume.points, (double) pos / sampleRate, m.gain) : m.gain.load();
        masterGain1 = autoVol ? evalAutomation (m.autoVolume.points, (double) (pos + n) / sampleRate, m.gain) : masterGain0;
        m.effGain = masterGain0;
    }

    if (withMetronome)
    {
        // One click per counted beat of the time signature (the unit is 1/denominator of a whole note), the first of each bar
        // higher. The tempo map is read once per block.
        const double blockSeconds = (double) pos / sampleRate;
        const auto& section = tempoMap.sectionAt (blockSeconds);
        double intoBar = 0.0;
        tempoMap.barAtSeconds (blockSeconds, &intoBar);
        const double unit = section.beatUnit();
        const double perSample = section.bpm / 60.0 / sampleRate;   // quarter notes per sample
        for (int i = 0; i < n; ++i)
        {
            const double units = (intoBar + (double) i * perSample) / unit;   // counted beats since the start of the bar
            const double k = std::floor (units);
            const double t = (units - k) * unit * 60.0 / section.bpm;        // seconds since the click
            if (t > 0.05)
                continue;
            const double freq = std::fmod (k, (double) section.numerator) < 0.5 ? 1500.0 : 1000.0;
            const auto s = (float) (std::sin (juce::MathConstants<double>::twoPi * freq * t) * std::exp (-t * 80.0) * 0.4);
            outL[i] += s;
            if (outR) outR[i] += s;
        }
    }

    // Soft limiter: transparent below 0.8, rounds peaks off instead of hard-clipping.
    auto softClip = [] (float x)
    {
        constexpr float knee = 0.8f;
        const float a = std::abs (x);
        if (a <= knee) return x;
        return std::copysign (knee + (1.0f - knee) * std::tanh ((a - knee) / (1.0f - knee)), x);
    };

    // Stems skip the master volume and the limiter, so that they add up to the mix instead of being limited one by one.
    const float masterRamp = n > 0 ? 1.0f / (float) n : 0.0f;
    float mp = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float mg = masterGain0 + (masterGain1 - masterGain0) * (float) i * masterRamp;
        outL[i] = renderMaster ? softClip (outL[i] * mg) : outL[i];
        mp = juce::jmax (mp, std::abs (outL[i]));
        if (outR)
        {
            outR[i] = renderMaster ? softClip (outR[i] * mg) : outR[i];
            mp = juce::jmax (mp, std::abs (outR[i]));
        }
    }
    setMasterLevel (mp);
}

// True if a track has something to put in a stem: clips, or notes routed to it by a sequencer plugin.
bool Engine::trackHasOutput (const Track& t) const
{
    const juce::ScopedLock sl (lock);
    if (t.bus)
        return false;   // buses are the sum of what plays into them: no stem of their own
    if (t.type == Track::Type::Midi && t.instrument != nullptr && t.routesElsewhere())
        return false;   // a sequencer track: silent itself, its notes sound on other tracks or on hardware
    if (t.type == Track::Type::Midi && t.instrument == nullptr && ! t.externalOuts.empty())
        return false;   // plays hardware instead of the built-in sound: nothing to put in a stem
    if (! t.clips.empty() || ! t.midiClips.empty())
        return true;
    for (auto& other : tracks)
        for (auto& route : other->midiOuts)
            if (route.target == &t)
                return true;
    return false;
}

bool Engine::renderToFile (const juce::File& file, double seconds, bool withMetronome)
{
    RenderOptions options;
    options.withMetronome = withMetronome;
    return renderToFile (file, seconds, options);
}

// Offline render into a WAV file. The lock is only held block by block, so the interface stays alive during long
// renders; while it runs the audio callback is silent (offlineRendering) so it can't disturb the shared scratch state.
bool Engine::renderToFile (const juce::File& file, double seconds, const RenderOptions& options)
{
    if (seconds <= 0.0)
        return false;

    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return false;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate, 2, options.bits, {}, 0));
    if (writer == nullptr)
        return false;
    stream.release(); // the writer owns the stream now

    struct Offline   // marks the engine as busy for the whole render, whatever way it ends
    {
        explicit Offline (std::atomic<bool>& f) : flag (f) { flag = true; }
        ~Offline() { flag = false; }
        std::atomic<bool>& flag;
    } offline (offlineRendering);

    {
        const juce::ScopedLock sl (lock);
        silenceSynths();
        renderOnly = options.onlyTrack;
        renderMaster = options.masterProcessing;
    }

    constexpr int blockSize = 1024;
    juce::AudioBuffer<float> buffer (2, blockSize);
    const auto total = (juce::int64) (seconds * sampleRate);
    bool ok = true;

    for (juce::int64 done = 0; done < total && ok; done += blockSize)
    {
        if (options.shouldCancel && options.shouldCancel())
        {
            ok = false;
            break;
        }

        const int n = (int) juce::jmin<juce::int64> (blockSize, total - done);
        buffer.clear();
        {
            const juce::ScopedLock sl (lock);
            mixBlock (buffer.getWritePointer (0), buffer.getWritePointer (1), n, done, options.withMetronome, true);
        }
        ok = writer->writeFromAudioSampleBuffer (buffer, 0, n);

        if (options.onProgress)
            options.onProgress ((double) (done + n) / (double) total);
    }

    {
        const juce::ScopedLock sl (lock);
        silenceSynths();
        lastBlockEnd = -1;
        renderOnly = nullptr;
        renderMaster = true;
    }
    return ok;
}
