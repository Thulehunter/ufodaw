#pragma once

// Tests of the CLAP hosting (Source/ClapFormat.*) with the small test plugin file of tests/clap (two plugins: a synth and a gain effect).

#include "Engine.h"
#include "ClapFormat.h"
#include "SfTests.h"

#ifdef TEST_CLAP_PATH
namespace ClapTests
{
using Report = SfTests::Report;

inline std::unique_ptr<juce::AudioPluginInstance> make (juce::AudioPluginFormatManager& manager, const juce::PluginDescription& d, juce::String& error)
{
    return manager.createPluginInstance (d, 48000.0, 512, error);
}

#if defined (_MSC_VER)
 __declspec (noinline)
#endif
inline bool run (const Report& report, int workers)
{
    bool all = true;
    const juce::File file (TEST_CLAP_PATH);
    ClapPluginFormat format;

    // ---- the scan
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, file.getFullPathName());
    const bool two = found.size() == 2;
    all &= report (two && found[0]->name == "UFODAW Test Synth" && found[0]->isInstrument && found[0]->numInputChannels == 0 && found[0]->numOutputChannels == 2
                   && found[1]->name == "UFODAW Test Gain" && ! found[1]->isInstrument && found[1]->numInputChannels == 2 && found[1]->pluginFormatName == "CLAP",
                   "CLAP: the scan finds both plugins of the file, their kind and channels");
    if (! two)
        return false;
    all &= report (format.searchPathsForPlugins (juce::FileSearchPath (file.getParentDirectory().getFullPathName()), false).contains (file.getFullPathName())
                       && format.fileMightContainThisPluginType (file.getFullPathName()) && format.doesPluginStillExist (*found[0]) && ! format.pluginNeedsRescanning (*found[0]),
                   "CLAP: the plugin folder is searched, the file is recognised");

    juce::AudioPluginFormatManager manager;
    manager.addFormat (new ClapPluginFormat());
    juce::String error;

    // ---- the instrument: notes, parameter, latency, state
    {
        auto synth = make (manager, *found[0], error);
        all &= report (synth != nullptr && synth->acceptsMidi() && synth->getParameters().size() == 1 && synth->getParameters()[0]->getName (50) == "Level", "CLAP: the instrument starts, takes notes, has its parameter " + error);
        if (synth == nullptr)
            return false;
        synth->setPlayConfigDetails (0, 2, 48000.0, 512);
        synth->prepareToPlay (48000.0, 512);
        all &= report (synth->getLatencySamples() == 64, "CLAP: the latency the plugin reports is passed on (" + juce::String (synth->getLatencySamples()) + ")");

        auto render = [&] (int note)
        {
            std::vector<float> out;
            juce::AudioBuffer<float> buffer (2, 512);
            for (int blk = 0; blk < 60; ++blk)
            {
                juce::MidiBuffer midi;
                if (blk == 0) midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 100);
                buffer.clear();
                synth->processBlock (buffer, midi);
                for (int i = 0; i < 512; ++i) out.push_back (buffer.getSample (0, i));
            }
            synth->reset();
            return out;
        };
        const auto a = render (69);
        const double f = SfTests::nearest (a, { 392.0, 415.3, 440.0, 466.2, 493.9 }, 0.2, 0.6);
        const double before = SfTests::rms (a, 0.2, 0.6);
        all &= report (f == 440.0 && before > 0.01, "CLAP: a note sounds at 440 Hz (rms " + juce::String (before, 3) + ")");

        auto* level = synth->getParameters()[0];
        level->setValue (1.0f);   // from 0.5: twice as loud
        const auto b = render (69);
        const double after = SfTests::rms (b, 0.2, 0.6);
        all &= report (after > 1.9 * before && after < 2.1 * before, "CLAP: a parameter change reaches the plugin (" + juce::String (after / before, 2) + " times as loud)");
        all &= report (level->getText (level->getValue(), 50) == "1.00 units" && std::abs (level->getValueForText ("0.25 units") - 0.25f) < 1.0e-3f, "CLAP: the plugin's own parameter text is used");

