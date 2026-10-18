#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <functional>
#include <vector>

// Effect settings of one track. Written by the UI, read by the audio thread.
// Chain order: EQ -> compressor -> delay -> reverb.
struct FxParams
{
    std::atomic<bool> eqOn { false };
    std::atomic<float> eqLow { 0.0f }, eqMid { 0.0f }, eqMidFreq { 1000.0f }, eqHigh { 0.0f };   // dB, Hz

    std::atomic<bool> compOn { false };
    std::atomic<float> compThreshold { -18.0f }, compRatio { 3.0f }, compAttack { 10.0f },
                       compRelease { 120.0f }, compMakeup { 0.0f };                              // dB, :1, ms, ms, dB

    std::atomic<bool> delayOn { false };
    std::atomic<float> delayTime { 350.0f }, delayFeedback { 0.35f }, delayMix { 0.3f };          // ms

    std::atomic<bool> reverbOn { false };
    std::atomic<float> reverbSize { 0.5f }, reverbDamping { 0.5f }, reverbMix { 0.25f };

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

    juce::IIRFilter eq[3][2];   // low shelf, peak, high shelf x (left, right)
    float lastEq[4] = { 1e9f, 1e9f, 1e9f, 1e9f };

    float compEnv = 0.0f;

    std::vector<float> delayL, delayR;
    int delayPos = 0;

    juce::Reverb reverb;
    float lastReverb[3] = { -1.0f, -1.0f, -1.0f };
};
