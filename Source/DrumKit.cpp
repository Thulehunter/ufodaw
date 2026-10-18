#include "DrumKit.h"
#include <cmath>

namespace
{
    constexpr double twoPi = 6.283185307179586;
    constexpr double genRate = 44100.0;

    struct PadInfo { const char* name; int note; };
    // index 0 is drawn at the bottom of the step sequencer
    const PadInfo padInfo[DrumKit::numPads] = {
        { "Kick", 36 }, { "Snare", 38 }, { "Clap", 39 }, { "Closed hat", 42 },
        { "Open hat", 46 }, { "Low tom", 41 }, { "High tom", 45 }, { "Crash", 49 },
        // pads 9..16 start empty: load your own samples
        { "Pad 9", 37 }, { "Pad 10", 40 }, { "Pad 11", 43 }, { "Pad 12", 44 },
        { "Pad 13", 47 }, { "Pad 14", 48 }, { "Pad 15", 50 }, { "Pad 16", 51 }
    };

    using Buffer = juce::AudioBuffer<float>;

    std::shared_ptr<Buffer> makeBuffer (double seconds)
    {
        auto b = std::make_shared<Buffer> (1, (int) (seconds * genRate));
        b->clear();
        return b;
    }

    // Noise with the low end taken out: a cheap way to get something hat/snare-like.
    float brightNoise (juce::Random& rng, float& last)
    {
        const float n = rng.nextFloat() * 2.0f - 1.0f;
        const float out = n - last;
        last = n;
        return out * 0.5f;
    }

    std::shared_ptr<Buffer> sweep (double seconds, double f0, double f1, double sweepRate, double decay, double drive)
    {
        auto b = makeBuffer (seconds);
        auto* d = b->getWritePointer (0);
        double phase = 0.0;
        for (int i = 0; i < b->getNumSamples(); ++i)
        {
            const double t = i / genRate;
            const double f = f1 + (f0 - f1) * std::exp (-t * sweepRate);
            phase += twoPi * f / genRate;
            d[i] = (float) (std::tanh (drive * std::sin (phase)) * std::exp (-t * decay));
        }
        return b;
    }
}


void DrumKit::generateDefault (int pad)
{
    juce::Random rng (1234 + pad);
    float last = 0.0f;
    std::shared_ptr<Buffer> b;

    switch (pad)
    {
        case 0:   // kick: a fast pitch drop plus a short click
        {
            b = sweep (0.5, 160.0, 48.0, 28.0, 7.0, 1.6);
            auto* d = b->getWritePointer (0);
            for (int i = 0; i < 200; ++i)
                d[i] += (rng.nextFloat() * 2.0f - 1.0f) * 0.3f * (1.0f - i / 200.0f);
            break;
        }
        case 1:   // snare: tone + noise
        {
            b = makeBuffer (0.3);
            auto* d = b->getWritePointer (0);
            for (int i = 0; i < b->getNumSamples(); ++i)
            {
                const double t = i / genRate;
                d[i] = (float) (std::sin (twoPi * 190.0 * t) * std::exp (-t * 28.0) * 0.5
                                + brightNoise (rng, last) * std::exp (-t * 16.0) * 0.9);
            }
            break;
        }
        case 2:   // clap: three quick noise bursts and a tail
        {
            b = makeBuffer (0.4);
            auto* d = b->getWritePointer (0);
            for (int i = 0; i < b->getNumSamples(); ++i)
            {
                const double t = i / genRate;
                double env = std::exp (-t * 14.0) * 0.5;
                for (double burst : { 0.0, 0.011, 0.024 })
                    if (t >= burst)
                        env += std::exp (-(t - burst) * 120.0);
                d[i] = (float) (brightNoise (rng, last) * env * 0.7);
            }
            break;
        }
        case 3:   // closed hat
        case 4:   // open hat
        {
            const double length = pad == 3 ? 0.09 : 0.55;
            const double decay = pad == 3 ? 55.0 : 8.0;
            b = makeBuffer (length);
            auto* d = b->getWritePointer (0);
            for (int i = 0; i < b->getNumSamples(); ++i)
                d[i] = (float) (brightNoise (rng, last) * std::exp (-(i / genRate) * decay) * 0.8);
            break;
        }
        case 5:  b = sweep (0.5, 190.0, 95.0, 14.0, 7.0, 1.0); break;    // low tom
        case 6:  b = sweep (0.45, 260.0, 150.0, 14.0, 8.0, 1.0); break;  // high tom
        case 7:  // crash
        {
            b = makeBuffer (1.6);
            auto* d = b->getWritePointer (0);
            for (int i = 0; i < b->getNumSamples(); ++i)
                d[i] = (float) (brightNoise (rng, last) * std::exp (-(i / genRate) * 2.6) * 0.7);
            break;
        }
    }

    auto& p = pads[(size_t) pad];
    p.sample = b;
    p.sampleRate = genRate;
    p.file = juce::File();
    p.sampler.setSample (b, genRate, juce::File(), p.name);
}

