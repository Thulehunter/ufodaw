#include "Synth.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr double twoPi = 6.283185307179586;
    constexpr double ln2 = 0.6931471805599453;
    constexpr int controlBlock = 16;   // filter coefficients, LFO and pitch are updated this often (samples)

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

    inline float oscillator (int waveform, double phase, double dt, double pulseWidth)
    {
        switch (waveform)
        {
            case SynthParams::Sine:     return (float) std::sin (twoPi * phase);
            case SynthParams::Saw:      return (float) (2.0 * phase - 1.0) - polyBlep (phase, dt);
            case SynthParams::Square:
            {
                float v = phase < pulseWidth ? 1.0f : -1.0f;
                v += polyBlep (phase, dt);
                v -= polyBlep (std::fmod (phase + 1.0 - pulseWidth, 1.0), dt);
                return v - (float) (2.0 * pulseWidth - 1.0);   // (no DC offset for a narrow pulse)
            }
            default:                    return (float) (4.0 * std::abs (phase - 0.5) - 1.0);   // triangle
        }
    }

    inline double logFrequency (int pitch) { return std::log (440.0) + (double) (pitch - 69) / 12.0 * ln2; }

}

void Synth::prepare (double rate)
{
    sampleRate = rate;
    reset();
}

void Synth::step (Env& e, float attackStep, float decayCoef, float sustain, float releaseCoef)
{
    switch (e.stage)
    {
        case Stage::Attack:
            e.level += attackStep;
            if (e.level >= 1.0f) { e.level = 1.0f; e.stage = Stage::Decay; }
            break;
        case Stage::Decay:
            e.level += (sustain - e.level) * decayCoef;
            if (std::abs (e.level - sustain) < 0.0005f) { e.level = sustain; e.stage = Stage::Sustain; }
            break;
        case Stage::Sustain:
            e.level = sustain;
            break;
        case Stage::Release:
            e.level -= e.level * releaseCoef;
            if (e.level < 0.0001f) { e.level = 0.0f; e.stage = Stage::Idle; }
            break;
        case Stage::Idle:
            break;
    }
}

void Synth::noteOn (int pitch, float velocity, const SynthParams& params)
{
    const bool piano = params.engine.load() == 1;
    const bool mono = params.mono.load() == 1 && ! piano;
    monoMode = mono;
    if (! mono)
        numHeld = 0;
    const float glide = params.glide.load();

    Voice* target = nullptr;
    if (mono)
    {
        // one voice: a note that comes while another is held glides to the new pitch without starting the envelopes again (legato)
        if (numHeld < 16)
            heldNotes[numHeld++] = pitch;
        target = &voices[0];
        if (target->active() && target->amp.stage != Stage::Release && ! target->piano)
        {
            target->pitch = pitch;
            target->targetLog = logFrequency (pitch);
            if (glide <= 0.0f)
                target->currentLog = target->targetLog;
            lastLog = target->targetLog;
            target->velocity = velocity;
            return;
        }
    }
    else
    {
        for (auto& v : voices)
            if (! v.active()) { target = &v; break; }

        if (target == nullptr)   // steal the quietest voice
            target = &*std::min_element (voices.begin(), voices.end(),
                                         [] (const Voice& a, const Voice& b) { return a.amp.level < b.amp.level; });
    }

    *target = Voice();
    target->age = ++counter;
    target->pitch = pitch;
    target->velocity = velocity;
    target->targetLog = logFrequency (pitch);
    if (piano)
    {
        startPiano (*target, pitch, velocity, params);
        return;
    }
    target->amp.stage = Stage::Attack;
    target->filt.stage = Stage::Attack;
    target->currentLog = (glide > 0.0f && lastLog != 0.0) ? lastLog : target->targetLog;
    lastLog = target->targetLog;

    auto random01 = [this] { noise = noise * 1664525u + 1013904223u; return (double) (noise >> 8) / 16777216.0; };
    for (auto& ph : target->phase)
        ph = random01();   // (unison voices must not start in step, or they would add up to one loud click)
    target->phase2 = random01();
    target->lfoHeld = (float) (random01() * 2.0 - 1.0);
}

