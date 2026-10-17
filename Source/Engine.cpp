#include "Engine.h"
#include <algorithm>
#include <cmath>

Engine::Engine()
{
    formats.registerBasicFormats();
}

void Engine::prepareToPlay (int, double newRate)
{
    sampleRate = newRate;
}

//==============================================================================
// Tracks

Track* Engine::addTrack (const juce::String& name)
{
    auto track = std::make_unique<Track>();
    track->name = name;
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

int Engine::addClipFromFile (Track& track, const juce::File& file, double startSeconds)
{
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

bool Engine::moveClip (Track& t, int id, double newStart)
{
    const juce::ScopedLock sl (lock);
    if (auto* c = findClip (t, id))
    {
        c->start = juce::jmax (0.0, newStart);
        return true;
    }
    return false;
}

int Engine::splitClip (Track& t, int id, double at)
{
    const juce::ScopedLock sl (lock);
    auto* c = findClip (t, id);
    constexpr double minPart = 0.01;
    if (c == nullptr || at <= c->start + minPart || at >= c->end() - minPart)
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

int Engine::duplicateClip (Track& t, int id)
{
    const juce::ScopedLock sl (lock);
    auto* c = findClip (t, id);
    if (c == nullptr)
        return -1;

    Clip copy = *c;
    copy.id = nextClipId++;
    copy.start = c->end();
    const int newId = copy.id;
    t.clips.push_back (std::move (copy));
    return newId;
}

bool Engine::deleteClip (Track& t, int id)
{
    const juce::ScopedLock sl (lock);
    const auto before = t.clips.size();
    t.clips.erase (std::remove_if (t.clips.begin(), t.clips.end(), [id] (auto& c) { return c.id == id; }),
                   t.clips.end());
    return t.clips.size() != before;
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

double Engine::getTotalLengthSeconds() const
{
    const juce::ScopedLock sl (lock);
    double end = 0.0;
    for (auto& t : tracks)
        for (auto& c : t->clips)
            end = juce::jmax (end, c.end());
    return end;
}

//==============================================================================
// Audio

void Engine::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();

    const juce::ScopedTryLock sl (lock);
    if (! sl.isLocked() || ! playing)
    {
        if (sl.isLocked())
            for (auto& t : tracks) t->level = 0.0f;
        masterLevel = 0.0f;
        return;
    }

    auto* outL = info.buffer->getWritePointer (0, info.startSample);
    auto* outR = info.buffer->getNumChannels() > 1 ? info.buffer->getWritePointer (1, info.startSample) : nullptr;

    mixBlock (outL, outR, info.numSamples, positionSamples.load(), metronome);
    positionSamples += info.numSamples;
}

void Engine::mixBlock (float* outL, float* outR, int n, juce::int64 pos, bool withMetronome)
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

            for (int i = 0; i < n; ++i)
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
                peak = juce::jmax (peak, std::abs (l), std::abs (r));

                if (audible)
                {
                    outL[i] += l * gl;
                    if (outR) outR[i] += r * gr;
                }
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
    constexpr int blockSize = 1024;
    juce::AudioBuffer<float> buffer (2, blockSize);
    const auto total = (juce::int64) (seconds * sampleRate);

    for (juce::int64 done = 0; done < total; done += blockSize)
    {
        const int n = (int) juce::jmin<juce::int64> (blockSize, total - done);
        buffer.clear();
        mixBlock (buffer.getWritePointer (0), buffer.getWritePointer (1), n, done, withMetronome);
        if (! writer->writeFromAudioSampleBuffer (buffer, 0, n))
            return false;
    }
    return true;
}
