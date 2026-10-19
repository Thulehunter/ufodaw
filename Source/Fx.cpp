#include "Fx.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <array>

void FxParams::forEachFloat (const std::function<void (const char*, std::atomic<float>&)>& f)
{
    f ("eqLow", eqLow);          f ("eqMid", eqMid);          f ("eqMidFreq", eqMidFreq);   f ("eqHigh", eqHigh);
    f ("compThreshold", compThreshold); f ("compRatio", compRatio);   f ("compAttack", compAttack);
    f ("compRelease", compRelease);     f ("compMakeup", compMakeup);
    f ("delayTime", delayTime);  f ("delayFeedback", delayFeedback);  f ("delayMix", delayMix);
    f ("reverbSize", reverbSize); f ("reverbDamping", reverbDamping); f ("reverbMix", reverbMix);
    f ("eqHpFreq", eqHpFreq);    f ("eqLpFreq", eqLpFreq);    f ("eqLowFreq", eqLowFreq);   f ("eqHighFreq", eqHighFreq);
    f ("eqMidQ", eqMidQ);        f ("eqMid2", eqMid2);        f ("eqMid2Freq", eqMid2Freq); f ("eqMid2Q", eqMid2Q);
    f ("compKnee", compKnee);    f ("compMix", compMix);      f ("compRms", compRms);       f ("compScHpf", compScHpf);
    f ("delayRatio", delayRatio); f ("delayLowCut", delayLowCut); f ("delayHighCut", delayHighCut); f ("delayMod", delayMod);
    f ("reverbDecay", reverbDecay); f ("reverbPreDelay", reverbPreDelay); f ("reverbLowCut", reverbLowCut);
    f ("reverbWidth", reverbWidth); f ("reverbMod", reverbMod);
    f ("gateThreshold", gateThreshold); f ("gateRange", gateRange); f ("gateAttack", gateAttack); f ("gateHold", gateHold); f ("gateRelease", gateRelease);
    f ("deEssFreq", deEssFreq); f ("deEssThreshold", deEssThreshold); f ("deEssAmount", deEssAmount);
    f ("satDrive", satDrive); f ("satType", satType); f ("satMix", satMix); f ("satOutput", satOutput);
    f ("mbLowFreq", mbLowFreq); f ("mbHighFreq", mbHighFreq); f ("mbLowThr", mbLowThr); f ("mbMidThr", mbMidThr); f ("mbHighThr", mbHighThr);
    f ("mbRatio", mbRatio); f ("mbMakeup", mbMakeup);
    f ("chorusRate", chorusRate); f ("chorusDepth", chorusDepth); f ("chorusDelay", chorusDelay); f ("chorusFeedback", chorusFeedback); f ("chorusMix", chorusMix);
    f ("limGain", limGain); f ("limCeiling", limCeiling); f ("limRelease", limRelease);
    f ("gateHyst", gateHyst); f ("gateScHpf", gateScHpf); f ("gateScLpf", gateScLpf);
    f ("deEssQ", deEssQ); f ("satBias", satBias); f ("satTone", satTone);
    f ("mbLowGain", mbLowGain); f ("mbMidGain", mbMidGain); f ("mbHighGain", mbHighGain); f ("mbAttack", mbAttack); f ("mbRelease", mbRelease);
    f ("chorusVoices", chorusVoices); f ("chorusWidth", chorusWidth); f ("chorusTone", chorusTone);
    f ("limLookahead", limLookahead);
}

void FxParams::forEachBool (const std::function<void (const char*, std::atomic<bool>&)>& f)
{
    f ("eqOn", eqOn);  f ("compOn", compOn);  f ("delayOn", delayOn);  f ("reverbOn", reverbOn);  f ("mono", mono);  f ("compAuto", compAuto);  f ("delayPing", delayPing);
    f ("gateOn", gateOn); f ("deEssOn", deEssOn); f ("satOn", satOn); f ("mbOn", mbOn); f ("chorusOn", chorusOn); f ("limOn", limOn);
    f ("deEssWide", deEssWide); f ("deEssListen", deEssListen); f ("satOversample", satOversample); f ("limTruePeak", limTruePeak);
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
        { "eqHpFreq",      "EQ high-pass",       20.0f,    2000.0f },
        { "eqLpFreq",      "EQ low-pass",        1000.0f,  20000.0f },
        { "eqLowFreq",     "EQ low frequency",   20.0f,    1000.0f },
        { "eqHighFreq",    "EQ high frequency",  1000.0f,  20000.0f },
        { "eqMidQ",        "EQ mid Q",           0.2f,     10.0f },
        { "eqMid2",        "EQ mid 2",           -18.0f,   18.0f },
        { "eqMid2Freq",    "EQ mid 2 frequency", 60.0f,    12000.0f },
        { "eqMid2Q",       "EQ mid 2 Q",         0.2f,     10.0f },
        { "compKnee",      "Compressor knee",    0.0f,     24.0f },
        { "compMix",       "Compressor mix",     0.0f,     1.0f },
        { "compRms",       "Compressor RMS/peak", 0.0f,    1.0f },
        { "compScHpf",     "Compressor sidechain filter", 20.0f, 500.0f },
        { "delayRatio",    "Delay right/left time", 0.25f,  2.0f },
        { "delayLowCut",   "Delay low cut",      20.0f,    2000.0f },
        { "delayHighCut",  "Delay high cut",     1000.0f,  20000.0f },
        { "delayMod",      "Delay wobble",       0.0f,     1.0f },
        { "reverbDecay",   "Reverb decay",       0.2f,     10.0f },
        { "reverbPreDelay", "Reverb pre-delay",  0.0f,     200.0f },
        { "reverbLowCut",  "Reverb low cut",     20.0f,    1000.0f },
        { "reverbWidth",   "Reverb width",       0.0f,     1.0f },
        { "reverbMod",     "Reverb modulation",  0.0f,     1.0f },
        { "gateThreshold", "Gate threshold",     -80.0f,   0.0f },
        { "gateRange",     "Gate range",         0.0f,     80.0f },
        { "gateAttack",    "Gate attack",        0.1f,     50.0f },
        { "gateHold",      "Gate hold",          0.0f,     500.0f },
        { "gateRelease",   "Gate release",       10.0f,    1000.0f },
        { "deEssFreq",     "De-esser frequency", 2000.0f,  14000.0f },
        { "deEssThreshold","De-esser threshold", -60.0f,   0.0f },
        { "deEssAmount",   "De-esser amount",    0.0f,     20.0f },
        { "satDrive",      "Saturation drive",   0.0f,     30.0f },
        { "satType",       "Saturation type",    0.0f,     4.0f },
        { "satMix",        "Saturation mix",     0.0f,     1.0f },
        { "satOutput",     "Saturation output",  -12.0f,   12.0f },
        { "mbLowFreq",     "Multiband low split", 40.0f,   1000.0f },
        { "mbHighFreq",    "Multiband high split", 300.0f, 12000.0f },
        { "mbLowThr",      "Multiband low threshold", -60.0f, 0.0f },
        { "mbMidThr",      "Multiband mid threshold", -60.0f, 0.0f },
        { "mbHighThr",     "Multiband high threshold", -60.0f, 0.0f },
        { "mbRatio",       "Multiband ratio",    1.0f,     10.0f },
        { "mbMakeup",      "Multiband makeup",   0.0f,     12.0f },
        { "chorusRate",    "Chorus rate",        0.05f,    5.0f },
        { "chorusDepth",   "Chorus depth",       0.0f,     1.0f },
        { "chorusDelay",   "Chorus delay",       0.5f,     40.0f },
        { "chorusFeedback","Chorus feedback",    0.0f,     0.9f },
        { "chorusMix",     "Chorus mix",         0.0f,     1.0f },
        { "limGain",       "Limiter gain",       0.0f,     18.0f },
        { "limCeiling",    "Limiter ceiling",    -12.0f,   0.0f },
        { "limRelease",    "Limiter release",    5.0f,     500.0f },
        { "gateHyst",      "Gate hysteresis",    0.0f,     12.0f },
        { "gateScHpf",     "Gate sidechain low cut", 20.0f, 2000.0f },
        { "gateScLpf",     "Gate sidechain high cut", 1000.0f, 20000.0f },
        { "deEssQ",        "De-esser width",     0.3f,     6.0f },
        { "satBias",       "Saturation bias",    0.0f,     1.0f },
        { "satTone",       "Saturation tone",    -1.0f,    1.0f },
        { "mbLowGain",     "Multiband low gain", -12.0f,   12.0f },
        { "mbMidGain",     "Multiband mid gain", -12.0f,   12.0f },
        { "mbHighGain",    "Multiband high gain", -12.0f,  12.0f },
        { "mbAttack",      "Multiband attack",   0.5f,     100.0f },
        { "mbRelease",     "Multiband release",  5.0f,     1000.0f },
        { "chorusVoices",  "Chorus voices",      1.0f,     4.0f },
        { "chorusWidth",   "Chorus width",       0.0f,     1.0f },
        { "chorusTone",    "Chorus tone",        1000.0f,  20000.0f },
        { "limLookahead",  "Limiter lookahead",  0.0f,     10.0f },
    };
    return specs;
}

