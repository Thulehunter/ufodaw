#include "Engine.h"

// The clip launcher. Every track has one cell (LaunchSlot) per scene. Launching a cell puts a looping copy of its clip on the timeline at the next beat
// or bar (the track's launchedAudio / launchedMidi, which the mix plays next to the ordinary clips); launching another cell of the track, or stopping
// it, ends that copy at the next beat or bar. So the audio thread needs no special state: everything is already placed on the timeline in advance.

namespace
{
    constexpr double launchLead = 0.03;     // a launch must be at least this far ahead (the audio thread is a few milliseconds ahead of the clock)
    constexpr double endless = 1.0e7;       // the length of a launched clip until it is stopped (seconds)
}

int Engine::getNumScenes() const
{
    const juce::ScopedLock sl (lock);
    return numScenes;
}

// Rows can be added at will; removing rows only works for empty rows at the end.
void Engine::setNumScenes (int n)
{
    const juce::ScopedLock sl (lock);
    int lastUsed = -1;
    for (auto& t : tracks)
        for (int i = 0; i < (int) t->slots.size(); ++i)
            if (t->slots[(size_t) i].used)
                lastUsed = juce::jmax (lastUsed, i);
    numScenes = juce::jlimit (juce::jmax (1, lastUsed + 1), 64, n);
    for (auto& t : tracks)
        t->slots.resize ((size_t) numScenes);
}

namespace
{
    LaunchSlot* slotAt (Track& t, int scene, int numScenes)
    {
        if (scene < 0 || scene >= numScenes)
            return nullptr;
        if ((int) t.slots.size() < numScenes)
            t.slots.resize ((size_t) numScenes);
        return &t.slots[(size_t) scene];
    }
}

bool Engine::setSlotFromClip (Track& t, int scene, int clipId, bool removeOriginal)
{
    const juce::ScopedLock sl (lock);
    auto* slot = slotAt (t, scene, numScenes);
    if (slot == nullptr || t.bus)
        return false;

    LaunchSlot made;
    made.used = true;
    if (t.type == Track::Type::Audio)
    {
        auto* c = findClip (t, clipId);
        if (c == nullptr || c->audio == nullptr)
            return false;
        made.audio = *c;
        made.audio.start = 0.0;
        made.audio.takeGroup = 0;
        made.name = c->sourceFile != juce::File() ? c->sourceFile.getFileNameWithoutExtension() : juce::String ("Clip");
        if (removeOriginal)
            t.clips.erase (t.clips.begin() + (c - t.clips.data()));
    }
    else
    {
        auto it = std::find_if (t.midiClips.begin(), t.midiClips.end(), [clipId] (const MidiClip& m) { return m.id == clipId; });
        if (it == t.midiClips.end())
            return false;
        made.midi = *it;
        made.midi.start = 0.0;
        made.name = "Pattern";
        if (removeOriginal)
            t.midiClips.erase (it);
    }
    *slot = std::move (made);
    return true;
}

bool Engine::setSlotFromFile (Track& t, int scene, const juce::File& file)
{
    if (t.type != Track::Type::Audio || t.bus)
        return false;

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return false;
    auto buffer = std::make_shared<juce::AudioBuffer<float>> ((int) juce::jmin<juce::uint32> (reader->numChannels, 2), (int) reader->lengthInSamples);
    reader->read (buffer.get(), 0, (int) reader->lengthInSamples, 0, true, true);

    LaunchSlot made;
    made.used = true;
    made.audio.audio = std::move (buffer);
    made.audio.fileRate = reader->sampleRate;
    made.audio.sourceFile = file;
    made.audio.length = made.audio.sourceLength();
    made.name = file.getFileNameWithoutExtension();

    const juce::ScopedLock sl (lock);
    auto* slot = slotAt (t, scene, numScenes);
    if (slot == nullptr)
        return false;
    *slot = std::move (made);
    return true;
}

bool Engine::newSlotMidi (Track& t, int scene, double lengthSeconds)
{
    if (t.type != Track::Type::Midi)
        return false;
    const juce::ScopedLock sl (lock);
    auto* slot = slotAt (t, scene, numScenes);
    if (slot == nullptr)
        return false;
    LaunchSlot made;
    made.used = true;
    made.midi.length = juce::jmax (0.1, lengthSeconds);
    made.name = "Pattern";
    *slot = std::move (made);
    return true;
}

bool Engine::moveSlotToTimeline (Track& t, int scene, double atSeconds)
{
    const juce::ScopedLock sl (lock);
    auto* slot = slotAt (t, scene, numScenes);
    if (slot == nullptr || ! slot->used)
        return false;

    if (t.type == Track::Type::Audio)
    {
        Clip c = slot->audio;
        c.start = juce::jmax (0.0, atSeconds);
        c.loopLength = 0.0;
        c.launchScene = -1;
        c.id = nextClipId++;
        t.clips.push_back (std::move (c));
    }
    else
    {
        MidiClip m = slot->midi;
        m.start = juce::jmax (0.0, atSeconds);
        m.loopLength = 0.0;
        m.launchScene = -1;
        m.id = nextClipId++;
        t.midiClips.push_back (std::move (m));
    }
    *slot = LaunchSlot();
    return true;
}

