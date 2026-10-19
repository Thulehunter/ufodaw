#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "Loudness.h"
#include <atomic>
#include <functional>
#include <vector>

// Effect settings of one track. Written by the UI, read by the audio thread.
// Chain order: EQ -> compressor -> delay -> reverb.
struct FxParams
{
    // EQ, 6 bands: high-pass (24 dB/oct), low shelf, two peaks, high shelf, low-pass (24 dB/oct)
    std::atomic<bool> eqOn { false };
    std::atomic<float> eqLow { 0.0f }, eqMid { 0.0f }, eqMidFreq { 1000.0f }, eqHigh { 0.0f };   // dB, Hz
    std::atomic<float> eqHpFreq { 20.0f }, eqLpFreq { 20000.0f };                                 // Hz (20 / 20000 = off)
    std::atomic<float> eqLowFreq { 120.0f }, eqHighFreq { 6000.0f };                              // Hz
    std::atomic<float> eqMidQ { 1.0f }, eqMid2 { 0.0f }, eqMid2Freq { 4000.0f }, eqMid2Q { 1.0f };

    std::atomic<bool> compOn { false };
    std::atomic<float> compThreshold { -18.0f }, compRatio { 3.0f }, compAttack { 10.0f },
                       compRelease { 120.0f }, compMakeup { 0.0f };                              // dB, :1, ms, ms, dB
    std::atomic<float> compKnee { 6.0f }, compMix { 1.0f }, compRms { 0.3f }, compScHpf { 20.0f };   // dB, 0..1 (parallel), 0 = peak .. 1 = RMS, Hz
    std::atomic<bool> compAuto { false };                                                         // makeup that follows threshold and ratio
    mutable std::atomic<bool> analyse { false };                                                  // the FX panel is open on the EQ: keep a copy of the sound for its spectrum
    mutable std::atomic<float> compGr { 0.0f };                                               // meter (audio thread writes): gain reduction in dB

    std::atomic<bool> delayOn { false };
    std::atomic<float> delayTime { 350.0f }, delayFeedback { 0.35f }, delayMix { 0.3f };          // ms
    std::atomic<bool> delayPing { false };                                                        // ping-pong: the echoes bounce left and right
    std::atomic<float> delayRatio { 1.0f }, delayLowCut { 20.0f }, delayHighCut { 20000.0f }, delayMod { 0.0f };   // right time / left time, Hz, Hz, 0..1 (tape wobble)

    std::atomic<bool> reverbOn { false };
    std::atomic<float> reverbSize { 0.5f }, reverbDamping { 0.5f }, reverbMix { 0.25f };
    std::atomic<float> reverbDecay { 2.0f }, reverbPreDelay { 0.0f }, reverbLowCut { 20.0f }, reverbWidth { 1.0f }, reverbMod { 0.3f };   // s (RT60), ms, Hz, 0..1, 0..1

    // More effects. Order of the whole chain: gate, EQ, compressor, de-esser, saturation, multiband compressor, chorus/flanger, delay, reverb, limiter.
    std::atomic<bool> gateOn { false };
    std::atomic<float> gateThreshold { -50.0f }, gateRange { 40.0f }, gateAttack { 1.0f }, gateHold { 50.0f }, gateRelease { 150.0f };   // dB, dB, ms, ms, ms
    std::atomic<float> gateHyst { 4.0f }, gateScHpf { 20.0f }, gateScLpf { 20000.0f };             // dB (it closes this far under the opening level), Hz, Hz (the detector's filters)
    mutable std::atomic<float> gateGr { 0.0f };                                                    // meter: how far the gate is turned down (dB)

    std::atomic<bool> deEssOn { false };
    std::atomic<float> deEssFreq { 6500.0f }, deEssThreshold { -28.0f }, deEssAmount { 8.0f }, deEssQ { 1.5f };   // Hz, dB, dB (the most it takes off), width of the band
    std::atomic<bool> deEssWide { false }, deEssListen { false };                                  // lower the whole sound instead of only the band / hear the band the detector listens to
    mutable std::atomic<float> deEssGr { 0.0f };

    std::atomic<bool> satOn { false };
    std::atomic<float> satDrive { 6.0f }, satType { 0.0f }, satMix { 1.0f }, satOutput { 0.0f };  // dB, 0 tube / 1 tape / 2 hard clip / 3 fold / 4 diode, 0..1, dB
    std::atomic<float> satBias { 0.0f }, satTone { 0.0f };                                         // 0..1 (uneven clipping: even harmonics), -1 dark .. +1 bright
    std::atomic<bool> satOversample { true };                                                      // 4x oversampling: much less aliasing (the sound is delayed 12 samples, the dry signal too)