//==============================================================================
// EQ design (RBJ audio-EQ cookbook), in double precision
namespace
{
    constexpr double pi = 3.14159265358979323846;

    EqCoef normalised (double b0, double b1, double b2, double a0, double a1, double a2)
    {
        EqCoef c;
        c.b0 = b0 / a0;  c.b1 = b1 / a0;  c.b2 = b2 / a0;  c.a1 = a1 / a0;  c.a2 = a2 / a0;
        c.active = true;
        return c;
    }

    double limitedFreq (double f, double rate) { return juce::jlimit (10.0, rate * 0.45, f); }

    EqCoef lowpass (double rate, double f, double q)
    {
        const double w = 2.0 * pi * limitedFreq (f, rate) / rate, cs = std::cos (w), al = std::sin (w) / (2.0 * q);
        return normalised ((1 - cs) / 2, 1 - cs, (1 - cs) / 2, 1 + al, -2 * cs, 1 - al);
    }

    EqCoef peak (double rate, double f, double q, double db)
    {
        const double A = std::pow (10.0, db / 40.0), w = 2.0 * pi * limitedFreq (f, rate) / rate, cs = std::cos (w), al = std::sin (w) / (2.0 * q);
        return normalised (1 + al * A, -2 * cs, 1 - al * A, 1 + al / A, -2 * cs, 1 - al / A);
    }

    EqCoef shelf (bool high, double rate, double f, double db)
    {
        const double A = std::pow (10.0, db / 40.0), w = 2.0 * pi * limitedFreq (f, rate) / rate, cs = std::cos (w);
        const double al = std::sin (w) / (2.0 * 0.707), t = 2.0 * std::sqrt (A) * al;
        if (high)
            return normalised (A * ((A + 1) + (A - 1) * cs + t), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - t),
                               (A + 1) - (A - 1) * cs + t, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - t);
        return normalised (A * ((A + 1) - (A - 1) * cs + t), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - t),
                           (A + 1) + (A - 1) * cs + t, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - t);
    }
}

EqCoef highpassCoef (double rate, double f, double q)
{
    const double w = 2.0 * pi * limitedFreq (f, rate) / rate, cs = std::cos (w), al = std::sin (w) / (2.0 * q);
    return normalised ((1 + cs) / 2, -(1 + cs), (1 + cs) / 2, 1 + al, -2 * cs, 1 - al);
}

void eqDesign (const FxParams& p, double rate, EqCoef (&out)[eqNumSections])
{
    for (auto& c : out)
        c = EqCoef();

    const double qButter[2] = { 0.5412, 1.3066 };   // two sections make a 4th order Butterworth: 24 dB per octave
    if (p.eqHpFreq.load() > 21.0f)
        for (int k = 0; k < 2; ++k)
            out[k] = highpassCoef (rate, p.eqHpFreq.load(), qButter[k]);
    if (p.eqLpFreq.load() < 19900.0f)
        for (int k = 0; k < 2; ++k)
            out[6 + k] = lowpass (rate, p.eqLpFreq.load(), qButter[k]);

    if (std::abs (p.eqLow.load()) > 0.01f)   out[2] = shelf (false, rate, p.eqLowFreq.load(), p.eqLow.load());
    if (std::abs (p.eqMid.load()) > 0.01f)   out[3] = peak (rate, p.eqMidFreq.load(), juce::jmax (0.1f, p.eqMidQ.load()), p.eqMid.load());
    if (std::abs (p.eqMid2.load()) > 0.01f)  out[4] = peak (rate, p.eqMid2Freq.load(), juce::jmax (0.1f, p.eqMid2Q.load()), p.eqMid2.load());
    if (std::abs (p.eqHigh.load()) > 0.01f)  out[5] = shelf (true, rate, p.eqHighFreq.load(), p.eqHigh.load());
}

