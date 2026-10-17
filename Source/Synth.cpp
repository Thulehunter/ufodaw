#include "Synth.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr double twoPi = 6.283185307179586;

    // Polynomial band-limited step correction, removes most of the aliasing of saw/square.
    inline float polyBlep (double t, double dt)
    {
        if (t < dt)
        {
            t /= dt;
            return (float) (t + t - t * t - 1.0);
        }
        if (t > 1.0 - dt)
        {
            t = (t - 1.0) / dt;
            return (float) (t * t + t + t + 1.0);
        }
        return 0.0f;
    }

    inline float oscillator (int waveform, double phase, double dt)
    {
        switch (waveform)
        {
            case SynthParams::Sine:     return (float) std::sin (twoPi * phase);
            case SynthParams::Saw:      return (float) (2.0 * phase - 1.0) - polyBlep (phase, dt);
            case SynthParams::Square:
            {
                float v = phase < 0.5 ? 1.0f : -1.0f;
                v += polyBlep (phase, dt);
                v -= polyBlep (std::fmod (phase + 0.5, 1.0), dt);
                return v;
            }
            default:                    return (float) (4.0 * std::abs (phase - 0.5) - 1.0);   // triangle
        }
    }
}

void Synth::prepare (double rate)
{
    sampleRate = rate;
    reset();
}

void Synth::noteOn (int pitch, float velocity)
{
    Voice* target = nullptr;
    for (auto& v : voices)
        if (v.stage == Stage::Idle) { target = &v; break; }

    if (target == nullptr)   // steal the quietest voice
        target = &*std::min_element (voices.begin(), voices.end(),
                                     [] (const Voice& a, const Voice& b) { return a.level < b.level; });

    *target = Voice();
    target->stage = Stage::Attack;
    target->pitch = pitch;
    target->velocity = velocity;
    target->increment = 440.0 * std::pow (2.0, (pitch - 69) / 12.0) / sampleRate;
    target->age = ++counter;
}

void Synth::noteOff (int pitch)
{
    Voice* oldest = nullptr;
    for (auto& v : voices)
        if (v.pitch == pitch && (v.stage == Stage::Attack || v.stage == Stage::Decay || v.stage == Stage::Sustain)
            && (oldest == nullptr || v.age < oldest->age))
            oldest = &v;

    if (oldest != nullptr)
    {
        oldest->stage = Stage::Release;
        oldest->releaseStep = -1.0f;   // computed on the first sample, when the release time is known
    }
}

void Synth::allNotesOff()
{
    for (auto& v : voices)
        if (v.stage != Stage::Idle && v.stage != Stage::Release)
        {
            v.stage = Stage::Release;
            v.releaseStep = -1.0f;
        }
}

void Synth::reset()
{
    for (auto& v : voices)
        v = Voice();
}

bool Synth::isActive() const
{
    for (auto& v : voices)
        if (v.stage != Stage::Idle)
            return true;
    return false;
}

void Synth::render (float* out, int numSamples, const SynthParams& p)
{
    const int waveform = p.waveform;
    const float attackStep = 1.0f / (float) std::max (1.0, p.attack.load() * sampleRate);
    const float sustain = p.sustain;
    const float decayStep = (1.0f - sustain) / (float) std::max (1.0, p.decay.load() * sampleRate);
    const double releaseSamples = std::max (1.0, p.release.load() * sampleRate);
    const float cutoff = std::min (p.cutoff.load(), (float) (sampleRate * 0.45));
    const float filterCoeff = 1.0f - std::exp ((float) (-twoPi * cutoff / sampleRate));

    for (auto& v : voices)
    {
        if (v.stage == Stage::Idle)
            continue;

        for (int i = 0; i < numSamples; ++i)
        {
            switch (v.stage)
            {
                case Stage::Attack:
                    v.level += attackStep;
                    if (v.level >= 1.0f) { v.level = 1.0f; v.stage = Stage::Decay; }
                    break;
                case Stage::Decay:
                    v.level -= decayStep;
                    if (v.level <= sustain) { v.level = sustain; v.stage = Stage::Sustain; }
                    break;
                case Stage::Sustain:
                    break;
                case Stage::Release:
                    if (v.releaseStep < 0.0f)
                        v.releaseStep = v.level / (float) releaseSamples;
                    v.level -= v.releaseStep;
                    if (v.level <= 0.0001f) { v.stage = Stage::Idle; v.level = 0.0f; }
                    break;
                case Stage::Idle:
                    break;
            }
            if (v.stage == Stage::Idle)
                break;

            const float raw = oscillator (waveform, v.phase, v.increment);
            v.filterState += filterCoeff * (raw - v.filterState);
            out[i] += v.filterState * v.level * v.velocity * 0.22f;

            v.phase += v.increment;
            if (v.phase >= 1.0)
                v.phase -= 1.0;
        }
    }
}
