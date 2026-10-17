#include "Engine.h"
#include <algorithm>
#include <cmath>

Engine::Engine()
{
    formats.registerBasicFormats();
    writerThread.startThread();
    noteEvents.reserve (2048);
    synthScratch.assign (16384, 0.0f);
    trackL.assign (16384, 0.0f);
    trackR.assign (16384, 0.0f);
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
    if ((int) synthScratch.size() < samplesPerBlock * 2)
    {
        synthScratch.assign ((size_t) samplesPerBlock * 2, 0.0f);
        trackL.assign ((size_t) samplesPerBlock * 2, 0.0f);
        trackR.assign ((size_t) samplesPerBlock * 2, 0.0f);
    }
    for (auto& t : tracks)
    {
        if (t->voices != nullptr)
            t->voices->prepare (newRate);
        t->fxState->prepare (newRate);
    }
}

//==============================================================================
// Recording

bool Engine::startRecording (const juce::File& file, int numChannels)
{
    stopRecording();
    file.getParentDirectory().createDirectory();
    file.deleteFile();

    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return false;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate,
                                                                         (unsigned int) juce::jlimit (1, 2, numChannels), 24, {}, 0));
    if (writer == nullptr)
        return false;
    stream.release(); // the writer owns the stream now

    threadedWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), writerThread, 32768);
    {
        const juce::SpinLock::ScopedLockType sl (recLock);
        activeWriter = threadedWriter.get();
    }
    return true;
}

void Engine::stopRecording()
{
    {
        const juce::SpinLock::ScopedLockType sl (recLock);
        activeWriter = nullptr;
    }
    threadedWriter.reset();   // flushes what is left and closes the file
}

//==============================================================================
// Tracks