double eqResponseDb (const EqCoef (&c)[eqNumSections], double rate, double f)
{
    const double w = 2.0 * pi * f / rate;
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
    double db = 0.0;
    for (auto& s : c)
    {
        if (! s.active)
            continue;
        const auto num = s.b0 + s.b1 * z1 + s.b2 * z2;
        const auto den = 1.0 + s.a1 * z1 + s.a2 * z2;
        db += 20.0 * std::log10 (std::max (1.0e-9, std::abs (num)) / std::max (1.0e-9, std::abs (den)));
    }
    return db;
}

//==============================================================================
void TrackFx::prepare (double rate)
{
    sampleRate = rate;
    delayL.assign ((size_t) (rate * 2.0) + 2, 0.0f);   // up to 2 s of delay
    delayR.assign (delayL.size(), 0.0f);
    const float bases[fdnLines] = { 1031, 1249, 1481, 1709, 1951, 2221, 2503, 2797 };   // (primes: no echoes lining up)
    for (int k = 0; k < fdnLines; ++k)
    {
        fdnBase[k] = bases[k];
        fdn[k].assign ((size_t) (bases[k] * 1.6f * (float) rate / 48000.0f) + 64, 0.0f);
    }
    chL.assign ((size_t) (rate * 0.1) + 8, 0.0f);
    chR.assign (chL.size(), 0.0f);
    preL.assign ((size_t) (rate * 0.2) + 2, 0.0f);
    preR.assign (preL.size(), 0.0f);
    reset();
}

void TrackFx::reset()
{
    for (auto& band : eqState)
        for (auto& st : band)
            st = Biquad();
    eqKnown = false;
    for (auto& st : scState)
        st = Biquad();
    lastScHpf = -1.0f;

    compGr = 0.0f;
    rmsSq = 0.0f;
    gateEnv = 0.0f; gateGain = 1.0f; gateHoldLeft = 0;
    for (auto& st : deEssState) st = Biquad();
    lastDeEssFreq = -1.0f; deEssEnv = 0.0f;
    for (auto& ch : mbState) for (auto& st : ch) st = Biquad();
    lastMbFreq[0] = lastMbFreq[1] = -1.0f;
    for (auto& e : mbEnv) e = 0.0f;
    std::fill (chL.begin(), chL.end(), 0.0f);
    std::fill (chR.begin(), chR.end(), 0.0f);
    chPos = 0; chPhase = 0.0f; chToneL = chToneR = 0.0f;
    limGainNow = 1.0f; limLastLook = -1; limSumMin = 0.0f; limPos = 0;
    for (auto& o : satOver) o = Over4();
    satLp[0] = satLp[1] = satTilt[0] = satTilt[1] = 0.0f;
    gateOpen = false; for (auto& st : gateState) st = Biquad();
    lastGateHp = lastGateLp = -1.0f;
    std::fill (delayL.begin(), delayL.end(), 0.0f);
    std::fill (delayR.begin(), delayR.end(), 0.0f);
    delayPos = 0;
    dlpL = dlpR = dhpL = dhpR = 0.0f;
    smoothTimeL = smoothTimeR = -1.0f;
    for (auto& line : fdn)
        std::fill (line.begin(), line.end(), 0.0f);
    for (auto& v : fdnLow) v = 0.0f;
    for (auto& v : fdnPos) v = 0;
    std::fill (preL.begin(), preL.end(), 0.0f);
    std::fill (preR.begin(), preR.end(), 0.0f);
    prePos = 0;
    revHpL = revHpR = 0.0f;
}

void TrackFx::process (float* l, float* r, int n, const FxParams& p, const float* keyL, const float* keyR)
{
    if (p.gateOn)   processGate (l, r, n, p);
    if (p.eqOn)     processEq (l, r, n, p);
    if (p.analyse.load (std::memory_order_relaxed))
        tap.write (l, r, n);
    if (p.compOn)   processCompressor (l, r, n, p, keyL, keyR);
    else            p.compGr.store (0.0f);
    if (p.deEssOn)  processDeEsser (l, r, n, p);
    if (p.satOn)    processSaturation (l, r, n, p);
    if (p.mbOn)     processMultiband (l, r, n, p);
    if (p.chorusOn) processChorus (l, r, n, p);
    if (p.delayOn)  processDelay (l, r, n, p);
    if (p.reverbOn) processReverb (l, r, n, p);
    if (p.limOn)    processLimiter (l, r, n, p);

    // what the chain delays the sound by now (the track is compensated for it): the limiter's lookahead, the oversampling of the saturation
    int latency = 0;
    if (p.satOn && p.satOversample)
        latency += 12;
    if (p.limOn)
        latency += juce::jlimit (0, 480, (int) std::lround (p.limLookahead.load() * 0.001 * sampleRate));
    p.latencySamples.store (latency, std::memory_order_relaxed);
}

void TrackFx::processEq (float* l, float* r, int n, const FxParams& p)
{
    const float now[12] = { p.eqLow, p.eqMid, p.eqMidFreq, p.eqHigh, p.eqHpFreq, p.eqLpFreq, p.eqLowFreq, p.eqHighFreq, p.eqMidQ, p.eqMid2, p.eqMid2Freq, p.eqMid2Q };
    if (! eqKnown || ! std::equal (std::begin (now), std::end (now), std::begin (lastEq)))
    {
        std::copy (std::begin (now), std::end (now), std::begin (lastEq));
        eqKnown = true;
        eqDesign (p, sampleRate, eqCoef);
    }

    for (int s = 0; s < eqNumSections; ++s)
    {
        if (! eqCoef[s].active)
            continue;
        const auto& c = eqCoef[s];
        auto& sl = eqState[s][0];
        auto& sr = eqState[s][1];
        for (int i = 0; i < n; ++i)
        {
            l[i] = run (c, sl, l[i]);
            r[i] = run (c, sr, r[i]);
        }
    }
}

