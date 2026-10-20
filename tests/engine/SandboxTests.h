#pragma once

// Tests of "plugins in a separate process" (Source/PluginProxy.h): the test plugin (Source/TestPlugin.h) runs in a helper process (UFODAW.exe) and
// must give exactly the same sound and MIDI as the same plugin inside this program; a helper that is killed, crashes or hangs must not hurt.
// Windows only. Needs UFODAW.exe (found next to the build folders, or named by the environment variable UFODAW_HELPER).

#include "PluginProxy.h"
#include "TestPlugin.h"
#include "Engine.h"
#include <thread>

namespace SandboxTests
{
    inline juce::File findHelper()
    {
        const auto env = juce::SystemStats::getEnvironmentVariable ("UFODAW_HELPER", {});
        if (env.isNotEmpty() && juce::File (env).existsAsFile())
            return juce::File (env);
        auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();   // build/EngineTest_artefacts/Release
        for (int up = 0; up < 4; ++up, dir = dir.getParentDirectory())
            for (auto* sub : { "UFODAW_artefacts/Release/UFODAW.exe", "UFODAW_artefacts/Debug/UFODAW.exe" })
                if (dir.getChildFile (sub).existsAsFile())
                    return dir.getChildFile (sub);
        return {};
    }

    inline void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

    inline bool sameBits (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
            return false;
        for (int c = 0; c < a.getNumChannels(); ++c)
            if (std::memcmp (a.getReadPointer (c), b.getReadPointer (c), (size_t) a.getNumSamples() * sizeof (float)) != 0)
                return false;
        return true;
    }

    inline bool sameMidi (const juce::MidiBuffer& a, const juce::MidiBuffer& b)
    {
        auto ia = a.begin(), ib = b.begin();
        for (; ia != a.end() && ib != b.end(); ++ia, ++ib)
        {
            const auto ma = *ia, mb = *ib;
            if (ma.samplePosition != mb.samplePosition || ma.numBytes != mb.numBytes || std::memcmp (ma.data, mb.data, (size_t) ma.numBytes) != 0)
                return false;
        }
        return ia == a.end() && ib == b.end();
    }

    // Deterministic input: noise and a few notes at awkward positions
    inline void fillInput (juce::AudioBuffer<float>& b, juce::Random& rng)
    {
        for (int c = 0; c < b.getNumChannels(); ++c)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (c, i, rng.nextFloat() * 0.2f - 0.1f);
    }