    std::atomic<bool> mbOn { false };
    std::atomic<float> mbLowFreq { 200.0f }, mbHighFreq { 2500.0f }, mbLowThr { -24.0f }, mbMidThr { -24.0f }, mbHighThr { -24.0f },
                       mbRatio { 3.0f }, mbMakeup { 0.0f };                                       // Hz, Hz, dB, dB, dB, :1, dB
    std::atomic<float> mbLowGain { 0.0f }, mbMidGain { 0.0f }, mbHighGain { 0.0f }, mbAttack { 8.0f }, mbRelease { 120.0f };   // dB per band (after compressing), ms, ms
    mutable std::atomic<float> mbGr[3] = { { 0.0f }, { 0.0f }, { 0.0f } };                                                            // meters: gain reduction of the three bands (dB)

    std::atomic<bool> chorusOn { false };
    std::atomic<float> chorusRate { 0.8f }, chorusDepth { 0.5f }, chorusDelay { 12.0f }, chorusFeedback { 0.0f }, chorusMix { 0.4f };   // Hz, 0..1, ms, 0..0.9, 0..1 (crossfade: 1 = only the effect)
    std::atomic<float> chorusVoices { 2.0f }, chorusWidth { 1.0f }, chorusTone { 20000.0f };       // 1..4, 0..1, Hz (low-pass on the effect)

    std::atomic<bool> limOn { false };
    std::atomic<float> limGain { 0.0f }, limCeiling { -1.0f }, limRelease { 80.0f }, limLookahead { 2.0f };   // dB, dB, ms, ms (the sound is delayed by this; the track is compensated)
    std::atomic<bool> limTruePeak { false };                                                       // also catch the peaks between the samples
    mutable std::atomic<float> limGr { 0.0f };                                                     // meter (dB)
    mutable std::atomic<int> latencySamples { 0 };                                                 // what the chain delays the sound by now (the limiter's lookahead, saturation oversampling)

    // Not an effect of the chain: the channel plays in mono (left and right summed) after its effects and plugins.
    std::atomic<bool> mono { false };

    bool anyOn() const { return eqOn || compOn || delayOn || reverbOn || gateOn || deEssOn || satOn || mbOn || chorusOn || limOn || analyse.load(); }
    bool hasTail() const { return delayOn || reverbOn || chorusOn; }

    // Used for saving/loading projects.
    void forEachFloat (const std::function<void (const char*, std::atomic<float>&)>&);
    void forEachBool (const std::function<void (const char*, std::atomic<bool>&)>&);

    std::atomic<float>* findFloat (const juce::String& name);   // by the names forEachFloat uses; null if unknown
};

// Display name and value range of every FxParams float, for automation.
struct FxParamSpec
{
    const char* id;      // same as the name in FxParams::forEachFloat
    const char* label;
    float lo, hi;
};
const std::vector<FxParamSpec>& fxParamSpecs();

// The EQ as biquad sections (RBJ cookbook): 0,1 high-pass, 2 low shelf, 3 and 4 peaks, 5 high shelf, 6,7 low-pass. Shared by the audio and the curve drawing.
struct EqCoef
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    bool active = false;
};
constexpr int eqNumSections = 8;
void eqDesign (const FxParams&, double sampleRate, EqCoef (&out)[eqNumSections]);
double eqResponseDb (const EqCoef (&c)[eqNumSections], double sampleRate, double frequency);
EqCoef highpassCoef (double sampleRate, double frequency, double q);

// Audio-thread state for one track's effect chain. No allocation outside prepare().
class TrackFx
{
public:
    void prepare (double sampleRate);
    void reset();
    // keyL / keyR: another track's signal for the compressor to listen to (sidechain: the compressor ducks this track when the key is loud);
    // null = it listens to its own signal.
    void process (float* left, float* right, int numSamples, const FxParams&, const float* keyL = nullptr, const float* keyR = nullptr);

    // The sound after the EQ (written while FxParams::analyse is set), for the spectrum behind the EQ curve
    const SpectrumTap& getSpectrumTap() const { return tap; }

private:
    void processEq (float* l, float* r, int n, const FxParams&);
    void processCompressor (float* l, float* r, int n, const FxParams&, const float* keyL, const float* keyR);
    void processGate (float* l, float* r, int n, const FxParams&);
    void processDeEsser (float* l, float* r, int n, const FxParams&);
    void processSaturation (float* l, float* r, int n, const FxParams&);
    void processMultiband (float* l, float* r, int n, const FxParams&);
    void processChorus (float* l, float* r, int n, const FxParams&);
    void processLimiter (float* l, float* r, int n, const FxParams&);
    void processDelay (float* l, float* r, int n, const FxParams&);
    void processReverb (float* l, float* r, int n, const FxParams&);