// Feed-forward compressor: the detector (peak, RMS or a blend, with an optional high-pass so bass doesn't pump it) in dB, a static curve with a soft
// knee, the gain reduction smoothed with attack and release, then makeup (manual or automatic) and a dry/wet mix for parallel compression.
void TrackFx::processCompressor (float* l, float* r, int n, const FxParams& p, const float* keyL, const float* keyR)
{
    const float threshold = p.compThreshold;
    const float ratio = juce::jmax (1.0f, p.compRatio.load());
    const float slope = 1.0f - 1.0f / ratio;
    const float knee = juce::jmax (0.0f, p.compKnee.load());
    const float makeupDb = p.compMakeup.load() + (p.compAuto.load() ? -threshold * slope * 0.5f : 0.0f);
    const float makeup = juce::Decibels::decibelsToGain (makeupDb);
    const float attack = std::exp (-1.0f / (0.001f * juce::jmax (0.05f, p.compAttack.load()) * (float) sampleRate));
    const float release = std::exp (-1.0f / (0.001f * juce::jmax (1.0f, p.compRelease.load()) * (float) sampleRate));
    const float mix = juce::jlimit (0.0f, 1.0f, p.compMix.load());
    const float rmsBlend = juce::jlimit (0.0f, 1.0f, p.compRms.load());
    const float rmsCoef = 1.0f - std::exp (-1.0f / (0.01f * (float) sampleRate));   // 10 ms window

    const float hpf = p.compScHpf;
    const bool useHpf = hpf > 21.0f;
    if (useHpf && hpf != lastScHpf)
    {
        lastScHpf = hpf;
        scCoef = highpassCoef (sampleRate, hpf, 0.707);
    }

    float maxGr = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        float kl = keyL != nullptr ? keyL[i] : l[i];                                   // sidechain: the other track decides
        float kr = keyL != nullptr ? (keyR != nullptr ? keyR[i] : keyL[i]) : r[i];
        if (useHpf)
        {
            kl = run (scCoef, scState[0], kl);
            kr = run (scCoef, scState[1], kr);
        }

        const float peak = juce::jmax (std::abs (kl), std::abs (kr));
        rmsSq += (0.5f * (kl * kl + kr * kr) - rmsSq) * rmsCoef;
        const float level = peak * (1.0f - rmsBlend) + std::sqrt (rmsSq) * 1.4142f * rmsBlend;   // (a sine reads the same in both)
        const float db = 20.0f * std::log10 (juce::jmax (level, 1.0e-6f));

        const float over = db - threshold;
        float target = 0.0f;                                  // wanted gain reduction in dB
        if (slope > 0.0f)
        {
            if (knee > 0.01f && 2.0f * std::abs (over) <= knee)
            {
                const float x = over + knee * 0.5f;
                target = slope * x * x / (2.0f * knee);       // soft knee: eases in over knee dB around the threshold
            }
            else if (over > 0.0f)
                target = over * slope;
        }

        const float coef = target > compGr ? attack : release;
        compGr = coef * compGr + (1.0f - coef) * target;
        maxGr = juce::jmax (maxGr, compGr);

        const float gain = juce::Decibels::decibelsToGain (-compGr) * makeup;
        l[i] = l[i] * (1.0f - mix) + l[i] * gain * mix;
        r[i] = r[i] * (1.0f - mix) + r[i] * gain * mix;
    }
    p.compGr.store (maxGr);
}

// Delay: separate left and right times (ratio), a low cut and a high cut inside the feedback (every repeat gets darker and thinner, like a tape or a
// good echo unit), ping-pong, and a slow wobble of the time. Time changes glide instead of clicking.
void TrackFx::processDelay (float* l, float* r, int n, const FxParams& p)
{
    const int size = (int) delayL.size();
    const float timeL = juce::jlimit (1.0f, (float) size - 8.0f, p.delayTime.load() * 0.001f * (float) sampleRate);
    const float timeR = juce::jlimit (1.0f, (float) size - 8.0f, timeL * juce::jlimit (0.25f, 2.0f, p.delayRatio.load()));
    const float feedback = juce::jlimit (0.0f, 0.95f, p.delayFeedback.load());
    const float mix = p.delayMix;
    const bool ping = p.delayPing;
    const float wobble = juce::jlimit (0.0f, 1.0f, p.delayMod.load()) * 0.003f * (float) sampleRate;   // up to 3 ms
    const float lfoStep = 2.0f * 3.14159265f * 0.7f / (float) sampleRate;
    const float aLow = 1.0f - std::exp (-2.0f * 3.14159265f * juce::jlimit (20.0f, 2000.0f, p.delayLowCut.load()) / (float) sampleRate);
    const float aHigh = 1.0f - std::exp (-2.0f * 3.14159265f * juce::jlimit (1000.0f, 20000.0f, p.delayHighCut.load()) / (float) sampleRate);
    const bool lowOn = p.delayLowCut.load() > 21.0f;
    const bool highOn = p.delayHighCut.load() < 19900.0f;

    if (smoothTimeL < 0.0f) { smoothTimeL = timeL; smoothTimeR = timeR; }

    auto read = [&] (const std::vector<float>& buf, float delaySamples)
    {
        float rp = (float) delayPos - delaySamples;
        while (rp < 0.0f) rp += (float) size;
        const int i0 = (int) rp;
        const float frac = rp - (float) i0;
        const int i1 = i0 + 1 >= size ? 0 : i0 + 1;
        return buf[(size_t) i0] * (1.0f - frac) + buf[(size_t) i1] * frac;
    };

    for (int i = 0; i < n; ++i)
    {
        smoothTimeL += (timeL - smoothTimeL) * 0.0008f;
        smoothTimeR += (timeR - smoothTimeR) * 0.0008f;
        delayLfo += lfoStep;
        if (delayLfo > 6.2831853f) delayLfo -= 6.2831853f;
        const float w = wobble * std::sin (delayLfo);

        const float dl = read (delayL, juce::jlimit (1.0f, (float) size - 4.0f, smoothTimeL + w));
        const float dr = read (delayR, juce::jlimit (1.0f, (float) size - 4.0f, smoothTimeR - w));

        float fl = dl * feedback, fr = dr * feedback;
        if (highOn) { dlpL += aHigh * (fl - dlpL); dlpR += aHigh * (fr - dlpR); fl = dlpL; fr = dlpR; }
        if (lowOn)  { dhpL += aLow * (fl - dhpL);  dhpR += aLow * (fr - dhpR);  fl -= dhpL; fr -= dhpR; }

        if (ping)
        {
            delayL[(size_t) delayPos] = 0.5f * (l[i] + r[i]) + fr;   // what comes back on the right is fed to the left: the echoes bounce
            delayR[(size_t) delayPos] = fl;
        }
        else
        {
            delayL[(size_t) delayPos] = l[i] + fl;
            delayR[(size_t) delayPos] = r[i] + fr;
        }
        l[i] += dl * mix;
        r[i] += dr * mix;

        if (++delayPos >= size)
            delayPos = 0;
    }
}