void Engine::clearSlot (Track& t, int scene)
{
    const juce::ScopedLock sl (lock);
    if (auto* slot = slotAt (t, scene, numScenes))
        *slot = LaunchSlot();
}

void Engine::renameSlot (Track& t, int scene, const juce::String& name)
{
    const juce::ScopedLock sl (lock);
    if (auto* slot = slotAt (t, scene, numScenes))
        slot->name = name;
}

//==============================================================================
// Launching

// The next beat or bar that is far enough ahead; "now" for no quantum.
double Engine::launchBoundary (double now, LaunchQuantum q) const
{
    const double earliest = now + launchLead;
    if (q == LaunchQuantum::None)
        return earliest;

    if (q == LaunchQuantum::Beat)
        return tempoMap.beatsToSeconds (std::ceil (tempoMap.secondsToBeats (earliest) - 1.0e-9));

    const int bar = tempoMap.barAtSeconds (earliest);
    const double start = tempoMap.barStartSeconds (bar);
    return start >= earliest - 1.0e-9 ? start : tempoMap.barStartSeconds (bar + 1);
}

void Engine::endLaunchedAt (Track& t, double at)
{
    t.launchedAudio.erase (std::remove_if (t.launchedAudio.begin(), t.launchedAudio.end(), [at] (const Clip& c) { return c.start >= at; }),
                           t.launchedAudio.end());
    for (auto& c : t.launchedAudio)
        if (c.end() > at)
        {
            c.length = at - c.start;
            c.fadeOut = juce::jmin (0.01, c.length);   // (a short fade, so the cut does not click)
        }

    t.launchedMidi.erase (std::remove_if (t.launchedMidi.begin(), t.launchedMidi.end(), [at] (const MidiClip& m) { return m.start >= at; }),
                          t.launchedMidi.end());
    for (auto& m : t.launchedMidi)
        if (m.end() > at)
            m.length = at - m.start;
}

namespace
{
    void clearAllLaunched (std::vector<std::unique_ptr<Track>>& tracks)
    {
        for (auto& t : tracks)
        {
            t->launchedAudio.clear();
            t->launchedMidi.clear();
        }
    }
}

void Engine::launchSlot (Track& t, int scene)
{
    const juce::ScopedLock sl (lock);
    auto* slot = slotAt (t, scene, numScenes);
    if (slot == nullptr || ! slot->used || t.bus)
        return;

    const bool wasPlaying = playing;
    if (! wasPlaying)
        clearAllLaunched (tracks);   // (left over from before the transport stopped)

    const double now = getPositionSeconds();
    const double at = wasPlaying ? launchBoundary (now, (LaunchQuantum) launchQuantum.load()) : now;
    endLaunchedAt (t, at);

    if (t.type == Track::Type::Audio)
    {
        Clip c = slot->audio;
        c.id = nextClipId++;
        c.start = at;
        c.loopLength = juce::jmax (0.01, c.length);
        c.length = endless;
        c.fadeIn = 0.002;
        c.fadeOut = 0.0;
        c.launchScene = scene;
        t.launchedAudio.push_back (std::move (c));
    }
    else
    {
        MidiClip m = slot->midi;
        m.id = nextClipId++;
        m.start = at;
        m.loopLength = juce::jmax (0.01, m.length);
        m.length = endless;
        m.launchScene = scene;
        t.launchedMidi.push_back (std::move (m));
    }

    launcherClear = false;
    if (! wasPlaying)
        playing = true;
}

void Engine::launchScene (int scene)
{
    const juce::ScopedLock sl (lock);
    for (auto& t : tracks)
        launchSlot (*t, scene);   // (each launch lands on the same boundary: the position does not move while the lock is held by the callers' thread order)
}

void Engine::stopTrackSlots (Track& t)
{
    const juce::ScopedLock sl (lock);
    const double now = getPositionSeconds();
    endLaunchedAt (t, playing ? launchBoundary (now, (LaunchQuantum) launchQuantum.load()) : now);
}

void Engine::stopAllSlots()
{
    const juce::ScopedLock sl (lock);
    const double at = playing ? launchBoundary (getPositionSeconds(), (LaunchQuantum) launchQuantum.load()) : getPositionSeconds();
    for (auto& t : tracks)
        endLaunchedAt (*t, at);
}

Engine::SlotState Engine::getSlotState (const Track& t) const
{
    const juce::ScopedLock sl (lock);
    SlotState s;
    const double now = getPositionSeconds();

    auto consider = [&] (double start, double end, double loop, int scene)
    {
        if (start <= now && end > now)
        {
            s.playing = scene;
            s.stopQueued = end < 1.0e6;
            s.loopPosition = loop > 0.0 ? std::fmod (now - start, loop) / loop : 0.0;
        }
        else if (start > now)
        {
            s.queued = scene;
            s.startsInSeconds = start - now;
        }
    };
    for (auto& c : t.launchedAudio)
        consider (c.start, c.end(), c.loopLength, c.launchScene);
    for (auto& m : t.launchedMidi)
        consider (m.start, m.end(), m.loopLength, m.launchScene);
    return s;
}

std::vector<LaunchSlot> Engine::getSlots (const Track& t) const
{
    const juce::ScopedLock sl (lock);
    auto slots = t.slots;
    slots.resize ((size_t) numScenes);
    return slots;
}