void Synth::noteOff (int pitch)
{
    if (monoMode)
    {
        const int top = numHeld > 0 ? heldNotes[numHeld - 1] : -1;
        int keep = 0;
        for (int i = 0; i < numHeld; ++i)
            if (heldNotes[i] != pitch)
                heldNotes[keep++] = heldNotes[i];
        const bool wasLast = top == pitch;
        numHeld = keep;

        auto& v = voices[0];
        if (numHeld > 0 && v.active() && v.amp.stage != Stage::Release)
        {
            if (wasLast || v.pitch == pitch)   // the sounding key was let go while another is still down: back to that one
            {
                v.pitch = heldNotes[numHeld - 1];
                v.targetLog = logFrequency (v.pitch);
                lastLog = v.targetLog;
            }
            return;
        }
        if (numHeld == 0 && v.active() && v.amp.stage != Stage::Release)
        {
            v.amp.stage = Stage::Release;
            if (v.filt.stage != Stage::Idle)
                v.filt.stage = Stage::Release;
            return;
        }
    }

    Voice* oldest = nullptr;
    for (auto& v : voices)
        if (v.pitch == pitch && v.active() && v.amp.stage != Stage::Release && (oldest == nullptr || v.age < oldest->age))
            oldest = &v;

    if (oldest != nullptr)
    {
        oldest->amp.stage = Stage::Release;
        if (oldest->filt.stage != Stage::Idle)
            oldest->filt.stage = Stage::Release;
    }
}

void Synth::allNotesOff()
{
    for (auto& v : voices)
        if (v.active() && v.amp.stage != Stage::Release)
        {
            v.amp.stage = Stage::Release;
            if (v.filt.stage != Stage::Idle)
                v.filt.stage = Stage::Release;
        }
}

void Synth::reset()
{
    for (auto& v : voices)
        v = Voice();
    lastLog = 0.0;
    numHeld = 0;
}

bool Synth::isActive() const
{
    for (auto& v : voices)
        if (v.active())
            return true;
    return false;
}