// Reverb: 8 delay lines whose outputs are mixed back into each other by a Hadamard matrix (a feedback delay network). The gain of every line is set from the
// decay time (RT60), the damping filter makes the high frequencies die sooner, and the lines are modulated slowly so the tail doesn't ring.
void TrackFx::processReverb (float* l, float* r, int n, const FxParams& p)
{
    const float rate = (float) sampleRate;
    const float scale = 0.6f + juce::jlimit (0.0f, 1.0f, p.reverbSize.load()) * 1.0f;
    const float decay = juce::jmax (0.1f, p.reverbDecay.load());
    const float damp = juce::jlimit (0.0f, 1.0f, p.reverbDamping.load());
    const float dampCoef = 1.0f - damp * 0.92f;                                   // 1 = bright, near 0 = dark
    const float mix = p.reverbMix;
    const float width = juce::jlimit (0.0f, 1.0f, p.reverbWidth.load());
    const float depth = juce::jlimit (0.0f, 1.0f, p.reverbMod.load()) * 7.0f * rate / 48000.0f;
    const int preSize = (int) preL.size();
    const int preSamples = juce::jlimit (0, preSize - 1, (int) (p.reverbPreDelay.load() * 0.001f * rate));
    const float aLow = 1.0f - std::exp (-2.0f * 3.14159265f * juce::jlimit (20.0f, 1000.0f, p.reverbLowCut.load()) / rate);
    const bool lowOn = p.reverbLowCut.load() > 21.0f;

    float len[fdnLines], gain[fdnLines], modOff[fdnLines] = {};
    for (int k = 0; k < fdnLines; ++k)
    {
        len[k] = fdnBase[k] * scale * rate / 48000.0f;
        gain[k] = std::pow (10.0f, -3.0f * len[k] / (decay * rate));
    }

    int done = 0;
    while (done < n)
    {
        const int chunk = juce::jmin (32, n - done);
        revLfo += 2.0f * 3.14159265f * 0.45f * (float) chunk / rate;
        if (revLfo > 6.2831853f) revLfo -= 6.2831853f;
        for (int k = 0; k < fdnLines; ++k)
            modOff[k] = depth * std::sin (revLfo + (float) k * 0.8f);

        for (int i = done; i < done + chunk; ++i)
        {
            preL[(size_t) prePos] = l[i];
            preR[(size_t) prePos] = r[i];
            int rp = prePos - preSamples;
            if (rp < 0) rp += preSize;
            const float inL = preL[(size_t) rp], inR = preR[(size_t) rp];
            if (++prePos >= preSize) prePos = 0;

            float y[fdnLines];
            for (int k = 0; k < fdnLines; ++k)
            {
                auto& buf = fdn[k];
                const int bs = (int) buf.size();
                float rpos = (float) fdnPos[k] - (len[k] + modOff[k]);
                while (rpos < 0.0f) rpos += (float) bs;
                const int i0 = (int) rpos;
                const float frac = rpos - (float) i0;
                const float x = buf[(size_t) i0] * (1.0f - frac) + buf[(size_t) (i0 + 1 >= bs ? 0 : i0 + 1)] * frac;
                fdnLow[k] += dampCoef * (x - fdnLow[k]);
                y[k] = fdnLow[k];
            }

            // output: even lines to the left, odd to the right
            float wl = (y[0] - y[2] + y[4] - y[6]) * 0.5f, wr = (y[1] - y[3] + y[5] - y[7]) * 0.5f;

            // Hadamard mixing (fast Walsh-Hadamard transform, scaled to keep the energy)
            float v[fdnLines];
            for (int k = 0; k < fdnLines; ++k) v[k] = y[k] * gain[k];
            for (int step = 1; step < fdnLines; step *= 2)
                for (int a = 0; a < fdnLines; a += step * 2)
                    for (int b = a; b < a + step; ++b)
                    {
                        const float s = v[b], d = v[b + step];
                        v[b] = s + d;
                        v[b + step] = s - d;
                    }
            for (int k = 0; k < fdnLines; ++k)
            {
                fdn[k][(size_t) fdnPos[k]] = v[k] * 0.35355339f + ((k & 1) ? inR : inL) * 0.3f;
                if (++fdnPos[k] >= (int) fdn[k].size()) fdnPos[k] = 0;
            }

            const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * width;
            wl = mid + side;
            wr = mid - side;
            if (lowOn)
            {
                revHpL += aLow * (wl - revHpL);  wl -= revHpL;
                revHpR += aLow * (wr - revHpR);  wr -= revHpR;
            }
            l[i] += wl * mix * 4.0f;
            r[i] += wr * mix * 4.0f;
        }
        done += chunk;
    }
}

// ---- more effects

namespace
{
    // the decimating / interpolating filter of the 4x oversampler: windowed sinc (Blackman), 49 taps, cut-off at the original Nyquist, gain 1
    const float* overFilter()
    {
        static const auto taps = []
        {
            std::array<float, 49> h {};
            double sum = 0.0;
            for (int i = 0; i < 49; ++i)
            {
                const double x = i - 24;
                const double sinc = x == 0.0 ? 1.0 : std::sin (3.14159265358979 * x * 0.25) / (3.14159265358979 * x * 0.25);
                const double w = 0.42 - 0.5 * std::cos (2.0 * 3.14159265358979 * i / 48.0) + 0.08 * std::cos (4.0 * 3.14159265358979 * i / 48.0);
                h[(size_t) i] = (float) (sinc * w * 0.25);
                sum += h[(size_t) i];
            }
            for (auto& v : h) v = (float) (v / sum);
            return h;
        }();
        return taps.data();
    }

    float shapeOf (int type, float x)
    {
        switch (type)
        {
            case 0:  return std::tanh (x);                                                 // tube: smooth
            case 1:  return x > 0.0f ? x / (1.0f + x) : x / (1.0f - 0.7f * x);             // tape: uneven for + and - (even harmonics)
            case 2:  return juce::jlimit (-1.0f, 1.0f, x);                                 // hard clip
            case 3:  return std::sin (1.5707963f * juce::jlimit (-4.0f, 4.0f, x));         // fold: beyond full scale the wave folds back
            default: return x >= 0.0f ? 1.0f - std::exp (-x) : -0.6f * (1.0f - std::exp (x));   // diode: one side harder than the other
        }
    }
}

