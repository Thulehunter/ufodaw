#include "Fx.h"
#include <algorithm>
#include <cmath>

void FxParams::forEachFloat (const std::function<void (const char*, std::atomic<float>&)>& f)
{
    f ("eqLow", eqLow);          f ("eqMid", eqMid);          f ("eqMidFreq", eqMidFreq);   f ("eqHigh", eqHigh);
    f ("compThreshold", compThreshold); f ("compRatio", compRatio);   f ("compAttack", compAttack);
    f ("compRelease", compRelease);     f ("compMakeup", compMakeup);
    f ("delayTime", delayTime);  f ("delayFeedback", delayFeedback);  f ("delayMix", delayMix);
    f ("reverbSize", reverbSize); f ("reverbDamping", reverbDamping); f ("reverbMix", reverbMix);
}

void FxParams::forEachBool (const std::function<void (const char*, std::atomic<bool>&)>& f)
{
    f ("eqOn", eqOn);  f ("compOn", compOn);  f ("delayOn", delayOn);  f ("reverbOn", reverbOn);  f ("mono", mono);
}

std::atomic<float>* FxParams::findFloat (const juce::String& name)
{
    std::atomic<float>* found = nullptr;
    forEachFloat ([&] (const char* n, std::atomic<float>& v) { if (name == n) found = &v; });
    return found;
}

const std::vector<FxParamSpec>& fxParamSpecs()
{
    static const std::vector<FxParamSpec> specs = {
        { "eqLow",         "EQ low",             -18.0f,   18.0f },
        { "eqMid",         "EQ mid",             -18.0f,   18.0f },
        { "eqMidFreq",     "EQ mid frequency",   200.0f,   5000.0f },
        { "eqHigh",        "EQ high",            -18.0f,   18.0f },
        { "compThreshold", "Compressor threshold", -60.0f, 0.0f },
        { "compRatio",     "Compressor ratio",   1.0f,     20.0f },
        { "compAttack",    "Compressor attack",  1.0f,     200.0f },
        { "compRelease",   "Compressor release", 10.0f,    1000.0f },
        { "compMakeup",    "Compressor makeup",  0.0f,     24.0f },
        { "delayTime",     "Delay time",         20.0f,    1500.0f },
        { "delayFeedback", "Delay feedback",     0.0f,     0.9f },
        { "delayMix",      "Delay mix",          0.0f,     1.0f },
        { "reverbSize",    "Reverb size",        0.0f,     1.0f },
        { "reverbDamping", "Reverb damping",     0.0f,     1.0f },
        { "reverbMix",     "Reverb mix",         0.0f,     1.0f },
    };
    return specs;
}

//==============================================================================
void TrackFx::prepare (double rate)
{
    sampleRate = rate;
    delayL.assign ((size_t) (rate * 2.0) + 2, 0.0f);   // up to 2 s of delay
    delayR.assign (delayL.size(), 0.0f);
    reverb.setSampleRate (rate);
    reset();
}

void TrackFx::reset()
{
    for (auto& band : eq)
        for (auto& f : band)
            f.reset();

    compEnv = 0.0f;
    std::fill (delayL.begin(), delayL.end(), 0.0f);
    std::fill (delayR.begin(), delayR.end(), 0.0f);
    delayPos = 0;
    reverb.reset();
}

void TrackFx::process (float* l, float* r, int n, const FxParams& p, const float* keyL, const float* keyR)
{
    if (p.eqOn)     processEq (l, r, n, p);
    if (p.compOn)   processCompressor (l, r, n, p, keyL, keyR);
    if (p.delayOn)  processDelay (l, r, n, p);
    if (p.reverbOn) processReverb (l, r, n, p);
}