Track* Engine::addTrack (const juce::String& name, Track::Type type)
{
    auto track = std::make_unique<Track>();
    track->name = name;
    track->type = type;
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

void Engine::removeTrack (Track* t)
{
    const juce::ScopedLock sl (lock);
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
        c->start = juce::jmax (0.0, newStart);
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

    if (auto* c = findClip (t, id))
    {
        if (at <= c->start + minPart || at >= c->end() - minPart)
            return -1;

        Clip right = *c;
        right.id = nextClipId++;
        right.start = at;
        right.offset = c->offset + (at - c->start);
        right.length = c->end() - at;
        c->length = at - c->start;   // `c` is not used after the push_back below

        const int newId = right.id;
        t.clips.push_back (std::move (right));
        return newId;
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
    t.clips.erase (std::remove_if (t.clips.begin(), t.clips.end(), [id] (auto& c) { return c.id == id; }), t.clips.end());
    t.midiClips.erase (std::remove_if (t.midiClips.begin(), t.midiClips.end(), [id] (auto& c) { return c.id == id; }),
                       t.midiClips.end());
    return t.clips.size() + t.midiClips.size() != before;
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
// Note preview (UI thread -> audio thread)

void Engine::previewNote (const Track& t, int pitch, bool on)
{
    int s1, n1, s2, n2;
    previewFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0)
        previewQueue[(size_t) s1] = { &t, pitch, on };
    else if (n2 > 0)
        previewQueue[(size_t) s2] = { &t, pitch, on };
    previewFifo.finishedWrite (n1 + n2);
}

void Engine::drainPreviewEvents()
{
    int s1, n1, s2, n2;
    previewFifo.prepareToRead (previewFifo.getNumReady(), s1, n1, s2, n2);

    auto apply = [this] (const PreviewEvent& e)
    {
        for (auto& t : tracks)
            if (t.get() == e.track && t->voices != nullptr)
            {
                if (e.on) t->voices->noteOn (e.pitch, 0.8f); else t->voices->noteOff (e.pitch);
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
        if (t->voices != nullptr && t->voices->isActive())
            return true;
    return false;
}

void Engine::silenceSynths()
{
    for (auto& t : tracks)
    {
        if (t->voices != nullptr)
            t->voices->reset();
        t->fxState->reset();
    }
}

double Engine::getRenderTailSeconds() const
{
    const juce::ScopedLock sl (lock);
    for (auto& t : tracks)
        if (t->fx.hasTail())
            return 3.0;
    return 0.5;
}

//==============================================================================
// Audio

void Engine::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    // The buffer still holds the device input here, so take it before clearing it for output.
    if (playing)
    {
        const juce::SpinLock::ScopedTryLockType rl (recLock);
        if (rl.isLocked())
            if (auto* w = activeWriter.load())
            {
                const float* in[2];
                in[0] = info.buffer->getReadPointer (0, info.startSample);
                in[1] = info.buffer->getNumChannels() > 1 ? info.buffer->getReadPointer (1, info.startSample) : in[0];
                w->write (in, info.numSamples);
            }
    }

    info.clearActiveBufferRegion();

    const juce::ScopedTryLock sl (lock);
    if (! sl.isLocked())
    {
        masterLevel = 0.0f;
        return;
    }

    drainPreviewEvents();

    const bool running = playing;
    const auto pos = positionSamples.load();

    if (! running)
    {
        if (wasRunning)
        {
            for (auto& t : tracks)
                if (t->voices != nullptr)
                    t->voices->allNotesOff();
            wasRunning = false;
        }
        if (! anySynthActive())
        {
            for (auto& t : tracks) t->level = 0.0f;
            masterLevel = 0.0f;
            return;
        }
    }
    else
    {
        if (! wasRunning || pos != lastBlockEnd)   // playback started or the playhead jumped
            for (auto& t : tracks)
            {
                if (t->voices != nullptr)
                    t->voices->allNotesOff();
                if (t->fx.hasTail())
                    t->fxState->reset();   // don't replay the tail of wherever we were before
            }
        wasRunning = true;
    }

    auto* outL = info.buffer->getWritePointer (0, info.startSample);
    auto* outR = info.buffer->getNumChannels() > 1 ? info.buffer->getWritePointer (1, info.startSample) : nullptr;

    mixBlock (outL, outR, info.numSamples, pos, running && metronome, running);

    if (running)
    {
        positionSamples += info.numSamples;
        lastBlockEnd = pos + info.numSamples;
    }
}

void Engine::renderMidiTrack (Track& t, int n, juce::int64 pos, bool running, float* scratch)
{
    std::fill (scratch, scratch + n, 0.0f);

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

        // note-offs go first at the same sample so a repeated pitch retriggers cleanly
        std::sort (noteEvents.begin(), noteEvents.end(), [] (const NoteEvent& a, const NoteEvent& b)
                   { return a.offset != b.offset ? a.offset < b.offset : (! a.on && b.on); });
    }

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

    for (auto& t : tracks)
    {
        float peak = 0.0f;
        const bool audible = ! t->mute && (! anySolo || t->solo);
        const float g = t->gain;
        const float angle = (t->pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        const float gl = std::cos (angle) * juce::MathConstants<float>::sqrt2 * g;
        const float gr = std::sin (angle) * juce::MathConstants<float>::sqrt2 * g;

        // Each track is built in its own stereo buffer, then goes through its effects
        // and the fader/pan on the way into the master mix.
        float* const bufL = trackL.data();
        float* const bufR = trackR.data();
        const int nn = juce::jmin (n, (int) trackL.size());
        std::fill (bufL, bufL + nn, 0.0f);
        std::fill (bufR, bufR + nn, 0.0f);

        if (t->type == Track::Type::Midi)
        {
            if (t->voices != nullptr)
            {
                renderMidiTrack (*t, nn, pos, running, bufL);
                std::copy (bufL, bufL + nn, bufR);
            }
        }
        else if (! running)
        {
            t->level = 0.0f;
            continue;
        }

        for (auto& c : t->clips)
        {
            const double clipStart = c.start * sampleRate;
            const double clipEnd = c.end() * sampleRate;
            if ((double) (pos + n) <= clipStart || (double) pos >= clipEnd || c.audio == nullptr)
                continue;

            const double ratio = c.fileRate / sampleRate;
            const double offsetSamples = c.offset * c.fileRate;
            const int len = c.audio->getNumSamples();
            const auto* srcL = c.audio->getReadPointer (0);
            const auto* srcR = c.audio->getNumChannels() > 1 ? c.audio->getReadPointer (1) : srcL;

            for (int i = 0; i < nn; ++i)
            {
                const double tl = (double) (pos + i);
                if (tl < clipStart || tl >= clipEnd)
                    continue;

                const double sp = (tl - clipStart) * ratio + offsetSamples;
                if (sp < 0.0 || sp >= len - 1)
                    continue;

                const int i0 = (int) sp;
                const float frac = (float) (sp - i0);
                const float edge = (float) juce::jmin (1.0, juce::jmin ((tl - clipStart) / fadeSamples,
                                                                        (clipEnd - tl) / fadeSamples));
                const float l = (srcL[i0] + frac * (srcL[i0 + 1] - srcL[i0])) * edge;
                const float r = (srcR[i0] + frac * (srcR[i0 + 1] - srcR[i0])) * edge;
                bufL[i] += l;
                bufR[i] += r;
            }
        }

        if (t->fx.anyOn())
            t->fxState->process (bufL, bufR, nn, t->fx);

        for (int i = 0; i < nn; ++i)
        {
            peak = juce::jmax (peak, std::abs (bufL[i]), std::abs (bufR[i]));
            if (audible)
            {
                outL[i] += bufL[i] * gl;
                if (outR) outR[i] += bufR[i] * gr;
            }
        }
        t->level = peak * g;
    }

    if (withMetronome)
    {
        const double samplesPerBeat = sampleRate * 60.0 / juce::jmax (20.0f, bpm.load());
        for (int i = 0; i < n; ++i)
        {
            const double inBeat = std::fmod ((double) (pos + i), samplesPerBeat);
            if (inBeat > sampleRate * 0.05)
                continue;
            const auto beat = (juce::int64) (((double) (pos + i)) / samplesPerBeat);
            const double t = inBeat / sampleRate;
            const double freq = (beat % 4 == 0) ? 1500.0 : 1000.0;
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

    const float mg = masterGain;
    float mp = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        outL[i] = softClip (outL[i] * mg);
        mp = juce::jmax (mp, std::abs (outL[i]));
        if (outR)
        {
            outR[i] = softClip (outR[i] * mg);
            mp = juce::jmax (mp, std::abs (outR[i]));
        }
    }
    masterLevel = mp;
}

bool Engine::renderToFile (const juce::File& file, double seconds, bool withMetronome)
{
    if (seconds <= 0.0)
        return false;

    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return false;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sampleRate, 2, 24, {}, 0));
    if (writer == nullptr)
        return false;
    stream.release(); // the writer owns the stream now

    const juce::ScopedLock sl (lock);
    silenceSynths();

    constexpr int blockSize = 1024;
    juce::AudioBuffer<float> buffer (2, blockSize);
    const auto total = (juce::int64) (seconds * sampleRate);
    bool ok = true;

    for (juce::int64 done = 0; done < total && ok; done += blockSize)
    {
        const int n = (int) juce::jmin<juce::int64> (blockSize, total - done);
        buffer.clear();
        mixBlock (buffer.getWritePointer (0), buffer.getWritePointer (1), n, done, withMetronome, true);
        ok = writer->writeFromAudioSampleBuffer (buffer, 0, n);
    }

    silenceSynths();
    lastBlockEnd = -1;
    return ok;
}