// Gate: closes (by `range` dB) when the signal stays under the threshold. The detector can be filtered (so the bleed of other instruments doesn't open
// it), it opens at the threshold and closes `hysteresis` dB lower (no chatter), with attack, hold and release.
void TrackFx::processGate (float* l, float* r, int n, const FxParams& p)
{
    const float openLevel = juce::Decibels::decibelsToGain (p.gateThreshold.load());
    const float closeLevel = juce::Decibels::decibelsToGain (p.gateThreshold.load() - juce::jmax (0.0f, p.gateHyst.load()));
    const float closed = juce::Decibels::decibelsToGain (-juce::jmax (0.0f, p.gateRange.load()));
    const float attack = std::exp (-1.0f / (0.001f * juce::jmax (0.05f, p.gateAttack.load()) * (float) sampleRate));
    const float release = std::exp (-1.0f / (0.001f * juce::jmax (1.0f, p.gateRelease.load()) * (float) sampleRate));
    const int hold = (int) (0.001f * p.gateHold.load() * (float) sampleRate);
    const float envFall = std::exp (-1.0f / (0.02f * (float) sampleRate));   // the level follower falls in 20 ms

    const float hp = p.gateScHpf, lp = p.gateScLpf;
    const bool useHp = hp > 21.0f, useLp = lp < 19900.0f;
    if (hp != lastGateHp) { lastGateHp = hp; gateHp = highpassCoef (sampleRate, hp, 0.707); }
    if (lp != lastGateLp) { lastGateLp = lp; gateLp = lowpass (sampleRate, lp, 0.707); }

    float minGain = 1.0f;
    for (int i = 0; i < n; ++i)
    {
        float key = 0.5f * (l[i] + r[i]);
        if (useHp) key = run (gateHp, gateState[0], key);
        if (useLp) key = run (gateLp, gateState[1], key);
        const float level = std::abs (key);
        gateEnv = level > gateEnv ? level : envFall * gateEnv + (1.0f - envFall) * level;

        if (gateEnv > openLevel)
        {
            gateOpen = true;
            gateHoldLeft = hold;
        }
        else if (gateEnv < closeLevel)
        {
            if (gateHoldLeft > 0)
                --gateHoldLeft;
            else
                gateOpen = false;
        }
        const float target = gateOpen ? 1.0f : closed;
        const float coef = target > gateGain ? attack : release;
        gateGain = coef * gateGain + (1.0f - coef) * target;
        minGain = juce::jmin (minGain, gateGain);
        l[i] *= gateGain;
        r[i] *= gateGain;
    }
    p.gateGr.store (-juce::Decibels::gainToDecibels (minGain, -80.0f));
}

// De-esser: a band-pass at the sibilance frequency (with an adjustable width) is the detector; when it is loud, only that band is turned down - or the whole
// sound in wide mode. Listen plays the detector's band, to find the right frequency.
void TrackFx::processDeEsser (float* l, float* r, int n, const FxParams& p)
{
    const float freq = juce::jlimit (2000.0f, 14000.0f, p.deEssFreq.load());
    const float q = juce::jlimit (0.3f, 6.0f, p.deEssQ.load());
    if (freq != lastDeEssFreq || q != lastDeEssQ)
    {
        lastDeEssFreq = freq;
        lastDeEssQ = q;
        const double w = 2.0 * 3.14159265358979 * freq / sampleRate, al = std::sin (w) / (2.0 * q), cs = std::cos (w);
        deEssCoef = normalised (al, 0.0, -al, 1.0 + al, -2.0 * cs, 1.0 - al);   // band-pass, constant 0 dB peak gain
    }
    const float threshold = p.deEssThreshold;
    const float amount = juce::jmax (0.0f, p.deEssAmount.load());
    const bool wide = p.deEssWide, listen = p.deEssListen;
    const float attack = std::exp (-1.0f / (0.0005f * (float) sampleRate)), release = std::exp (-1.0f / (0.06f * (float) sampleRate));

    float maxReduction = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float bl = run (deEssCoef, deEssState[0], l[i]);
        const float br = run (deEssCoef, deEssState[1], r[i]);
        const float level = juce::jmax (std::abs (bl), std::abs (br));
        const float coef = level > deEssEnv ? attack : release;
        deEssEnv = coef * deEssEnv + (1.0f - coef) * level;

        const float over = 20.0f * std::log10 (juce::jmax (deEssEnv, 1.0e-6f)) - threshold;
        const float reductionDb = juce::jlimit (0.0f, amount, over > 0.0f ? over * 0.8f : 0.0f);
        maxReduction = juce::jmax (maxReduction, reductionDb);
        const float gain = juce::Decibels::decibelsToGain (-reductionDb);
        if (listen)
        {
            l[i] = bl;
            r[i] = br;
        }
        else if (wide)
        {
            l[i] *= gain;
            r[i] *= gain;
        }
        else
        {
            l[i] -= bl * (1.0f - gain);   // only the band is lowered
            r[i] -= br * (1.0f - gain);
        }
    }
    p.deEssGr.store (maxReduction);
}