void Synth::render (float* outL, float* outR, int numSamples, const SynthParams& params)
{
    const SynthValues p = params.capture();

    const int unison = std::clamp (p.unison, 1, 5);
    const double sr = sampleRate;
    auto timeToStep = [sr] (float seconds) { return 1.0f / (float) std::max (1.0, seconds * sr); };
    auto timeToCoef = [sr] (float seconds) { return 1.0f - std::exp (-4.6f / (float) std::max (1.0, seconds * sr)); };   // (-40 dB after `seconds`)
    const float attackStep = timeToStep (p.attack), fAttackStep = timeToStep (p.fAttack);
    const float decayCoef = timeToCoef (p.decay), fDecayCoef = timeToCoef (p.fDecay);
    const float releaseCoef = timeToCoef (p.release), fReleaseCoef = timeToCoef (p.fRelease);
    const float outGain = 0.22f * p.level;
    const float unisonNorm = 1.0f / std::sqrt ((float) unison);
    const float mixScale = 1.0f / (1.0f + 0.5f * (p.osc2Mix + p.subLevel));
    const double pulseWidth = p.pulseWidth;
    const float driveGain = 1.0f + 5.0f * p.drive, driveComp = 1.0f / (1.0f + 0.5f * p.drive);
    const float k = 2.0f * (1.0f - 0.96f * p.resonance);   // (the damping of the filter: small = more resonance)
    const double maxCutoff = sr * 0.45;
    const double osc2Ratio = std::exp2 ((p.osc2Semi + p.osc2Detune / 100.0) / 12.0);

    // the unison voices: detune and place in the stereo image
    double unisonRatio[5];
    float gainL[5], gainR[5];
    for (int u = 0; u < unison; ++u)
    {
        const double pos = unison == 1 ? 0.0 : ((double) u / (double) (unison - 1)) * 2.0 - 1.0;   // -1 .. 1
        unisonRatio[u] = std::exp2 (pos * p.unisonDetune / 1200.0);
        const double angle = (pos * p.spread + 1.0) * 0.25 * twoPi * 0.5;   // 0 .. pi/2: equal power
        gainL[u] = (float) std::cos (angle) * unisonNorm * 1.4142f;
        gainR[u] = (float) std::sin (angle) * unisonNorm * 1.4142f;
    }

    for (auto& v : voices)
    {
        if (! v.active())
            continue;
        if (v.piano)
        {
            renderPiano (v, outL, outR, numSamples, p);
            continue;
        }

        int done = 0;
        while (done < numSamples && v.active())
        {
            const int chunk = std::min (controlBlock, numSamples - done);

            // ---- control rate: glide, LFO, pitch, filter
            if (p.glide > 0.0f)
                v.currentLog += (v.targetLog - v.currentLog) * (1.0 - std::exp (-4.0 * chunk / (p.glide * sr)));
            else
                v.currentLog = v.targetLog;

            double lfo = 0.0;
            if (p.lfoPitch > 0.0f || p.lfoCutoff > 0.0f || p.lfoAmp > 0.0f)
            {
                switch (p.lfoShape)
                {
                    case 0:  lfo = std::sin (twoPi * v.lfoPhase); break;
                    case 1:  lfo = 4.0 * std::abs (v.lfoPhase - 0.5) - 1.0; break;
                    case 2:  lfo = v.lfoPhase < 0.5 ? 1.0 : -1.0; break;
                    default: lfo = v.lfoHeld; break;
                }
                const double fade = p.lfoDelay > 0.0f ? std::min (1.0, v.lfoTime / p.lfoDelay) : 1.0;
                lfo *= fade;
                v.lfoPhase += p.lfoRate * chunk / sr;
                if (v.lfoPhase >= 1.0)
                {
                    v.lfoPhase -= std::floor (v.lfoPhase);
                    noise = noise * 1664525u + 1013904223u;
                    v.lfoHeld = (float) ((double) (noise >> 8) / 16777216.0 * 2.0 - 1.0);
                }
                v.lfoTime += (double) chunk / sr;
            }

            const double pitchEnvOctaves = p.pitchEnv != 0.0f ? p.pitchEnv / 12.0 * std::exp (-v.voiceTime * 4.6 / p.pitchEnvTime) : 0.0;
            v.voiceTime += (double) chunk / sr;
            const double freq = std::exp (v.currentLog + (lfo * p.lfoPitch / 1200.0 + pitchEnvOctaves) * ln2);
            const double inc = freq / sr;
            double incUnison[5];
            for (int u = 0; u < unison; ++u)
                incUnison[u] = std::min (0.45, inc * unisonRatio[u]);
            const double inc2 = std::min (0.45, inc * osc2Ratio), incSub = std::min (0.45, inc * 0.5);

            const double octaves = p.keyTrack * (v.pitch - 60) / 12.0 + p.filterEnv * v.filt.level + p.lfoCutoff * lfo;
            const double cutoff = std::clamp (p.cutoff * std::exp2 (octaves), 20.0, maxCutoff);
            const float g = (float) std::tan (twoPi * 0.5 * cutoff / sr);
            const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
            const float ampLfo = 1.0f - p.lfoAmp * 0.5f * (1.0f - (float) lfo);   // (a tremolo that never gets louder than the note)
            const float amp = outGain * v.velocity * ampLfo;

            // ---- audio rate
            for (int s = 0; s < chunk; ++s)
            {
                step (v.amp, attackStep, decayCoef, p.sustain, releaseCoef);
                step (v.filt, fAttackStep, fDecayCoef, p.fSustain, fReleaseCoef);
                if (! v.active())
                    break;

                float l = 0.0f, r = 0.0f;
                for (int u = 0; u < unison; ++u)
                {
                    const float x = oscillator (p.waveform, v.phase[u], incUnison[u], pulseWidth);
                    l += x * gainL[u];
                    r += x * gainR[u];
                    v.phase[u] += incUnison[u];
                    if (v.phase[u] >= 1.0) v.phase[u] -= 1.0;
                }
                if (p.osc2Mix > 0.0f)
                {
                    const float x = oscillator (p.osc2Wave, v.phase2, inc2, pulseWidth) * p.osc2Mix;
                    l += x;
                    r += x;
                    v.phase2 += inc2;
                    if (v.phase2 >= 1.0) v.phase2 -= 1.0;
                }
                if (p.subLevel > 0.0f)
                {
                    const float x = (float) std::sin (twoPi * v.phaseSub) * p.subLevel;
                    l += x;
                    r += x;
                    v.phaseSub += incSub;
                    if (v.phaseSub >= 1.0) v.phaseSub -= 1.0;
                }
                l *= mixScale;
                r *= mixScale;

                if (p.drive > 0.0f)
                {
                    l = std::tanh (l * driveGain) * driveComp;
                    r = std::tanh (r * driveGain) * driveComp;
                }

                // the filter (a 12 dB state-variable low-pass, one per channel)
                const float v3L = l - v.ic2L, v1L = a1 * v.ic1L + a2 * v3L, v2L = v.ic2L + a2 * v.ic1L + a3 * v3L;
                v.ic1L = 2.0f * v1L - v.ic1L;
                v.ic2L = 2.0f * v2L - v.ic2L;
                const float v3R = r - v.ic2R, v1R = a1 * v.ic1R + a2 * v3R, v2R = v.ic2R + a2 * v.ic1R + a3 * v3R;
                v.ic1R = 2.0f * v1R - v.ic1R;
                v.ic2R = 2.0f * v2R - v.ic2R;

                const float gain = amp * v.amp.level;
                outL[done + s] += v2L * gain;
                outR[done + s] += v2R * gain;
            }
            done += chunk;
        }
    }
}