DrumKit::DrumKit()
{
    scratchL.assign (16384, 0.0f);
    scratchR.assign (16384, 0.0f);
    kitL.assign (16384, 0.0f);
    kitR.assign (16384, 0.0f);

    for (int i = 0; i < numPads; ++i)
    {
        auto& p = pads[(size_t) i];
        p.name = padInfo[i].name;
        p.note = padInfo[i].note;
        setupSampler (i);
        generateDefault (i);
    }
    pads[3].chokeGroup = 1;   // closed hat cuts the open hat
    pads[4].chokeGroup = 1;
}

// What a drum pad's Simpler starts as: one-shot, played at its own pitch when its own key is hit.
void DrumKit::setupSampler (int pad)
{
    auto& sp = pads[(size_t) pad].sampler.params;
    sp.mode = Sampler::OneShot;
    sp.rootNote = pads[(size_t) pad].note;   // so the pad's own note plays the sample at its original pitch
    sp.attack = 0.0005f;
    sp.release = 0.08f;
    sp.volume = 0.7f;   // (headroom: kick + snare + hat together stay below full scale)
    sp.velocitySens = 0.85f;
}

void DrumKit::prepare (double outputRate)
{
    for (auto& p : pads)
        p.sampler.prepare (outputRate);
}

void DrumKit::setSample (int pad, std::shared_ptr<const juce::AudioBuffer<float>> buffer, double rate, const juce::File& file)
{
    auto& p = pads[(size_t) pad];
    p.sample = buffer;
    p.sampleRate = rate;
    p.file = file;
    p.sampler.setSample (std::move (buffer), rate, file, file.getFileNameWithoutExtension());
    p.sampler.params.start = 0.0f;   // a new sample plays from its start to its end
    p.sampler.params.end = 1.0f;
}

void DrumKit::resetPad (int pad)
{
    generateDefault (pad);
    pads[(size_t) pad].sampler.params.start = 0.0f;
    pads[(size_t) pad].sampler.params.end = 1.0f;
}

int DrumKit::padForNote (int note) const
{
    for (int i = 0; i < numPads; ++i)
        if (pads[(size_t) i].note == note)
            return i;
    return -1;
}

void DrumKit::noteOn (int note, float velocity)
{
    const int pad = padForNote (note);
    if (pad < 0 || pads[(size_t) pad].sample == nullptr)
        return;

    // a pad in a choke group cuts off the other pads of its group (closed hat chokes the open hat)
    const int group = pads[(size_t) pad].chokeGroup.load();
    if (group > 0)
        for (int i = 0; i < numPads; ++i)
            if (i != pad && pads[(size_t) i].chokeGroup.load() == group)
                pads[(size_t) i].sampler.choke();

    pads[(size_t) pad].sampler.noteOn (note, velocity);
}

