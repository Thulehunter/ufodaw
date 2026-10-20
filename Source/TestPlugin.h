#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include "PluginSandbox.h"

// A small plugin that is built into the program, for the automatic tests of the plugin sandbox (PluginProxy.h): it is a stereo effect AND a synth (a sine for
// the last note played, plus the input), it has a latency, two parameters and a state, and it gives the same answer for the same input every time, so the
// output in a helper process can be compared sample for sample with the output in the program itself.
// Its "file" says what it does: "synth" (normal), "echo" (also sends a note a fifth higher for every note it gets), "crash" (an access violation
// after 40 blocks), "hang" (never answers after 40 blocks), "leak" (never a problem, only slow: each block takes a few milliseconds).
class TestPlugin : public juce::AudioPluginInstance
{
public:
    explicit TestPlugin (const juce::String& m = "synth")
        : juce::AudioPluginInstance (BusesProperties().withInput ("in", juce::AudioChannelSet::stereo()).withOutput ("out", juce::AudioChannelSet::stereo())),
          mode (m)
    {
        juce::AudioProcessor::addParameter (gain = new juce::AudioParameterFloat (juce::ParameterID { "gain", 1 }, "Gain", juce::NormalisableRange<float> (0.0f, 1.0f), 0.8f));
        juce::AudioProcessor::addParameter (tone = new juce::AudioParameterFloat (juce::ParameterID { "tone", 1 }, "Tone level", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        setLatencySamples (latency);
    }

    static juce::PluginDescription describe (const juce::String& mode)
    {
        juce::PluginDescription d;
        d.name = "UFODAW Test " + mode;
        d.descriptiveName = "Test plugin for the plugin sandbox";
        d.pluginFormatName = PluginSandbox::testFormatName;
        d.fileOrIdentifier = mode;
        d.manufacturerName = "UFODAW";
        d.version = "1.0";
        d.uniqueId = d.deprecatedUid = mode.hashCode();
        d.isInstrument = true;
        d.numInputChannels = d.numOutputChannels = 2;
        return d;
    }

    const juce::String getName() const override { return "UFODAW Test " + mode; }
    void fillInPluginDescription (juce::PluginDescription& d) const override { d = describe (mode); }

    void prepareToPlay (double rate, int) override
    {
        sampleRate = rate;
        reset();
    }
    void releaseResources() override {}
    void reset() override
    {
        delayL.assign ((size_t) latency, 0.0f);
        delayR.assign ((size_t) latency, 0.0f);
        delayAt = 0;
        phase = 0.0;
        env = 0.0f;
        freq = 0.0;
        blocks = 0;
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        const int n = buffer.getNumSamples(), channels = buffer.getNumChannels();
        ++blocks;
        if (mode == "crash" && blocks >= 40)
        {
            volatile int* nothing = nullptr;
            *nothing = 1;   // (on purpose)
        }
        if (mode == "hang" && blocks >= 40)
            juce::Thread::sleep (600000);
        if (mode == "leak")
            juce::Thread::sleep (3);

        juce::MidiBuffer echo;
        std::vector<std::pair<int, int>> notes;   // position, pitch (or -1 for off)
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            if (m.isNoteOn())  { notes.push_back ({ meta.samplePosition, m.getNoteNumber() }); velocity = m.getFloatVelocity(); }
            if (m.isNoteOff()) notes.push_back ({ meta.samplePosition, -1 });
            if (mode == "echo" && m.isNoteOn())  echo.addEvent (juce::MidiMessage::noteOn (m.getChannel(), juce::jmin (127, m.getNoteNumber() + 7), m.getVelocity()), meta.samplePosition);
            if (mode == "echo" && m.isNoteOff()) echo.addEvent (juce::MidiMessage::noteOff (m.getChannel(), juce::jmin (127, m.getNoteNumber() + 7)), meta.samplePosition);
        }
        midi.swapWith (echo);   // (what the plugin sends out: nothing, or the echo)

        const float g = gain->get(), t = tone->get();
        size_t next = 0;
        for (int i = 0; i < n; ++i)
        {
            while (next < notes.size() && notes[next].first <= i)
            {
                if (notes[next].second >= 0) { freq = 440.0 * std::pow (2.0, (notes[next].second - 69) / 12.0); env = velocity; }
                else gate = false;
                if (notes[next].second >= 0) gate = true;
                ++next;
            }
            env = gate ? env : env * 0.9995f;
            const float tonal = (float) std::sin (phase) * env * t;
            phase += 6.283185307179586 * freq / sampleRate;
            if (phase > 6.283185307179586) phase -= 6.283185307179586;

            for (int c = 0; c < channels && c < 2; ++c)
            {
                auto& line = c == 0 ? delayL : delayR;
                float* data = buffer.getWritePointer (c);
                const float dry = line[delayAt];
                line[delayAt] = data[i];
                data[i] = dry * g + tonal;
            }
            delayAt = (delayAt + 1) % (size_t) latency;
        }
    }

    double getTailLengthSeconds() const override { return 0.5; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return mode == "echo"; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Init"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        juce::MemoryOutputStream out (dest, false);
        out.writeFloat (gain->get());
        out.writeFloat (tone->get());
        out.writeInt (stateMarker);
    }
    void setStateInformation (const void* data, int size) override
    {
        juce::MemoryInputStream in (data, (size_t) size, false);
        if (size >= 12)
        {
            *gain = in.readFloat();
            *tone = in.readFloat();
            stateMarker = in.readInt();
        }
    }
    int stateMarker = 0;   // (carried in the state, to see that a restart gives it back)

private:
    juce::String mode;
    juce::AudioParameterFloat* gain = nullptr;
    juce::AudioParameterFloat* tone = nullptr;
    static constexpr int latency = 64;
    std::vector<float> delayL = std::vector<float> (64, 0.0f), delayR = std::vector<float> (64, 0.0f);
    size_t delayAt = 0;
    double sampleRate = 48000.0, phase = 0.0, freq = 0.0;
    float env = 0.0f, velocity = 0.8f;
    bool gate = false;
    int blocks = 0;
};
