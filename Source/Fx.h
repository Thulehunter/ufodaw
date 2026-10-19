#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
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
    mutable std::atomic<float> compGr { 0.0f };                                                   // meter (audio thread writes): gain reduction in dB

    std::atomic<bool> delayOn { false };
    std::atomic<float> delayTime { 350.0f }, delayFeedback { 0.35f }, delayMix { 0.3f };          // ms
    std::atomic<bool> delayPing { false };                                                        // ping-pong: the echoes bounce left and right
    std::atomic<float> delayRatio { 1.0f }, delayLowCut { 20.0f }, delayHighCut { 20000.0f }, delayMod { 0.0f };   // right time / left time, Hz, Hz, 0..1 (tape wobble)

    std::atomic<bool> reverbOn { false };
    std::atomic<float> reverbSize { 0.5f }, reverbDamping { 0.5f }, reverbMix { 0.25f };
    std::atomic<float> reverbDecay { 2.0f }, reverbPreDelay { 0.0f }, reverbLowCut { 20.0f }, reverbWidth { 1.0f }, reverbMod { 0.3f };   // s (RT60), ms, Hz, 0..1, 0..1

    // Not an effect of the chain: the channel plays in mono (left and right summed) after its effects and plugins.
    std::atomic<bool> mono { false };

    bool anyOn() const { return eqOn || compOn || delayOn || reverbOn; }
    bool hasTail() const { return delayOn || reverbOn; }

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

private:
    void processEq (float* l, float* r, int n, const FxParams&);
    void processCompressor (float* l, float* r, int n, const FxParams&, const float* keyL, const float* keyR);
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