// Saturation: five characters, drive in, bias (uneven clipping adds even harmonics), a tone tilt, a mix and a level that stays about the same. With
// oversampling the clipping runs at 4x the sample rate (no folded-back harmonics, so it sounds clean instead of fizzy); the dry signal is delayed to match.
void TrackFx::processSaturation (float* l, float* r, int n, const FxParams& p)
{
    const float drive = juce::Decibels::decibelsToGain (juce::jmax (0.0f, p.satDrive.load()));
    const int type = juce::jlimit (0, 4, (int) std::lround (p.satType.load()));
    const float mix = juce::jlimit (0.0f, 1.0f, p.satMix.load());
    const float out = juce::Decibels::decibelsToGain (p.satOutput.load());
    const float bias = juce::jlimit (0.0f, 1.0f, p.satBias.load()) * 0.4f;
    const float tone = juce::jlimit (-1.0f, 1.0f, p.satTone.load());
    const bool over = p.satOversample;

    const float ref = shapeOf (type, drive * 0.25f + bias) - shapeOf (type, bias);
    const float norm = ref > 1.0e-4f ? 0.25f / ref : 1.0f;   // a signal at -12 dBFS keeps its level
    const float dc = shapeOf (type, bias);
    auto shape = [&] (float x) { return (shapeOf (type, x * drive + bias) - dc) * norm; };

    // tone: negative = a low-pass that goes down to 1 kHz, positive = the highs lifted (a split at 3 kHz)
    const float lpHz = tone < 0.0f ? 20000.0f * std::pow (0.05f, -tone) : 3000.0f;
    const float lpCoef = 1.0f - std::exp (-2.0f * 3.14159265f * lpHz / (float) sampleRate);

    const float* h = overFilter();
    float* chans[2] = { l, r };
    for (int ch = 0; ch < 2; ++ch)
    {
        auto& o = satOver[ch];
        float* x = chans[ch];
        for (int i = 0; i < n; ++i)
        {
            const float in = x[i];
            float wet;
            float dry = in;
            if (over)
            {
                for (int k = overHist - 1; k > 0; --k) o.up[k] = o.up[k - 1];
                o.up[0] = in;
                float acc = 0.0f;
                for (int k = 0; k < 4; ++k)   // 4 interpolated samples (polyphase), shaped, into the decimating filter
                {
                    float y = 0.0f;
                    for (int m = 0; 4 * m + k < overTaps; ++m)
                        y += h[4 * m + k] * o.up[m];
                    o.down[o.downPos] = shape (y * 4.0f);
                    o.downPos = (o.downPos + 1) % overTaps;
                    if (k == 0)   // the output of the decimating filter is taken at the first of the four: the whole path then delays exactly 12 samples
                        for (int j = 0; j < overTaps; ++j)
                            acc += h[j] * o.down[(o.downPos - 1 - j + overTaps * 2) % overTaps];
                }
                wet = acc;
                dry = o.dry[o.dryPos];   // (12 samples late, like the wet signal)
                o.dry[o.dryPos] = in;
                o.dryPos = (o.dryPos + 1) % 12;
            }
            else
                wet = shape (in);

            if (tone < 0.0f)
            {
                satLp[ch] += lpCoef * (wet - satLp[ch]);
                wet = satLp[ch];
            }
            else if (tone > 0.0f)
            {
                satTilt[ch] += lpCoef * (wet - satTilt[ch]);
                wet += tone * 1.5f * (wet - satTilt[ch]);
            }
            x[i] = (dry * (1.0f - mix) + wet * mix) * out;
        }
    }
}

// Multiband compressor: the signal is split in low, mid and high with 4th order Linkwitz-Riley filters (they add up flat again); every band has its own
// threshold and gain, the ratio, attack and release are shared. The meters show the reduction of each band.
void TrackFx::processMultiband (float* l, float* r, int n, const FxParams& p)
{
    const float f1 = juce::jlimit (40.0f, 1000.0f, p.mbLowFreq.load());
    const float f2 = juce::jlimit (juce::jmax (300.0f, f1 * 2.0f), 12000.0f, p.mbHighFreq.load());
    if (f1 != lastMbFreq[0] || f2 != lastMbFreq[1])
    {
        lastMbFreq[0] = f1;
        lastMbFreq[1] = f2;
        mbLp1 = lowpass (sampleRate, f1, 0.70710678);
        mbHp1 = highpassCoef (sampleRate, f1, 0.70710678);
        mbLp2 = lowpass (sampleRate, f2, 0.70710678);
        mbHp2 = highpassCoef (sampleRate, f2, 0.70710678);
    }

    const float thr[3] = { p.mbLowThr.load(), p.mbMidThr.load(), p.mbHighThr.load() };
    const float bandGain[3] = { juce::Decibels::decibelsToGain (p.mbLowGain.load()), juce::Decibels::decibelsToGain (p.mbMidGain.load()),
                                juce::Decibels::decibelsToGain (p.mbHighGain.load()) };
    const float slope = 1.0f - 1.0f / juce::jmax (1.0f, p.mbRatio.load());
    const float makeup = juce::Decibels::decibelsToGain (p.mbMakeup.load());
    const float attack = std::exp (-1.0f / (0.001f * juce::jmax (0.5f, p.mbAttack.load()) * (float) sampleRate));
    const float release = std::exp (-1.0f / (0.001f * juce::jmax (5.0f, p.mbRelease.load()) * (float) sampleRate));

    float maxGr[3] = { 0.0f, 0.0f, 0.0f };
    float* chans[2] = { l, r };
    for (int i = 0; i < n; ++i)
    {
        float band[2][3];
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* st = mbState[ch];
            const float x = chans[ch][i];
            float low = run (mbLp1, st[1], run (mbLp1, st[0], x));
            float rest = run (mbHp1, st[3], run (mbHp1, st[2], x));
            // the low band gets the all-pass of the second split (low-pass + high-pass of it), so the phases of the three bands match
            low = run (mbLp2, st[9], run (mbLp2, st[8], low)) + run (mbHp2, st[11], run (mbHp2, st[10], low));
            band[ch][0] = low;
            band[ch][1] = run (mbLp2, st[5], run (mbLp2, st[4], rest));
            band[ch][2] = run (mbHp2, st[7], run (mbHp2, st[6], rest));
        }

        for (int b = 0; b < 3; ++b)
        {
            const float level = juce::jmax (std::abs (band[0][b]), std::abs (band[1][b]));
            const float coef = level > mbEnv[b] ? attack : release;
            mbEnv[b] = coef * mbEnv[b] + (1.0f - coef) * level;
            const float over = 20.0f * std::log10 (juce::jmax (mbEnv[b], 1.0e-6f)) - thr[b];
            const float reductionDb = over > 0.0f ? over * slope : 0.0f;
            maxGr[b] = juce::jmax (maxGr[b], reductionDb);
            const float gain = juce::Decibels::decibelsToGain (-reductionDb) * bandGain[b];
            band[0][b] *= gain;
            band[1][b] *= gain;
        }

        l[i] = (band[0][0] + band[0][1] + band[0][2]) * makeup;
        r[i] = (band[1][0] + band[1][1] + band[1][2]) * makeup;
    }
    for (int b = 0; b < 3; ++b)
        p.mbGr[b].store (maxGr[b]);
}

