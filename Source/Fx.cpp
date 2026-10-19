#include "Fx.h"
#include <algorithm>
#include <cmath>
#include <complex>

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
}

void FxParams::forEachBool (const std::function<void (const char*, std::atomic<bool>&)>& f)
{
    f ("eqOn", eqOn);  f ("compOn", compOn);  f ("delayOn", delayOn);  f ("reverbOn", reverbOn);  f ("mono", mono);  f ("compAuto", compAuto);  f ("delayPing", delayPing);
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
    if (p.eqOn)     processEq (l, r, n, p);
    if (p.compOn)   processCompressor (l, r, n, p, keyL, keyR);
    else            p.compGr.store (0.0f);
    if (p.delayOn)  processDelay (l, r, n, p);
    if (p.reverbOn) processReverb (l, r, n, p);
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