        level->setValue (0.8f);
        { juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m; synth->processBlock (b, m); }   // (the next block delivers it)
        juce::MemoryBlock state;
        synth->getStateInformation (state);
        auto copy = make (manager, *found[0], error);
        copy->prepareToPlay (48000.0, 512);
        copy->setStateInformation (state.getData(), (int) state.getSize());
        all &= report (state.getSize() == 4 && std::abs (copy->getParameters()[0]->getValue() - 0.8f) < 1.0e-3f, "CLAP: the state is saved and restored");
        synth->releaseResources();
    }

    // ---- the effect
    {
        auto gain = make (manager, *found[1], error);
        all &= report (gain != nullptr && ! gain->acceptsMidi() && gain->getTotalNumInputChannels() == 2, "CLAP: the effect starts with a stereo input " + error);
        if (gain == nullptr)
            return false;
        gain->prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        gain->getParameters()[0]->setValue (0.25f);   // 0..2: 0.5
        for (int i = 0; i < 2; ++i)   // (the second block: the value has arrived)
        {
            for (int c = 0; c < 2; ++c) for (int s = 0; s < 512; ++s) buffer.setSample (c, s, 0.8f);
            gain->processBlock (buffer, midi);
        }
        all &= report (std::abs (buffer.getSample (0, 300) - 0.4f) < 1.0e-5f && std::abs (buffer.getSample (1, 300) - 0.4f) < 1.0e-5f, "CLAP: the effect changes the audio (0.8 becomes " + juce::String (buffer.getSample (0, 300), 3) + ")");
    }

    // ---- on a track: serial and parallel mixing identical, the notes come out, the plugin's latency is compensated
    auto build = [&] (Engine& e)
    {
        auto* track = e.addTrack ("CLAP", Track::Type::Midi);
        auto* synthTrack = e.addTrack ("Synth", Track::Type::Midi);
        juce::String err;
        auto slot = std::make_shared<PluginSlot>();
        slot->instance = make (manager, *found[0], err);
        slot->description = *found[0];
        e.preparePlugin (*slot);
        track->instrument = slot;
        auto fx = std::make_shared<PluginSlot>();
        fx->instance = make (manager, *found[1], err);
        fx->description = *found[1];
        e.preparePlugin (*fx);
        track->inserts.push_back (fx);
        std::vector<Note> notes;
        for (int i = 0; i < 16; ++i) notes.push_back ({ 55 + (i * 5) % 17, i * 0.25, 0.2, 0.8f });
        for (auto* t : { track, synthTrack })
            e.setMidiNotes (*t, e.addMidiClip (*t, 0.0, 4.0), notes);
        return slot->instance != nullptr && fx->instance != nullptr;
    };
    auto mix = [&] (int n, std::vector<float>& out)
    {
        Engine e;
        e.prepareToPlay (512, 48000.0);
        const bool made = build (e);
        e.setWorkerThreads (n);
        e.forceParallel = n > 0;
        juce::AudioBuffer<float> b (2, 512);
        juce::AudioSourceChannelInfo info (&b, 0, 512);
        e.play();
        for (int blk = 0; blk < 300; ++blk)
        {
            b.clear();
            e.getNextAudioBlock (info);
            for (int i = 0; i < 512; ++i) { out.push_back (b.getSample (0, i)); out.push_back (b.getSample (1, i)); }
        }
        return made;
    };
    std::vector<float> serial, parallel;
    std::cout << "     (mixing on tracks...)" << std::endl;
    const bool m1 = mix (0, serial);
    std::cout << "     (serial done)" << std::endl;
    const bool m2 = mix (workers, parallel);
    float peak = 0;
    double worst = 0;
    for (size_t i = 0; i < parallel.size() && i < serial.size(); ++i) { peak = std::max (peak, std::abs (parallel[i])); worst = std::max (worst, (double) std::abs (parallel[i] - serial[i])); }
    all &= report (m1 && m2 && serial.size() == parallel.size() && worst < 1.0e-9 && peak > 0.05f, "CLAP: instrument and effect on tracks, serial and " + juce::String (workers) + " workers identical (peak " + juce::String (peak, 3) + ")");
    return all;
}
}
#endif
