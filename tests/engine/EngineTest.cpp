#include "Engine.h"
#include "Stretch.h"
#include <iostream>
#include <cmath>
#include <algorithm>

// Self-checking engine tests (no golden files, so they do not depend on the machine):
//  1. real-time: the engine is run block by block like the audio device does, serial and with worker threads: the output must be identical
//  2. offline:   the same project rendered to a file serial and with workers: identical
//  3. stretch:   a pitch shift with and without formant preservation gives audio of the right length that is not silent
// Run: EngineTest [workers]   (exit code 0 = all passed)

struct LatencyPlugin : public juce::AudioPluginInstance
{
    LatencyPlugin() : juce::AudioPluginInstance (BusesProperties().withInput ("in", juce::AudioChannelSet::stereo()).withOutput ("out", juce::AudioChannelSet::stereo())) { setLatencySamples (480); }
    const juce::String getName() const override { return "Latency"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override { b.applyGain (0.7f); }
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
    void fillInPluginDescription (juce::PluginDescription& d) const override { d.name = "Latency"; }
};

static juce::File makeTone (const juce::File& file, double freq, double seconds)
{
    juce::AudioBuffer<float> b (2, (int) (seconds * 48000));
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const float v = 0.4f * (float) (std::sin (6.2831853 * freq * i / 48000.0) * (0.5 + 0.5 * std::sin (6.2831853 * 3.0 * i / 48000.0)));
        b.setSample (0, i, v); b.setSample (1, i, v * 0.8f);
    }
    file.deleteFile();
    juce::WavAudioFormat f;
    std::unique_ptr<juce::AudioFormatWriter> w (f.createWriterFor (file.createOutputStream().release(), 48000.0, 2, 24, {}, 0));
    w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    return file;
}

static bool gHeavy = false;
static void buildProject (Engine& e, int tracksPerKind)
{
    const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto toneA = makeTone (tmp.getChildFile ("golden-a.wav"), 220.0, 6.0);
    const auto toneB = makeTone (tmp.getChildFile ("golden-b.wav"), 330.0, 6.0);

    auto* reverbBus = e.addTrack ("Reverb bus", Track::Type::Audio, true);
    reverbBus->fx.reverbOn = true; reverbBus->fx.reverbMix = 1.0f; reverbBus->fx.reverbDecay = 2.5f;
    auto* drumBus = e.addTrack ("Drum bus", Track::Type::Audio, true);
    drumBus->fx.compOn = true; drumBus->fx.compThreshold = -20.0f; drumBus->fx.compRatio = 4.0f;

    auto* drums = e.addDrumTrack ("Drums");
    {
        const int id = e.addMidiClip (*drums, 0.0, 6.0);
        std::vector<Note> notes;
        for (int bar = 0; bar < 3; ++bar)
            for (int i = 0; i < 16; ++i)
            {
                const double t = bar * 2.0 + i * 0.125;
                if (i % 4 == 0) notes.push_back ({ 36, t, 0.1, 0.95f });
                if (i % 8 == 4) notes.push_back ({ 38, t, 0.1, 0.9f });
                if (i % 2 == 0) notes.push_back ({ 42, t, 0.05, 0.6f });
            }
        e.setMidiNotes (*drums, id, notes);
    }
    e.setTrackOutput (*drums, drumBus);
    drums->fx.satOn = true; drums->fx.satDrive = 6.0f;

    for (int k = 0; k < tracksPerKind; ++k)
    {
        auto* lead = e.addTrack ("Lead " + juce::String (k), Track::Type::Midi);
        const int id = e.addMidiClip (*lead, 0.0, 6.0);
        std::vector<Note> notes;
        for (int i = 0; i < 24; ++i)
            notes.push_back ({ 55 + ((i * 5 + k * 3) % 17), i * 0.25, 0.22, 0.5f + 0.02f * (float) (i % 10) });
        e.setMidiNotes (*lead, id, notes);
        lead->synth.waveform = 1 + (k % 3);
        lead->synth.cutoff = 3000.0f + 500.0f * (float) k;
        lead->pan = k % 2 ? 0.4f : -0.4f;
        lead->fx.chorusOn = true; lead->fx.chorusMix = 0.4f;
        lead->fx.delayOn = (k % 2) == 0; lead->fx.delayMix = 0.25f;
        e.setSend (*lead, *reverbBus, 0.3f, k % 2 == 0);

        auto* loop = e.addTrack ("Loop " + juce::String (k), Track::Type::Audio);
        e.addClipFromFile (*loop, k % 2 ? toneA : toneB, 0.5 * k);
        loop->fx.eqOn = true; loop->fx.eqMid = 4.0f; loop->fx.eqMidFreq = 800.0f + 100.0f * (float) k;
        loop->fx.limOn = k % 2 == 1;
        loop->gain = 0.6f;
        e.setSend (*loop, *reverbBus, 0.2f, false);
        if (gHeavy)
            for (auto* tr : { lead, loop })
            {
                tr->fx.reverbOn = true; tr->fx.reverbMix = 0.2f;
                tr->fx.mbOn = true; tr->fx.satOn = true; tr->fx.satDrive = 9.0f; tr->fx.compOn = true; tr->fx.delayOn = true;
            }
    }

    auto* bass = e.addTrack ("Bass", Track::Type::Midi);
    {
        const int id = e.addMidiClip (*bass, 0.0, 6.0);
        std::vector<Note> notes;
        for (int i = 0; i < 12; ++i) notes.push_back ({ 36 + (i % 3) * 5, i * 0.5, 0.45, 0.8f });
        e.setMidiNotes (*bass, id, notes);
        bass->synth.waveform = 1; bass->synth.cutoff = 700.0f;
        bass->fx.compOn = true; bass->fx.compThreshold = -30.0f; bass->fx.compRatio = 8.0f;
        e.setSidechain (*bass, drums);
    }

    auto* plug = e.addTrack ("Plugin track", Track::Type::Audio);
    e.addClipFromFile (*plug, toneA, 1.0);
    auto slot = std::make_shared<PluginSlot>();
    slot->instance.reset (new LatencyPlugin());
    slot->description.name = "Latency";
    e.preparePlugin (*slot);
    plug->inserts.push_back (slot);

    auto& m = e.getMaster();
    m.fx.eqOn = true; m.fx.eqHigh = 2.0f;
    m.fx.limOn = true; m.fx.limCeiling = -2.0f;
}