//==============================================================================
// The piano engine. A piano string is a set of nearly harmonic partials (a little stretched: the stiffness of the string), each one dying away at its
// own speed (the high ones first). A struck note has two or three strings tuned a hair apart, so the sound beats softly; the hammer adds a short
// burst of noise. Every partial is a two-pole resonator, so the whole note costs about thirty multiplications per sample.

void Synth::startPiano (Voice& v, int pitch, float velocity, const SynthParams& params)
{
    const SynthValues p = params.capture();
    const double sr = sampleRate;
    const double f0 = 440.0 * std::pow (2.0, (pitch - 69) / 12.0);
    const double inharmonicity = std::clamp (4.0e-4 * std::pow (2.0, (pitch - 60) / 14.0), 5.0e-5, 1.0e-2);
    const double ringTime = p.pianoDecay * std::clamp (14.0 * std::pow (2.0, -(pitch - 36) / 16.0), 0.8, 24.0);   // seconds to -60 dB, fundamental
    const double slope = 1.4 - 0.7 * velocity * (0.4 + 0.6 * p.pianoBright);
    const double stringCents = p.pianoDetune * 1.6;
    const double centre = std::clamp ((pitch - 60) / 36.0, -1.0, 1.0) * p.spread * 0.6;   // low notes to the left, high notes to the right

    struct Partial { double freq, amp, time, pan; };
    Partial partials[Voice::maxPartials];
    int count = 0;
    double energy = 0.0;
    for (int n = 1; n <= 24 && count < Voice::maxPartials - 1; ++n)
    {
        const double fn = n * f0 * std::sqrt (1.0 + inharmonicity * n * n);
        if (fn > 0.45 * sr)
            break;
        double a = std::pow ((double) n, -slope) * std::max (0.15, std::abs (std::sin (3.141592653589793 * n * 0.12)));   // (the hammer hits at about 1/8 of the string)
        a *= std::exp (-n * (1.0 - p.pianoBright) * 0.05);
        const double time = ringTime / (1.0 + 0.55 * (n - 1));
        if (n <= 8 && count < Voice::maxPartials - 2)   // two strings, slightly apart
        {
            const double d = std::pow (2.0, stringCents / 1200.0 * 0.5);
            partials[count++] = { fn / d, a * 0.55, time, centre - 0.12 * p.spread };
            partials[count++] = { fn * d, a * 0.45, time, centre + 0.12 * p.spread };
            energy += a * a * 0.55 * 0.55 + a * a * 0.45 * 0.45;
        }
        else
        {
            partials[count++] = { fn, a, time, centre };
            energy += a * a;
        }
    }

    const double scale = energy > 0.0 ? 1.0 / std::sqrt (energy) : 1.0;
    v.piano = true;
    v.numPartials = count;
    for (int k = 0; k < count; ++k)
    {
        const double w = 2.0 * 3.141592653589793 * partials[k].freq / sr;
        const double r = std::exp (-6.9 / (partials[k].time * sr));
        const double angle = (std::clamp (partials[k].pan, -1.0, 1.0) + 1.0) * 0.25 * 3.141592653589793;
        v.a1[k] = 2.0 * r * std::cos (w);
        v.a2[k] = -r * r;
        v.y1[k] = partials[k].amp * scale * std::sin (w);
        v.y2[k] = 0.0;
        v.partialL[k] = (float) (std::cos (angle) * 1.4142);
        v.partialR[k] = (float) (std::sin (angle) * 1.4142);
    }

    v.lifetime = ringTime * 1.1;
    v.hammerLevel = (float) (p.pianoHammer * velocity * velocity * 0.3);
    v.hammerDecay = (float) std::exp (-1.0 / (0.004 * sr));
    v.hammerCoef = (float) (1.0 - std::exp (-2.0 * 3.141592653589793 * (500.0 + 4000.0 * velocity) / sr));
    v.amp.stage = Stage::Attack;
    v.amp.level = 0.0f;
}

