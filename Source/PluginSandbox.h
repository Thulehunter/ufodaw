#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <cstdint>

// Plugins in a separate process ("sandbox"): what the program and its helper process agree on.
//
// The helper is UFODAW.exe itself, started with --ufodawhost:<pipe> (see PluginHostWorker.h). It hosts ONE plugin instance. Two channels connect it
// with the program (PluginProxy.h):
//  - a named pipe (JUCE's ChildProcessCoordinator) for everything that is not time critical: loading, state, parameters, the editor window.
//  - shared memory plus two named events for the audio: per block the program writes the input and the MIDI into the shared block, signals
//    `request`, the helper runs the plugin on it in place, and signals `done`. The program waits for `done` (or the helper's death) with a time-out;
//    on a time-out or a dead helper the block becomes silence and the plugin is marked as failed (it never blocks the audio thread longer than that).
namespace PluginSandbox
{
    constexpr const char* commandLineId = "ufodawhost";
    constexpr const char* testFormatName = "UFODAW Test";   // the built-in test plugin (TestPlugin.h): used by the automatic tests

    constexpr int maxChannels = 32;
    constexpr int maxBlock = 8192;          // longer blocks are sent in pieces
    constexpr int maxParamChanges = 256;    // parameter changes per block that travel with the audio (the rest travels over the pipe)
    constexpr int midiAreaBytes = 64 * 1024;

    // Messages over the pipe: [type: 1 byte][seq: 4 bytes][payload]. A request has a seq > 0 and is answered by a Reply with the same seq.
    enum Msg : uint8_t
    {
        // program -> helper
        Load = 1,        // payload: description xml, sample rate, block size, names of the shared memory and the events
        CheckLayout,     // payload: layout. Reply: bool
        ChangeLayout,       // payload: layout (applies it)
        Prepare,         // payload: sample rate, block size. Reply: latency, tail
        Release,
        GetState,        // Reply: the state
        SetState,        // payload: state. Reply: all parameter values
        SetParam,        // payload: index, value (async)
        GetParamText,    // payload: index, value. Reply: text
        GetParamValue,   // payload: index, text. Reply: value
        ShowEditor,      // payload: title
        CloseEditor,
        Reset,
        SetProgram,
        GetProgramName,  // payload: index. Reply: name
        Ping,            // Reply: memory use and GDI/USER object counts of the helper
        Quit,

        // helper -> program
        Reply = 100,     // payload: ok (bool), then the answer (or the error text)
        Hello,           // payload: process id
        ParamChanged,    // payload: index, value (the plugin's own window or the plugin itself moved a parameter)
        GestureBegin,    // payload: index
        GestureEnd,      // payload: index
        LatencyChanged,  // payload: latency, tail
        EditorClosed,
        HelperFailed     // payload: text. The helper caught a fault in the plugin and is going down.
    };

    // The shared block. `request` fields are written by the program before it signals, `reply` fields by the helper before it signals back.
    struct Shared
    {
        uint32_t magic;
        // request
        int32_t numSamples, numChannels, midiInBytes, numParamChanges, flags;   // flags: 1 = reset the plugin before this block
        int32_t isPlaying, isRecording, isLooping, hasLoop, timeSigNum, timeSigDen, hasPosition;
        double bpm, ppq, barStartPpq, timeSeconds, loopStartPpq, loopEndPpq;
        int64_t timeSamples;
        // reply
        int32_t midiOutBytes, latency, status;   // status: 0 ok, 1 the plugin faulted while processing
        double tailSeconds;
        uint64_t blocksDone;

        struct ParamChange { int32_t index; float value; };
        ParamChange params[maxParamChanges];
        uint8_t midiIn[midiAreaBytes];
        uint8_t midiOut[midiAreaBytes];
        float audio[maxChannels][maxBlock];
    };

    constexpr uint32_t sharedMagic = 0x55464f31;   // "UFO1"

    struct Names
    {
        juce::String memory, request, done;
    };

    // MIDI in the shared block: [sample position: int32][size: int32][bytes, padded to 4] ...
    inline int writeMidi (const juce::MidiBuffer& midi, int startSample, int numSamples, uint8_t* dest, int capacity)
    {
        int used = 0;
        for (const auto meta : midi)
        {
            if (meta.samplePosition < startSample || meta.samplePosition >= startSample + numSamples)
                continue;
            const int padded = (meta.numBytes + 3) & ~3;
            if (used + 8 + padded > capacity)
                break;
            const int32_t position = meta.samplePosition - startSample, size = meta.numBytes;
            std::memcpy (dest + used, &position, 4);
            std::memcpy (dest + used + 4, &size, 4);
            std::memcpy (dest + used + 8, meta.data, (size_t) meta.numBytes);
            used += 8 + padded;
        }
        return used;
    }

    inline void readMidi (const uint8_t* src, int bytes, int offset, juce::MidiBuffer& into)
    {
        int at = 0;
        while (at + 8 <= bytes)
        {
            int32_t position, size;
            std::memcpy (&position, src + at, 4);
            std::memcpy (&size, src + at + 4, 4);
            if (size <= 0 || at + 8 + ((size + 3) & ~3) > bytes)
                break;
            into.addEvent (src + at + 8, size, position + offset);
            at += 8 + ((size + 3) & ~3);
        }
    }

    // Bus layouts travel as text (the abbreviated speaker names); a set the text cannot give back is replaced by that many discrete channels.
    inline void writeLayout (juce::MemoryOutputStream& out, const juce::AudioProcessor::BusesLayout& layout)
    {
        for (auto* buses : { &layout.inputBuses, &layout.outputBuses })
        {
            out.writeInt (buses->size());
            for (auto& set : *buses)
            {
                out.writeInt (set.size());
                out.writeString (set.getSpeakerArrangementAsString());
            }
        }
    }

    inline juce::AudioProcessor::BusesLayout readLayout (juce::MemoryInputStream& in)
    {
        juce::AudioProcessor::BusesLayout layout;
        for (auto* buses : { &layout.inputBuses, &layout.outputBuses })
        {
            const int count = in.readInt();
            for (int i = 0; i < count; ++i)
            {
                const int channels = in.readInt();
                auto set = juce::AudioChannelSet::fromAbbreviatedString (in.readString());
                if (set.size() != channels)
                    set = juce::AudioChannelSet::discreteChannels (channels);
                buses->add (set);
            }
        }
        return layout;
    }
}