static std::vector<float> readWav (const juce::File& f)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    std::vector<float> out;
    if (r == nullptr) return out;
    juce::AudioBuffer<float> b (2, (int) r->lengthInSamples);
    r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
    out.reserve ((size_t) b.getNumSamples() * 2);
    for (int i = 0; i < b.getNumSamples(); ++i) { out.push_back (b.getSample (0, i)); out.push_back (b.getSample (1, i)); }
    return out;
}

static double largestDifference (const std::vector<float>& a, const std::vector<float>& b)
{
    double worst = 0;
    for (size_t i = 0; i < std::min (a.size(), b.size()); ++i) worst = std::max (worst, (double) std::abs (a[i] - b[i]));
    return worst;
}

static bool report (bool ok, const juce::String& what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << "\n";
    return ok;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const int workers = juce::jmax (1, argc > 1 ? juce::String (argv[1]).getIntValue() : 3);
    std::cout << "cores: " << juce::SystemStats::getNumCpus() << ", workers: " << workers << "\n";
    bool all = true;

    // 1. real time, serial against parallel
    {
        auto run = [&] (int n, std::vector<float>& out)
        {
            Engine x; x.prepareToPlay (512, 48000.0);
            buildProject (x, 3);
            x.setWorkerThreads (n);
            x.forceParallel = n > 0;
            juce::AudioBuffer<float> b (2, 512); juce::AudioSourceChannelInfo info (&b, 0, 512);
            x.play();
            for (int blk = 0; blk < 500; ++blk) { b.clear(); x.getNextAudioBlock (info); for (int i = 0; i < 512; ++i) { out.push_back (b.getSample (0, i)); out.push_back (b.getSample (1, i)); } }
        };
        std::vector<float> serial, par;
        run (0, serial); run (workers, par);
        float peak = 0; for (auto v : par) peak = std::max (peak, std::abs (v));
        all &= report (serial.size() == par.size() && largestDifference (serial, par) < 1e-9 && peak > 0.1f,
                       "real time: serial and " + juce::String (workers) + " workers identical (peak " + juce::String (peak, 3) + ")");
    }

    // 2. offline, serial against parallel (a mix and a stem)
    {
        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory);
        auto render = [&] (int n, const juce::String& tag, std::vector<float>& mix, std::vector<float>& stem)
        {
            Engine e; e.prepareToPlay (512, 48000.0);
            buildProject (e, 3);
            e.setWorkerThreads (n);
            const auto m = tmp.getChildFile ("enginetest-mix-" + tag + ".wav"), s = tmp.getChildFile ("enginetest-stem-" + tag + ".wav");
            e.renderToFile (m, 6.0, false);
            Engine::RenderOptions opt; opt.onlyTrack = e.getTrack (e.getNumTracks() - 2); opt.masterProcessing = false;
            e.renderToFile (s, 6.0, opt);
            mix = readWav (m); stem = readWav (s);
            m.deleteFile(); s.deleteFile();
        };
        std::vector<float> m0, s0, m1, s1;
        render (0, "serial", m0, s0); render (workers, "parallel", m1, s1);
        all &= report (! m0.empty() && m0.size() == m1.size() && largestDifference (m0, m1) < 1e-9, "offline mix identical");
        all &= report (! s0.empty() && s0.size() == s1.size() && largestDifference (s0, s1) < 1e-9, "offline stem identical");
    }

    // 3. pitch shift with and without formants
    {
        juce::AudioBuffer<float> in (2, 48000);
        for (int i = 0; i < in.getNumSamples(); ++i)
        {
            const float v = 0.3f * (float) (std::sin (6.2831853 * 200.0 * i / 48000.0) + 0.5 * std::sin (6.2831853 * 600.0 * i / 48000.0));
            in.setSample (0, i, v); in.setSample (1, i, v);
        }
        for (bool formants : { false, true })
        {
            auto out = Stretch::process (in, 0, in.getNumSamples(), 48000.0, 1.0, 5.0, nullptr, formants);
            const float peak = out != nullptr ? out->getMagnitude (0, out->getNumSamples()) : 0.0f;
            all &= report (out != nullptr && std::abs (out->getNumSamples() - in.getNumSamples()) <= 2 && peak > 0.05f,
                           juce::String ("pitch +5 semitones, formants ") + (formants ? "kept" : "not kept") + ": length and level fine");
        }
    }

    // 4. clip launcher: a MIDI pattern and an audio loop are launched, loop, and stop at the next bar
    {
        Engine e; e.prepareToPlay (512, 48000.0);
        const auto tone = makeTone (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("enginetest-tone.wav"), 220.0, 3.0);

        auto* synth = e.addTrack ("Synth", Track::Type::Midi);
        const int midiClip = e.addMidiClip (*synth, 0.0, 2.0);   // one bar at 120 BPM
        e.setMidiNotes (*synth, midiClip, { { 60, 0.0, 0.4, 0.9f }, { 64, 0.5, 0.4, 0.9f }, { 67, 1.0, 0.4, 0.9f }, { 72, 1.5, 0.4, 0.9f } });
        const bool slotOk = e.setSlotFromClip (*synth, 0, midiClip, true);

        auto* loop = e.addTrack ("Loop", Track::Type::Audio);
        const bool fileOk = e.setSlotFromFile (*loop, 1, tone);

        // the slots are the only thing that sounds: the clip left the timeline
        e.launchQuantum = (int) Engine::LaunchQuantum::Bar;
        e.launchScene (0);   // (stopped transport: starts at once)
        e.launchSlot (*loop, 1);

        std::vector<float> out;
        juce::AudioBuffer<float> b (2, 512); juce::AudioSourceChannelInfo info (&b, 0, 512);
        bool stopped = false;
        for (int blk = 0; blk < 750; ++blk)   // 8 seconds
        {
            if (blk == 280 && ! stopped) { e.stopTrackSlots (*synth); stopped = true; }   // at about 3 s: ends at 4 s
            b.clear(); e.getNextAudioBlock (info);
            for (int i = 0; i < 512; ++i) out.push_back (b.getSample (0, i));
        }
        auto rms = [&] (double from, double to)
        {
            double sum = 0; int count = 0;
            for (size_t i = (size_t) (from * 48000); i < (size_t) (to * 48000) && i < out.size(); ++i) { sum += (double) out[i] * out[i]; ++count; }
            return std::sqrt (sum / juce::jmax (1, count));
        };
        const double early = rms (0.2, 1.8), later = rms (2.2, 3.8), afterStop = rms (4.3, 7.5);
        all &= report (slotOk && fileOk, "launcher: slots filled");
        all &= report (early > 0.01 && later > 0.01, "launcher: the pattern and the loop play and repeat (" + juce::String (early, 3) + ", " + juce::String (later, 3) + ")");
        // after 4 s only the audio loop is left (it is a 3 s tone that repeats)
        all &= report (afterStop > 0.01, "launcher: the audio loop goes on after the pattern stopped (" + juce::String (afterStop, 3) + ")");
        auto st = e.getSlotState (*synth);
        all &= report (st.playing == -1 && st.queued == -1, "launcher: the stopped pattern is no longer playing");
        e.stop();
    }

    std::cout << (all ? "ALL PASSED\n" : "FAILED\n");
    return all ? 0 : 1;
}
