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

Track* Engine::addTrackFromFile (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return nullptr;

    auto track = std::make_unique<Track>();
    track->name = file.getFileNameWithoutExtension();
    track->fileRate = reader->sampleRate;
    track->audio.setSize ((int) juce::jmin<juce::int64> (reader->numChannels, 2), (int) reader->lengthInSamples);
    reader->read (&track->audio, 0, (int) reader->lengthInSamples, 0, true, true);

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

void Engine::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();

    const juce::ScopedTryLock sl (lock);
    if (! sl.isLocked() || ! playing)
    {
        for (auto& t : tracks) t->level = 0.0f;
        masterLevel = 0.0f;
        return;
    }

    auto* outL = info.buffer->getWritePointer (0, info.startSample);
    auto* outR = info.buffer->getNumChannels() > 1 ? info.buffer->getWritePointer (1, info.startSample) : nullptr;
    const auto n = info.numSamples;
    const auto pos = positionSamples.load();

    bool anySolo = false;
    for (auto& t : tracks)
        anySolo = anySolo || t->solo;

    for (auto& t : tracks)
    {
        float peak = 0.0f;
        const bool audible = ! t->mute && (! anySolo || t->solo);
        const double ratio = t->fileRate / sampleRate;
        const double startSample = t->startSeconds * sampleRate;
        const int len = t->audio.getNumSamples();
        const float g = t->gain;
        const float angle = (t->pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        const float gl = std::cos (angle) * juce::MathConstants<float>::sqrt2 * g;
        const float gr = std::sin (angle) * juce::MathConstants<float>::sqrt2 * g;
        const auto* srcL = t->audio.getReadPointer (0);
        const auto* srcR = t->audio.getNumChannels() > 1 ? t->audio.getReadPointer (1) : srcL;

        for (int i = 0; i < n; ++i)
        {
            const double sp = ((double) (pos + i) - startSample) * ratio;
            if (sp < 0.0 || sp >= len - 1)
                continue;

            const int i0 = (int) sp;
            const float frac = (float) (sp - i0);
            const float l = srcL[i0] + frac * (srcL[i0 + 1] - srcL[i0]);
            const float r = srcR[i0] + frac * (srcR[i0 + 1] - srcR[i0]);
            peak = juce::jmax (peak, std::abs (l), std::abs (r));

            if (audible)
            {
                outL[i] += l * gl;
                if (outR) outR[i] += r * gr;
            }
        }
        t->level = peak * g;
    }

    if (metronome)
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

    const float mg = masterGain;
    float mp = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        outL[i] *= mg;
        mp = juce::jmax (mp, std::abs (outL[i]));
        if (outR)
        {
            outR[i] *= mg;
            mp = juce::jmax (mp, std::abs (outR[i]));
        }
    }
    masterLevel = mp;

    positionSamples += n;
}