// Chorus / flanger: 1 to 4 modulated delays per side (the phases spread evenly, the right side a quarter turn behind, scaled by the width), a tone filter on
// the effect, feedback, and a crossfade with the dry signal (mix 1 = vibrato). A short delay with feedback makes a flanger.
void TrackFx::processChorus (float* l, float* r, int n, const FxParams& p)
{
    const int size = (int) chL.size();
    const float baseSamples = juce::jlimit (0.5f, 40.0f, p.chorusDelay.load()) * 0.001f * (float) sampleRate;
    const float depth = juce::jlimit (0.0f, 1.0f, p.chorusDepth.load()) * baseSamples * 0.8f;
    const float feedback = juce::jlimit (0.0f, 0.9f, p.chorusFeedback.load());
    const float mix = juce::jlimit (0.0f, 1.0f, p.chorusMix.load());
    const float step = juce::jlimit (0.01f, 10.0f, p.chorusRate.load()) / (float) sampleRate;
    const int voices = juce::jlimit (1, 4, (int) std::lround (p.chorusVoices.load()));
    const float width = juce::jlimit (0.0f, 1.0f, p.chorusWidth.load());
    const float toneCoef = 1.0f - std::exp (-2.0f * 3.14159265f * juce::jlimit (1000.0f, 20000.0f, p.chorusTone.load()) / (float) sampleRate);

    auto read = [&] (const std::vector<float>& buf, float delay)
    {
        float rp = (float) chPos - delay;
        while (rp < 0.0f) rp += (float) size;
        const int i0 = (int) rp;
        const float frac = rp - (float) i0;
        return buf[(size_t) i0 % (size_t) size] * (1.0f - frac) + buf[(size_t) (i0 + 1) % (size_t) size] * frac;
    };

    for (int i = 0; i < n; ++i)
    {
        chPhase += step;
        if (chPhase >= 1.0f) chPhase -= 1.0f;
        float wl = 0.0f, wr = 0.0f;
        for (int v = 0; v < voices; ++v)
        {
            const float phase = chPhase + (float) v / (float) voices;
            wl += read (chL, juce::jlimit (1.0f, (float) size - 3.0f, baseSamples + depth * std::sin (6.2831853f * phase)));
            wr += read (chR, juce::jlimit (1.0f, (float) size - 3.0f, baseSamples + depth * std::sin (6.2831853f * (phase + 0.25f * width))));
        }
        wl /= (float) voices;
        wr /= (float) voices;
        chToneL += toneCoef * (wl - chToneL);
        chToneR += toneCoef * (wr - chToneR);
        wl = chToneL;
        wr = chToneR;
        if (width < 1.0f)   // narrower: the two sides move towards each other
        {
            const float m = 0.5f * (wl + wr);
            wl = m + (wl - m) * width;
            wr = m + (wr - m) * width;
        }

        chL[(size_t) chPos] = l[i] + wl * feedback;
        chR[(size_t) chPos] = r[i] + wr * feedback;
        if (++chPos >= size) chPos = 0;
        l[i] = l[i] * (1.0f - mix) + wl * mix;
        r[i] = r[i] * (1.0f - mix) + wr * mix;
    }
}

// Limiter with lookahead: the gain is brought down smoothly in the time before a peak arrives (a minimum-hold followed by a moving average over the
// lookahead), so nothing is clipped; the release is slow and smooth. The sound is delayed by the lookahead (the track is compensated). Optionally the
// peaks between the samples are found too (true peak, by 4x interpolation). The ceiling is also a hard stop for what is left.
void TrackFx::processLimiter (float* l, float* r, int n, const FxParams& p)
{
    const float in = juce::Decibels::decibelsToGain (p.limGain.load());
    const float ceiling = juce::Decibels::decibelsToGain (juce::jmin (0.0f, p.limCeiling.load()));
    const float release = std::exp (-1.0f / (0.001f * juce::jmax (5.0f, p.limRelease.load()) * (float) sampleRate));
    const int look = juce::jlimit (0, 480, (int) std::lround (p.limLookahead.load() * 0.001 * sampleRate));
    const bool truePeak = p.limTruePeak;

    if (look != limLastLook)   // a new lookahead: the delay line and the windows start again (silence)
    {
        limLastLook = look;
        limDelayL.assign ((size_t) look + 1, 0.0f);
        limDelayR.assign ((size_t) look + 1, 0.0f);
        limNeed.assign ((size_t) look + 1, 1.0f);
        limMin.assign ((size_t) look + 1, 1.0f);
        limPos = 0;
        limSumMin = (float) (look + 1);
    }

    float minGain = 1.0f;
    const int window = look + 1;
    for (int i = 0; i < n; ++i)
    {
        const float x = in * l[i], y = in * r[i];
        float peak = juce::jmax (std::abs (x), std::abs (y));
        if (truePeak)
        {
            // Catmull-Rom interpolation between the previous two samples at 1/4, 1/2 and 3/4 (history: oldest first)
            for (int k = 0; k < 3; ++k) { limHistL[k] = limHistL[k + 1]; limHistR[k] = limHistR[k + 1]; }
            limHistL[3] = x;
            limHistR[3] = y;
            for (float t : { 0.25f, 0.5f, 0.75f })
            {
                auto cr = [t] (const float* v) { return 0.5f * ((2.0f * v[1]) + (-v[0] + v[2]) * t + (2.0f * v[0] - 5.0f * v[1] + 4.0f * v[2] - v[3]) * t * t + (-v[0] + 3.0f * v[1] - 3.0f * v[2] + v[3]) * t * t * t); };
                peak = juce::jmax (peak, std::abs (cr (limHistL)), std::abs (cr (limHistR)));
            }
        }
        const float need = peak > ceiling ? ceiling / peak : 1.0f;

        // delay line, the "needed gain" of the last window, the minimum of it, and the moving average of that minimum
        limDelayL[(size_t) limPos] = x;
        limDelayR[(size_t) limPos] = y;
        limNeed[(size_t) limPos] = need;
        float mn = 1.0f;
        for (int k = 0; k < window; ++k)
            mn = juce::jmin (mn, limNeed[(size_t) k]);
        limSumMin += mn - limMin[(size_t) limPos];
        limMin[(size_t) limPos] = mn;
        const float target = juce::jmin (1.0f, limSumMin / (float) window);

        if (target < limGainNow)
            limGainNow = target;                                          // down: as fast as the window says
        else
            limGainNow = release * limGainNow + (1.0f - release) * target;   // up: the release
        limGainNow = juce::jmin (limGainNow, 1.0f);

        const int readPos = (limPos + 1) % window;   // the oldest sample: `look` samples ago
        const float dx = limDelayL[(size_t) readPos], dy = limDelayR[(size_t) readPos];
        limPos = (limPos + 1) % window;
        minGain = juce::jmin (minGain, limGainNow);
        l[i] = juce::jlimit (-ceiling, ceiling, dx * limGainNow);
        r[i] = juce::jlimit (-ceiling, ceiling, dy * limGainNow);
    }
    p.limGr.store (-juce::Decibels::gainToDecibels (minGain, -60.0f));
}
