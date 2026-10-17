#include "Stretch.h"
#include <rubberband/RubberBandStretcher.h>
#include <algorithm>
#include <cmath>

namespace Stretch
{
    std::shared_ptr<juce::AudioBuffer<float>> process (const juce::AudioBuffer<float>& in, int startSample, int numSamples,
                                                       double sampleRate, double timeRatio, double semitones,
                                                       const std::function<bool (double)>& progress)
    {
        using RB = RubberBand::RubberBandStretcher;

        const int channels = juce::jlimit (1, 2, in.getNumChannels());
        numSamples = juce::jlimit (0, in.getNumSamples() - juce::jmax (0, startSample), numSamples);
        if (numSamples <= 0 || timeRatio <= 0.0)
            return std::make_shared<juce::AudioBuffer<float>> (channels, 1);

        const int options = RB::OptionProcessOffline | RB::OptionEngineFiner | RB::OptionPitchHighQuality | RB::OptionChannelsTogether;
        RB stretcher ((size_t) sampleRate, (size_t) channels, options, timeRatio, std::pow (2.0, semitones / 12.0));
        stretcher.setExpectedInputDuration ((size_t) numSamples);

        constexpr int block = 4096;
        const float* pointers[2] = {};
        auto point = [&] (int position)
        {
            for (int c = 0; c < channels; ++c)
                pointers[c] = in.getReadPointer (c, startSample + position);
        };

        // the expected output length, with some room: the stretcher may produce a little more
        const int expected = (int) std::llround ((double) numSamples * timeRatio);
        auto out = std::make_shared<juce::AudioBuffer<float>> (channels, expected + block);
        out->clear();
        int written = 0;

        auto drain = [&]
        {
            for (;;)
            {
                const int available = stretcher.available();
                if (available <= 0)
                    break;
                if (written + available > out->getNumSamples())
                    out->setSize (channels, written + available + block, true, true, false);
                float* dest[2] = {};
                for (int c = 0; c < channels; ++c)
                    dest[c] = out->getWritePointer (c, written);
                written += (int) stretcher.retrieve (dest, (size_t) available);
            }
        };

        for (int pos = 0; pos < numSamples; pos += block)   // study: the whole input is looked at first (offline mode)
        {
            const int count = juce::jmin (block, numSamples - pos);
            point (pos);
            stretcher.study (pointers, (size_t) count, pos + count >= numSamples);
            if (progress && ! progress (0.35 * (double) (pos + count) / (double) numSamples))
                return nullptr;
        }

        for (int pos = 0; pos < numSamples; pos += block)   // then it is processed
        {
            const int count = juce::jmin (block, numSamples - pos);
            point (pos);
            stretcher.process (pointers, (size_t) count, pos + count >= numSamples);
            drain();
            if (progress && ! progress (0.35 + 0.65 * (double) (pos + count) / (double) numSamples))
                return nullptr;
        }
        drain();

        out->setSize (channels, juce::jmax (1, expected), true, true, false);   // exactly the length the ratio asks for
        return out;
    }
}