void DrumKit::noteOff (int note)
{
    const int pad = padForNote (note);
    if (pad >= 0)
        pads[(size_t) pad].sampler.noteOff (note);   // (one-shot pads ignore it; a pad set to Classic releases)
}

void DrumKit::stopAll()
{
    for (auto& p : pads)
        p.sampler.choke();
}

void DrumKit::reset()
{
    for (auto& p : pads)
        p.sampler.reset();
}

bool DrumKit::isActive() const
{
    for (auto& p : pads)
        if (p.sampler.isActive())
            return true;
    return false;
}

void DrumKit::render (float* outL, float* outR, int numSamples)
{
    bool anySolo = false;
    for (auto& p : pads)
        anySolo = anySolo || p.solo.load();

    // A soft limiter on the sum of the pads: below 0.7 the sound is untouched, above it the peaks bend towards full scale instead of
    // clipping (kick, snare and hats hitting together stay clean).
    auto soft = [] (float x)
    {
        constexpr float knee = 0.7f;
        const float a = std::abs (x);
        return a <= knee ? x : std::copysign (knee + (1.0f - knee) * std::tanh ((a - knee) / (1.0f - knee)), x);
    };

    for (int done = 0; done < numSamples;)
    {
        const int n = juce::jmin ((int) scratchL.size(), numSamples - done);
        std::fill (kitL.begin(), kitL.begin() + n, 0.0f);
        std::fill (kitR.begin(), kitR.begin() + n, 0.0f);

        for (auto& p : pads)
        {
            if (! p.sampler.isActive())
                continue;

            std::fill (scratchL.begin(), scratchL.begin() + n, 0.0f);
            std::fill (scratchR.begin(), scratchR.begin() + n, 0.0f);
            p.sampler.render (scratchL.data(), scratchR.data(), n);   // (the pad keeps running when it is muted: unmuting is seamless)

            const bool audible = ! p.mute.load() && (! anySolo || p.solo.load());
            if (! audible)
                continue;

            // level and pan (the same law as a track's pan)
            const float angle = (juce::jlimit (-1.0f, 1.0f, p.pan.load()) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
            const float g = p.gain.load();
            const float gl = std::cos (angle) * juce::MathConstants<float>::sqrt2 * g, gr = std::sin (angle) * juce::MathConstants<float>::sqrt2 * g;
            for (int i = 0; i < n; ++i)
            {
                kitL[(size_t) i] += scratchL[(size_t) i] * gl;
                kitR[(size_t) i] += scratchR[(size_t) i] * gr;
            }
        }

        for (int i = 0; i < n; ++i)
        {
            outL[done + i] += soft (kitL[(size_t) i]);
            outR[done + i] += soft (kitR[(size_t) i]);
        }
        done += n;
    }
}

std::vector<DrumKit::PadState> DrumKit::captureState() const
{
    std::vector<PadState> out;
    for (auto& p : pads)
    {
        PadState s;
        s.gain = p.gain;
        s.pan = p.pan;
        s.mute = p.mute;
        s.solo = p.solo;
        s.chokeGroup = p.chokeGroup;
        s.sample = p.sample;
        s.sampleRate = p.sampleRate;
        s.file = p.file;
        s.sampler = p.sampler.captureState();
        out.push_back (std::move (s));
    }
    return out;
}

void DrumKit::applyState (const std::vector<PadState>& states)
{
    for (size_t i = 0; i < pads.size() && i < states.size(); ++i)
    {
        auto& p = pads[i];
        const auto& s = states[i];
        p.gain = s.gain;
        p.pan = s.pan;
        p.mute = s.mute;
        p.solo = s.solo;
        p.chokeGroup = s.chokeGroup;
        p.sample = s.sample;
        p.sampleRate = s.sampleRate;
        p.file = s.file;
        p.sampler.applyState (s.sampler);
    }
}