    double sampleRate = 44100.0;

    struct Biquad { double z1 = 0.0, z2 = 0.0; };   // transposed direct form II state
    static float run (const EqCoef& c, Biquad& s, float x)
    {
        const double in = x, out = c.b0 * in + s.z1;
        s.z1 = c.b1 * in - c.a1 * out + s.z2;
        s.z2 = c.b2 * in - c.a2 * out;
        return (float) out;
    }

    EqCoef eqCoef[eqNumSections];
    Biquad eqState[eqNumSections][2];
    float lastEq[12];
    bool eqKnown = false;

    float compGr = 0.0f, rmsSq = 0.0f;   // gain reduction (dB, smoothed) and the mean square of the detector
    EqCoef scCoef;                       // high-pass in the detector's path
    Biquad scState[2];
    float lastScHpf = -1.0f;

    SpectrumTap tap;
    // gate
    float gateEnv = 0.0f, gateGain = 1.0f;
    int gateHoldLeft = 0;
    bool gateOpen = false;
    EqCoef gateHp, gateLp;
    Biquad gateState[2];
    float lastGateHp = -1.0f, lastGateLp = -1.0f;
    // de-esser: band-pass of the detector, per channel
    EqCoef deEssCoef;
    Biquad deEssState[2];
    float lastDeEssFreq = -1.0f, lastDeEssQ = -1.0f, deEssEnv = 0.0f, deEssGain = 1.0f;
    // multiband: 3 bands (4th order Linkwitz-Riley splits) with their own gain reduction
    EqCoef mbLp1, mbHp1, mbLp2, mbHp2;                 // (first split: low / rest; second split: mid / high; the low band also gets an all-pass at the second)
    Biquad mbState[2][12];                             // [channel][filter]: low-pass 1 (x2), high-pass 1 (x2), low-pass 2 (x2), high-pass 2 (x2), all-pass of the low band (x4)
    float lastMbFreq[2] = { -1.0f, -1.0f };
    float mbEnv[3] = { 0.0f, 0.0f, 0.0f };
    // chorus / flanger
    std::vector<float> chL, chR;
    int chPos = 0;
    float chPhase = 0.0f;
    // saturation: 4x oversampling (windowed sinc), the dry signal delayed to match, and the tone filters
    static constexpr int overTaps = 49, overHist = 13;
    struct Over4
    {
        float up[overHist] = {};             // the newest input samples (for the 4 polyphase outputs)
        float down[overTaps] = {};           // the newest samples at 4x (for the decimating filter)
        int downPos = 0;
        float dry[16] = {};                  // delay line of the dry signal (12 samples)
        int dryPos = 0;
    };
    Over4 satOver[2];
    float satLp[2] = { 0.0f, 0.0f }, satTilt[2] = { 0.0f, 0.0f };
    // chorus extras
    float chToneL = 0.0f, chToneR = 0.0f;
    // limiter: lookahead with a minimum-hold and a smoothing window (the gain is down in time for the peak), a true-peak detector
    std::vector<float> limDelayL, limDelayR, limNeed, limMin;
    int limPos = 0;
    float limSumMin = 0.0f;
    float limHistL[4] = {}, limHistR[4] = {};
    float limGainNow = 1.0f;
    int limLastLook = -1;

    std::vector<float> delayL, delayR;
    int delayPos = 0;
    float delayLfo = 0.0f;
    float dlpL = 0.0f, dlpR = 0.0f, dhpL = 0.0f, dhpR = 0.0f;   // the filters in the feedback of the delay
    float smoothTimeL = -1.0f, smoothTimeR = -1.0f;              // delay times glide so turning the knob doesn't click

    // Reverb: a feedback delay network of 8 lines (Hadamard mixing), pre-delay, damping, low cut, width and slow modulation
    static constexpr int fdnLines = 8;
    std::vector<float> fdn[fdnLines];
    int fdnPos[fdnLines] = {};
    float fdnLow[fdnLines] = {};          // damping filter state
    std::vector<float> preL, preR;
    int prePos = 0;
    float revHpL = 0.0f, revHpR = 0.0f;
    float revLfo = 0.0f;
    float fdnBase[fdnLines] = {};         // line lengths at size 1 (samples)
};