void TrackFx::processEq (float* l, float* r, int n, const FxParams& p)
{
    const float now[4] = { p.eqLow, p.eqMid, p.eqMidFreq, p.eqHigh };
    if (! std::equal (std::begin (now), std::end (now), std::begin (lastEq)))
    {
        std::copy (std::begin (now), std::end (now), std::begin (lastEq));

        const auto low  = juce::IIRCoefficients::makeLowShelf (sampleRate, 120.0, 0.707, juce::Decibels::decibelsToGain (now[0]));
        const auto mid  = juce::IIRCoefficients::makePeakFilter (sampleRate, juce::jlimit (60.0f, 12000.0f, now[2]), 1.0,
                                                                 juce::Decibels::decibelsToGain (now[1]));
        const auto high = juce::IIRCoefficients::makeHighShelf (sampleRate, 6000.0, 0.707, juce::Decibels::decibelsToGain (now[3]));
        for (int ch = 0; ch < 2; ++ch)
        {
            eq[0][ch].setCoefficients (low);
            eq[1][ch].setCoefficients (mid);
            eq[2][ch].setCoefficients (high);
        }
    }

    for (auto& band : eq)
    {
        band[0].processSamples (l, n);
        band[1].processSamples (r, n);
    }
}

void TrackFx::processCompressor (float* l, float* r, int n, const FxParams& p, const float* keyL, const float* keyR)
{
    const float threshold = p.compThreshold;
    const float slope = 1.0f - 1.0f / juce::jmax (1.0f, p.compRatio.load());
    const float makeup = juce::Decibels::decibelsToGain (p.compMakeup.load());
    const float attack = std::exp (-1.0f / (0.001f * juce::jmax (0.1f, p.compAttack.load()) * (float) sampleRate));
    const float release = std::exp (-1.0f / (0.001f * juce::jmax (1.0f, p.compRelease.load()) * (float) sampleRate));

    for (int i = 0; i < n; ++i)
    {
        const float level = keyL != nullptr ? juce::jmax (std::abs (keyL[i]), std::abs (keyR != nullptr ? keyR[i] : keyL[i]))   // sidechain: the other track decides
                                            : juce::jmax (std::abs (l[i]), std::abs (r[i]));                                    // linked stereo detector
        const float coef = level > compEnv ? attack : release;
        compEnv = coef * compEnv + (1.0f - coef) * level;

        const float db = 20.0f * std::log10 (juce::jmax (compEnv, 1.0e-6f));
        const float over = db - threshold;
        const float gain = over > 0.0f ? juce::Decibels::decibelsToGain (-over * slope) : 1.0f;

        l[i] *= gain * makeup;
        r[i] *= gain * makeup;
    }
}

void TrackFx::processDelay (float* l, float* r, int n, const FxParams& p)
{
    const int size = (int) delayL.size();
    const int delaySamples = juce::jlimit (1, size - 1, (int) (p.delayTime.load() * 0.001 * sampleRate));
    const float feedback = juce::jlimit (0.0f, 0.95f, p.delayFeedback.load());
    const float mix = p.delayMix;

    for (int i = 0; i < n; ++i)
    {
        int rp = delayPos - delaySamples;
        if (rp < 0) rp += size;

        const float dl = delayL[(size_t) rp], dr = delayR[(size_t) rp];
        delayL[(size_t) delayPos] = l[i] + dl * feedback;
        delayR[(size_t) delayPos] = r[i] + dr * feedback;
        l[i] += dl * mix;
        r[i] += dr * mix;

        if (++delayPos >= size)
            delayPos = 0;
    }
}

void TrackFx::processReverb (float* l, float* r, int n, const FxParams& p)
{
    const float now[3] = { p.reverbSize, p.reverbDamping, p.reverbMix };
    if (! std::equal (std::begin (now), std::end (now), std::begin (lastReverb)))
    {
        std::copy (std::begin (now), std::end (now), std::begin (lastReverb));

        juce::Reverb::Parameters params;
        params.roomSize = now[0];
        params.damping = now[1];
        params.wetLevel = now[2] * 0.2f;   // Reverb scales this by 3
        params.dryLevel = 0.5f;            // ...and this by 2, so 0.5 leaves the dry signal untouched
        params.width = 1.0f;
        params.freezeMode = 0.0f;
        reverb.setParameters (params);
    }
    reverb.processStereo (l, r, n);
}