void Synth::renderPiano (Voice& v, float* outL, float* outR, int numSamples, const SynthValues& p)
{
    const double sr = sampleRate;
    const float toneCoef = 1.0f - std::exp ((float) (-2.0 * 3.141592653589793 * std::min ((double) p.cutoff, 0.45 * sr) / sr));
    const float attackStep = 1.0f / (float) (0.002 * sr);
    const float releaseCoef = 1.0f - std::exp (-4.6f / (float) std::max (1.0, p.release * sr));
    const float gain = 0.11f * p.level * std::pow (v.velocity, 1.4f);

    for (int i = 0; i < numSamples; ++i)
    {
        Env& e = v.amp;
        if (e.stage == Stage::Attack)
        {
            e.level += attackStep;
            if (e.level >= 1.0f) { e.level = 1.0f; e.stage = Stage::Sustain; }
        }
        else if (e.stage == Stage::Release)
        {
            e.level -= e.level * releaseCoef;
            if (e.level < 0.0001f) { e.stage = Stage::Idle; e.level = 0.0f; break; }
        }
        v.voiceTime += 1.0 / sr;
        if (v.voiceTime > v.lifetime) { e.stage = Stage::Idle; e.level = 0.0f; break; }

        float l = 0.0f, r = 0.0f;
        for (int k = 0; k < v.numPartials; ++k)
        {
            const double y0 = v.a1[k] * v.y1[k] + v.a2[k] * v.y2[k];
            v.y2[k] = v.y1[k];
            v.y1[k] = y0;
            l += (float) y0 * v.partialL[k];
            r += (float) y0 * v.partialR[k];
        }

        if (v.hammerLevel > 1.0e-5f)   // the hammer
        {
            noise = noise * 1664525u + 1013904223u;
            const float white = (float) ((double) (noise >> 8) / 16777216.0 * 2.0 - 1.0);
            v.hammerLp += v.hammerCoef * (white - v.hammerLp);
            const float h = v.hammerLp * v.hammerLevel;
            v.hammerLevel *= v.hammerDecay;
            l += h;
            r += h;
        }

        v.ic1L += toneCoef * (l - v.ic1L);   // (a one-pole tone control: the cutoff)
        v.ic1R += toneCoef * (r - v.ic1R);
        outL[i] += v.ic1L * e.level * gain;
        outR[i] += v.ic1R * e.level * gain;
    }
}