    template <typename Report>
    bool run (const juce::File& helper, Report&& report)
    {
        bool all = true;
        PluginProxy::helperExecutable = helper;

        // 1. the same input through the plugin inside the program and through the proxy: sample for sample the same, MIDI too
        for (const juce::String mode : { "synth", "echo" })
        {
            TestPlugin direct (mode);
            juce::String error;
            auto proxy = PluginProxy::create (TestPlugin::describe (mode), 48000.0, 512, error);
            if (! report (proxy != nullptr, "sandbox: the plugin starts in a helper process " + error))
                return false;

            direct.prepareToPlay (48000.0, 512);
            proxy->prepareToPlay (48000.0, 512);
            bool audioSame = true, midiSame = true, sizesOk = true;
            juce::Random rng (42);
            const int sizes[] = { 512, 512, 100, 1, 37, 512, 9000, 64, 512, 256 };   // (9000 is longer than the shared block: sent in pieces)
            int blockNumber = 0;
            for (int rep = 0; rep < 30; ++rep)
                for (int size : sizes)
                {
                    juce::AudioBuffer<float> a (2, size), b (2, size);
                    fillInput (a, rng);
                    b.makeCopyOf (a);
                    juce::MidiBuffer ma, mb;
                    if (blockNumber % 3 == 0)   // notes, some in the middle of a block, off after a while
                    {
                        const int note = 40 + (blockNumber * 7) % 40;
                        ma.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), size / 3);
                        ma.addEvent (juce::MidiMessage::noteOff (1, note), size - 1);
                    }
                    mb = ma;
                    direct.processBlock (a, ma);
                    proxy->processBlock (b, mb);
                    audioSame = audioSame && sameBits (a, b);
                    midiSame = midiSame && sameMidi (ma, mb);
                    ++blockNumber;
                }
            sizesOk = ! proxy->hasFailed();
            report (audioSame, "sandbox (" + mode + "): " + juce::String (blockNumber) + " blocks of audio are bit for bit the same as in the program");
            report (midiSame, "sandbox (" + mode + "): MIDI out is the same");
            all = all && audioSame && midiSame && sizesOk;
            if (mode == "synth")
            {
                all &= report (proxy->getLatencySamples() == direct.getLatencySamples() && proxy->getLatencySamples() == 64, "sandbox: latency is passed on (" + juce::String (proxy->getLatencySamples()) + " samples)");
                all &= report (proxy->getParameters().size() == 2 && proxy->getParameters()[0]->getName (20) == "Gain", "sandbox: the parameters are there");
                all &= report (proxy->acceptsMidi() && ! proxy->producesMidi() && proxy->getTotalNumOutputChannels() == 2, "sandbox: buses and MIDI flags are the same");

                // parameters: changed from another thread (like the audio thread / automation does) they apply at the next block, exactly
                direct.getParameters()[0]->setValue (0.3f);
                std::thread ([&] { proxy->getParameters()[0]->setValue (0.3f); }).join();
                juce::AudioBuffer<float> a (2, 512), b (2, 512);
                fillInput (a, rng);
                b.makeCopyOf (a);
                juce::MidiBuffer m1, m2;
                direct.processBlock (a, m1);
                proxy->processBlock (b, m2);
                all &= report (sameBits (a, b), "sandbox: a parameter change travels with the next audio block");

                // from the message thread through the pipe, and the state
                proxy->getParameters()[0]->setValue (0.25f);
                juce::MemoryBlock state;
                proxy->getStateInformation (state);
                float first = 0.0f;
                if (state.getSize() >= 4)
                    std::memcpy (&first, state.getData(), 4);
                all &= report (first == 0.25f, "sandbox: a parameter set on the message thread is in the plugin's state");

                juce::MemoryBlock directState;
                direct.getParameters()[0]->setValue (0.25f);
                direct.getStateInformation (directState);
                all &= report (directState == state, "sandbox: the state is the same bytes as in the program (projects stay compatible both ways)");

                direct.stateMarker = 77;
                direct.getStateInformation (directState);
                proxy->setStateInformation (directState.getData(), (int) directState.getSize());
                juce::MemoryBlock back;
                proxy->getStateInformation (back);
                all &= report (back == directState, "sandbox: a state set through the proxy comes back");
                all &= report (proxy->getParameters()[0]->getValue() == 0.25f, "sandbox: parameter values follow the loaded state");
                all &= report (proxy->getParameters()[0]->getText (0.5f, 10).isNotEmpty(), "sandbox: parameter text comes from the plugin");

                // the editor: a window of the helper process
                proxy->showEditor ("Test editor");
                pump (700);
                const bool opened = proxy->isEditorOpen();
                proxy->closeEditor();
                pump (200);
                all &= report (opened && ! proxy->isEditorOpen() && ! proxy->hasFailed(), "sandbox: the editor window opens and closes in the helper");

                juce::int64 bytes = 0; int gdi = 0, user = 0;
                const bool stats = proxy->getHelperStats (bytes, gdi, user);
                all &= report (stats && bytes > 1000000, "sandbox: the helper reports its memory (" + juce::String (bytes / 1048576) + " MB) and GDI objects (" + juce::String (gdi) + ")");
            }
        }

        // 2. inside an engine: an instrument track renders the same with the plugin in the program and in a helper
        {
            auto render = [&] (bool sandboxed, std::vector<float>& out)
            {
                Engine e; e.prepareToPlay (512, 48000.0);
                auto* t = e.addTrack ("Test", Track::Type::Midi);
                const int clip = e.addMidiClip (*t, 0.0, 4.0);
                std::vector<Note> notes;
                for (int i = 0; i < 16; ++i) notes.push_back ({ 48 + (i * 5) % 24, i * 0.25, 0.2, 0.5f + 0.03f * (float) i });
                e.setMidiNotes (*t, clip, notes);
                auto slot = std::make_shared<PluginSlot>();
                slot->description = TestPlugin::describe ("synth");
                juce::String error;
                if (sandboxed)
                {
                    auto proxy = PluginProxy::create (slot->description, 48000.0, 512, error);
                    if (proxy == nullptr)
                        return false;
                    slot->remote = proxy.get();
                    slot->instance = std::move (proxy);
                }
                else
                    slot->instance = std::make_unique<TestPlugin> ("synth");
                e.preparePlugin (*slot);
                e.setInstrument (*t, slot);
                t->fx.eqOn = false;
                juce::AudioBuffer<float> b (2, 512); juce::AudioSourceChannelInfo info (&b, 0, 512);
                e.play();
                for (int blk = 0; blk < 300; ++blk)
                {
                    b.clear(); e.getNextAudioBlock (info);
                    for (int i = 0; i < 512; ++i) { out.push_back (b.getSample (0, i)); out.push_back (b.getSample (1, i)); }
                }
                e.stop();
                return ! slot->crashed.load();
            };
            std::vector<float> inside, outside;
            const bool a = render (false, inside), b = render (true, outside);
            float peak = 0; for (auto v : inside) peak = std::max (peak, std::abs (v));
            all &= report (a && b && inside.size() == outside.size() && peak > 0.05f
                               && std::memcmp (inside.data(), outside.data(), inside.size() * sizeof (float)) == 0,
                           "sandbox: an instrument track in the engine renders bit for bit the same in a helper (peak " + juce::String (peak, 3) + ")");
        }

        // 3. the helper is ended from outside in the middle of the sound: the program goes on, the plugin is marked, the last state is kept, and it restarts
        {
            juce::String error;
            auto proxy = PluginProxy::create (TestPlugin::describe ("synth"), 48000.0, 512, error);
            bool failedCalled = false;
            proxy->onFailed = [&] { failedCalled = true; };
            proxy->prepareToPlay (48000.0, 512);
            proxy->getParameters()[1]->setValue (0.9f);
            juce::MemoryBlock keptState;
            proxy->getStateInformation (keptState);   // (the program saves now and then: this is what survives)

            juce::AudioBuffer<float> b (2, 512);
            juce::Random rng (7);
            for (int i = 0; i < 20; ++i)
            {
                juce::MidiBuffer m;
                if (i == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                fillInput (b, rng);
                proxy->processBlock (b, m);
            }
            const float before = b.getMagnitude (0, 512);

            proxy->killHelper();
            double worstMs = 0;
            bool silent = true;
            for (int i = 0; i < 20; ++i)
            {
                juce::MidiBuffer m;
                fillInput (b, rng);
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                proxy->processBlock (b, m);
                worstMs = std::max (worstMs, juce::Time::getMillisecondCounterHiRes() - t0);
                if (proxy->hasFailed())
                    silent = silent && b.getMagnitude (0, 512) == 0.0f;
            }
            pump (600);
            all &= report (before > 0.01f && proxy->hasFailed() && silent && worstMs < 100.0,
                           "sandbox: helper killed during playback: silence at once, no block took longer than " + juce::String (worstMs, 1) + " ms");
            all &= report (failedCalled && proxy->failureReason().isNotEmpty(), "sandbox: the program is told (\"" + proxy->failureReason() + "\")");

            juce::MemoryBlock stateNow;
            proxy->getStateInformation (stateNow);
            all &= report (stateNow == keptState && stateNow.getSize() > 0, "sandbox: saving a project still works with the last known state");

            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            const bool restarted = proxy->restart (error);
            const double restartMs = juce::Time::getMillisecondCounterHiRes() - t0;
            juce::MemoryBlock afterRestart;
            proxy->getStateInformation (afterRestart);
            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 0);
            for (int i = 0; i < 4; ++i) { fillInput (b, rng); proxy->processBlock (b, m); m.clear(); }
            all &= report (restarted && ! proxy->hasFailed() && afterRestart == keptState && b.getMagnitude (0, 512) > 0.01f,
                           "sandbox: restart gives the plugin back with its settings and it sounds again (" + juce::String (restartMs, 0) + " ms)");
        }

        // 4. a plugin that crashes (access violation) and one that hangs
        for (const juce::String mode : { "crash", "hang" })
        {
            juce::String error;
            auto proxy = PluginProxy::create (TestPlugin::describe (mode), 48000.0, 512, error);
            proxy->prepareToPlay (48000.0, 512);
            const int oldTimeout = PluginProxy::blockTimeoutMs.load();
            PluginProxy::blockTimeoutMs = 400;
            juce::AudioBuffer<float> b (2, 512);
            juce::Random rng (3);
            double worstMs = 0;
            int blocks = 0;
            for (; blocks < 100 && ! proxy->hasFailed(); ++blocks)
            {
                juce::MidiBuffer m;
                fillInput (b, rng);
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                proxy->processBlock (b, m);
                worstMs = std::max (worstMs, juce::Time::getMillisecondCounterHiRes() - t0);
            }
            PluginProxy::blockTimeoutMs = oldTimeout;
            pump (300);
            all &= report (proxy->hasFailed() && blocks <= 45 && b.getMagnitude (0, 512) == 0.0f && worstMs < 1000.0,
                           "sandbox: a plugin that " + juce::String (mode == "crash" ? "crashes" : "hangs") + " is stopped after block " + juce::String (blocks)
                               + " (the longest block took " + juce::String (worstMs, 0) + " ms) and the program is unharmed");
        }

        // 5. what it costs: time for one block of 512 samples, through the helper and inside the program
        {
            juce::String error;
            auto proxy = PluginProxy::create (TestPlugin::describe ("synth"), 48000.0, 512, error);
            TestPlugin direct ("synth");
            proxy->prepareToPlay (48000.0, 512);
            direct.prepareToPlay (48000.0, 512);
            juce::AudioBuffer<float> b (2, 512);
            juce::Random rng (1);
            juce::MidiBuffer m;
            for (int i = 0; i < 200; ++i) { fillInput (b, rng); proxy->processBlock (b, m); }   // (warm up)
            proxy->resetTiming();
            const int n = 4000;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            for (int i = 0; i < n; ++i) proxy->processBlock (b, m);
            const double proxyUs = (juce::Time::getMillisecondCounterHiRes() - t0) * 1000.0 / n;
            const auto t1 = juce::Time::getMillisecondCounterHiRes();
            for (int i = 0; i < n; ++i) direct.processBlock (b, m);
            const double directUs = (juce::Time::getMillisecondCounterHiRes() - t1) * 1000.0 / n;
            const auto timing = proxy->getTiming();
            std::cout << "     per block of 512 samples at 48 kHz (10.7 ms): inside the program " << juce::String (directUs, 1) << " us, in a helper "
                      << juce::String (proxyUs, 1) << " us (round trip on the audio thread: average " << juce::String (timing.averageMicros, 1)
                      << " us, worst " << juce::String (timing.worstMicros, 0) << " us); extra " << juce::String (proxyUs - directUs, 1) << " us = "
                      << juce::String ((proxyUs - directUs) / 10667.0 * 100.0, 2) << " % of one block\n";
            all &= report (proxyUs - directUs < 1500.0, "sandbox: the extra time per block is small");
        }
        return all;
    }
}
