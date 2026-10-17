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
        { "Open hat", 46 }, { "Low tom", 41 }, { "High tom", 45 }, { "Crash", 49 }
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

DrumKit::DrumKit()
{
    for (int i = 0; i < numPads; ++i)
    {
        pads[(size_t) i].name = padInfo[i].name;
        pads[(size_t) i].note = padInfo[i].note;
        generateDefault (i);
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
        default: // crash
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
}

void DrumKit::setSample (int pad, std::shared_ptr<const juce::AudioBuffer<float>> buffer, double rate, const juce::File& file)
{
    auto& p = pads[(size_t) pad];
    p.sample = std::move (buffer);
    p.sampleRate = rate;
    p.file = file;
}

void DrumKit::resetPad (int pad)
{
    generateDefault (pad);
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

    if (pad == 3)   // closed hat chokes the open hat
        for (auto& v : voices)
            if (v.active && v.pad == 4)
                v.fading = true;

    Voice* target = nullptr;
    for (auto& v : voices)
        if (! v.active) { target = &v; break; }
    if (target == nullptr)   // steal the oldest
    {
        target = &voices[0];
        for (auto& v : voices)
            if (v.age < target->age)
                target = &v;
    }

    *target = Voice();
    target->active = true;
    target->pad = pad;
    target->velocity = velocity;
    target->age = ++counter;
}

void DrumKit::stopAll()
{
    for (auto& v : voices)
        if (v.active)
            v.fading = true;
}

void DrumKit::reset()
{
    for (auto& v : voices)
        v = Voice();
}

bool DrumKit::isActive() const
{
    for (auto& v : voices)
        if (v.active)
            return true;
    return false;
}

void DrumKit::render (float* out, int numSamples)
{
    constexpr float fadeStep = 1.0f / 256.0f;   // ~6 ms fade-out when a voice is stopped or choked

    for (auto& v : voices)
    {
        if (! v.active)
            continue;

        const auto& pad = pads[(size_t) v.pad];
        const auto& buf = *pad.sample;
        const int len = buf.getNumSamples();
        const double ratio = pad.sampleRate / sampleRate;
        const float* left = buf.getReadPointer (0);
        const float* right = buf.getNumChannels() > 1 ? buf.getReadPointer (1) : left;
        const float gain = v.velocity * pad.gain * 0.9f;

        for (int i = 0; i < numSamples; ++i)
        {
            const int i0 = (int) v.position;
            if (i0 >= len - 1)
            {
                v.active = false;
                break;
            }

            const float frac = (float) (v.position - i0);
            const float s = 0.5f * ((left[i0] + frac * (left[i0 + 1] - left[i0]))
                                    + (right[i0] + frac * (right[i0 + 1] - right[i0])));
            out[i] += s * gain * v.fade;

            if (v.fading)
            {
                v.fade -= fadeStep;
                if (v.fade <= 0.0f)
                {
                    v.active = false;
                    break;
                }
            }
            v.position += ratio;
        }
    }
}
