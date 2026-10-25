#include "Engine.h"
#include "Stretch.h"
#include "SynthPresets.h"
#include "StarterPack.h"
#include "SfTests.h"
#include "Wavetable.h"
#include "SynthPanel.h"
#include "ClapTests.h"
#include <iostream>
#if JUCE_WINDOWS
 #include "SandboxTests.h"
#endif
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
    std::cout << (ok ? "ok   " : "FAIL ") << what << std::endl;
    return ok;
}

// The wavetable engine: table 0 starts as a sine, moves to a saw and a square; a high note has no aliasing; the position changes the sound.
#if defined (_MSC_VER)
 __declspec (noinline)
#endif
static bool wavetableTests (int workers)
{
    const double rate = 48000.0;
    auto play = [&] (int note, double pos, int table)
    {
        Synth synth; synth.prepare (rate);
        SynthParams params; params.engine = 2; params.wtTable = table; params.wtPos = (float) pos; params.attack = 0.002f; params.cutoff = 18000.0f; params.sustain = 1.0f;
        std::vector<float> l ((size_t) rate), r (l.size());
        synth.noteOn (note, 0.8f, params);
        for (size_t done = 0; done < l.size(); done += 512) synth.render (l.data() + done, r.data() + done, (int) std::min<size_t> (512, l.size() - done), params);
        return l;
    };
    auto mag = [&] (const std::vector<float>& x, double hz) { return SfTests::magnitude (x, hz, 0.2, 0.9); };
    bool ok = true;

    const auto sine = play (69, 0.0, 0);
    ok &= report (mag (sine, 440.0) > 20.0 * mag (sine, 880.0) && mag (sine, 440.0) > 0.05, "wavetable: the first frame of the basic table is a sine at 440 Hz");
    const auto saw = play (69, 2.0 / 3.0, 0);
    const double sawRatio = mag (saw, 880.0) / mag (saw, 440.0);
    ok &= report (sawRatio > 0.4 && sawRatio < 0.6 && mag (saw, 1320.0) / mag (saw, 440.0) > 0.25, "wavetable: two thirds in the basic table is a saw (harmonics 1/n: " + juce::String (sawRatio, 2) + ")");
    const auto square = play (69, 1.0, 0);
    ok &= report (mag (square, 880.0) < 0.05 * mag (square, 440.0) && mag (square, 1320.0) / mag (square, 440.0) > 0.25, "wavetable: the last frame is a square (odd harmonics only)");
    const auto high = play (100, 2.0 / 3.0, 0);   // 2637 Hz: a saw without band limiting would fold back below it
    const double fundamental = mag (high, 440.0 * std::pow (2.0, 31.0 / 12.0));
    double worst = 0.0;
    for (double f : { 1000.0, 1500.0, 1850.0, 3100.0, 4200.0 }) worst = std::max (worst, mag (high, f));
    ok &= report (fundamental > 0.01 && worst < 0.05 * fundamental, "wavetable: a high note is band-limited (fundamental " + juce::String (fundamental, 3) + ", alias " + juce::String (worst / fundamental, 3) + " of it)");
    double different = 0.0;
    const auto a = play (60, 0.0, 1), b = play (60, 1.0, 1);
    for (size_t i = 4800; i < 24000; ++i) different += std::abs (a[i] - b[i]);
    ok &= report (different > 50.0, "wavetable: the position changes the sound");
    bool allTables = true;
    for (int t = 0; t < Wavetable::numSets; ++t)
    {
        const auto x = play (57, 0.5, t);
        float peak = 0; for (auto v : x) { if (! std::isfinite (v)) peak = 1e9f; peak = std::max (peak, std::abs (v)); }
        allTables = allTables && peak > 0.02f && peak < 1.0f;
    }
    ok &= report (allTables, "wavetable: every table sounds, finite and not clipping");
    // the size of the tables
    ok &= report (Wavetable::numFrames >= 64 && Wavetable::size == 2048 && Wavetable::numSets >= 12, "wavetable: " + juce::String (Wavetable::numSets) + " tables of " + juce::String (Wavetable::numFrames) + " waves of 2048 samples");

    // the new tables do what their name says
    {
        const double f0 = 440.0 * std::pow (2.0, -9.0 / 12.0);   // note 60
        auto rising0 = play (60, 0.1, 9), rising1 = play (60, 1.0, 9);
        ok &= report (mag (rising1, f0 * 40.0) > 5.0 * mag (rising0, f0 * 40.0) && mag (rising1, f0 * 40.0) > 0.0003, "wavetable: the rising table gains overtones with the position (" + juce::String (mag (rising1, f0 * 40.0), 5) + " against " + juce::String (mag (rising0, f0 * 40.0), 5) + ")");
        auto fold0 = play (60, 0.0, 6), fold1 = play (60, 1.0, 6);
        ok &= report (mag (fold0, f0) > 20.0 * mag (fold0, f0 * 5.0) && mag (fold1, f0 * 5.0) > 0.1 * mag (fold1, f0), "wavetable: the wavefold table goes from a sine to a rich wave");
        const auto sat0 = play (60, 0.0, 7), sat1 = play (60, 1.0, 7);
        ok &= report (mag (sat1, f0 * 3.0) > 2.0 * mag (sat0, f0 * 3.0), "wavetable: the saturated table gets more harmonics with the position");
    }

    // no table aliases on a high note (the energy between the harmonics stays low)
    {
        const double fundamental = 440.0 * std::pow (2.0, 31.0 / 12.0);   // note 100
        bool clean = true;
        juce::String dirty;
        for (int table : { 1, 6, 7, 9, 10, 11, 12, 13 })
            for (double pos : { 0.5, 1.0 })
            {
                const auto x = play (100, pos, table);
                const double main = mag (x, fundamental);
                double worst = 0.0;
                for (double f : { 1000.0, 1500.0, 1850.0, 3100.0, 4200.0 }) worst = std::max (worst, mag (x, f));
                if (main > 1.0e-4 && worst > 0.05 * main) { clean = false; dirty << " " << table << "@" << juce::String (pos, 1); }
            }
        ok &= report (clean, "wavetable: high notes are clean in every table" + (dirty.isEmpty() ? juce::String() : " (aliasing in:" + dirty + ")"));
    }

    // the velocity moves the position
    {
        auto playVelocity = [&] (float velocity)
        {
            Synth synth; synth.prepare (rate);
            SynthParams params; params.engine = 2; params.wtTable = 6; params.wtPos = 0.5f; params.wtVel = 1.0f; params.attack = 0.002f; params.cutoff = 18000.0f; params.sustain = 1.0f;
            std::vector<float> l ((size_t) rate / 2), r (l.size());
            synth.noteOn (60, velocity, params);
            for (size_t done = 0; done < l.size(); done += 512) synth.render (l.data() + done, r.data() + done, (int) std::min<size_t> (512, l.size() - done), params);
            return l;
        };
        const auto soft = playVelocity (0.2f), hard = playVelocity (1.0f);
        const double f0 = 440.0 * std::pow (2.0, -9.0 / 12.0);
        ok &= report (SfTests::magnitude (hard, f0 * 6.0, 0.1, 0.4) / SfTests::magnitude (hard, f0, 0.1, 0.4) != SfTests::magnitude (soft, f0 * 6.0, 0.1, 0.4) / SfTests::magnitude (soft, f0, 0.1, 0.4),
                      "wavetable: the velocity changes the position");
    }

    // the same wavetable sounds every time (deterministic tables)
    ok &= report (play (64, 0.37, 10) == play (64, 0.37, 10), "wavetable: the same notes give the same samples");

    // tracks with the engine: serial and parallel mixing identical, and the engine's settings survive a captured state
    {
        auto build = [] (Engine& x)
        {
            const int tables[3] = { 2, 11, 13 };
            for (int i = 0; i < 3; ++i)
            {
                auto* t = x.addTrack ("Wavetable " + juce::String (i), Track::Type::Midi);
                t->synth.engine = 2; t->synth.wtTable = tables[i]; t->synth.wtPos = 0.2f + 0.3f * (float) i; t->synth.wtLfo = 0.6f; t->synth.wtEnv = 0.4f - 0.4f * (float) i; t->synth.wtVel = 0.5f;
                t->synth.unison = 1 + 2 * i; t->synth.lfoRate = 0.7f; t->synth.attack = 0.01f; t->synth.cutoff = 6000.0f; t->synth.filterEnv = 1.0f;
                std::vector<Note> notes;
                for (int n = 0; n < 16; ++n) notes.push_back ({ 40 + 12 * i + (n * 5) % 19, n * 0.25, 0.4, 0.4f + 0.04f * (float) (n % 10) });
                x.setMidiNotes (*t, x.addMidiClip (*t, 0.0, 5.0), notes);
            }
        };
        auto run = [&] (int n, std::vector<float>& out)
        {
            Engine x; x.prepareToPlay (512, 48000.0);
            build (x);
            x.setWorkerThreads (n);
            x.forceParallel = n > 0;
            juce::AudioBuffer<float> b (2, 512); juce::AudioSourceChannelInfo info (&b, 0, 512);
            x.play();
            for (int blk = 0; blk < 400; ++blk) { b.clear(); x.getNextAudioBlock (info); for (int i = 0; i < 512; ++i) { out.push_back (b.getSample (0, i)); out.push_back (b.getSample (1, i)); } }
        };
        std::vector<float> serial, par;
        run (0, serial); run (workers, par);
        float peak = 0; for (auto v : par) peak = std::max (peak, std::abs (v));
        ok &= report (serial.size() == par.size() && largestDifference (serial, par) < 1e-9 && peak > 0.05f,
                      "wavetable tracks: serial and " + juce::String (workers) + " workers identical (peak " + juce::String (peak, 3) + ")");

        [&]   // (a lambda: the engine would add to the stack frame of main)
        {
            Engine x; x.prepareToPlay (512, 48000.0);
            build (x);
            const auto saved = x.captureState();
            x.getTrack (1)->synth.engine = 0; x.getTrack (1)->synth.wtTable = 0; x.getTrack (1)->synth.wtVel = 0.0f;
            x.restoreState (saved);
            const auto& s = x.getTrack (1)->synth;
            ok &= report (s.engine.load() == 2 && s.wtTable.load() == 11 && std::abs (s.wtVel.load() - 0.5f) < 1.0e-6f && std::abs (s.wtPos.load() - 0.5f) < 1.0e-6f,
                          "wavetable tracks: the engine, table, position and velocity amount come back with the state");
        }();
    }
    return ok;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
   #if JUCE_WINDOWS
    if (argc > 1 && juce::String (argv[1]) == "sandbox")   // only the tests of plugins in a separate process (EngineTest sandbox)
    {
        const auto helper = SandboxTests::findHelper();
        const bool ok = helper.existsAsFile() && SandboxTests::run (helper, [] (bool ok, const juce::String& what) { return report (ok, what); });
        std::cout << (ok ? "ALL PASSED\n" : "FAILED\n");
        return ok ? 0 : 1;
    }
   #endif
    if (argc > 2 && juce::String (argv[1]) == "sfgen")   // writes the test SoundFont and SFZ into a folder (tests/projects/sf)
    {
        const juce::File dir (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
        SfTests::writeSf2 (dir.getChildFile ("test.sf2"));
        SfTests::writeSfz (dir);
        std::cout << "written to " << dir.getFullPathName() << std::endl;
        return 0;
    }
   #ifdef TEST_CLAP_PATH
    if (argc > 1 && juce::String (argv[1]) == "clap")   // only the CLAP tests
    {
        const bool ok = ClapTests::run ([] (bool ok, const juce::String& what) { return report (ok, what); }, 3);
        std::cout << (ok ? "ALL PASSED" : "FAILED") << std::endl;
        return ok ? 0 : 1;
    }
   #endif
    if (argc > 1 && juce::String (argv[1]) == "sf")   // only the SoundFont / SFZ tests
    {
        const bool ok = SfTests::run ([] (bool ok, const juce::String& what) { return report (ok, what); }, juce::File::getSpecialLocation (juce::File::tempDirectory));
        std::cout << (ok ? "ALL PASSED" : "FAILED") << std::endl;
        return ok ? 0 : 1;
    }
    if (argc > 2 && juce::String (argv[1]) == "snapshot")   // draws the synth window of a wavetable sound to a PNG (a look at the UI without opening the program)
    {
        Engine x; x.prepareToPlay (512, 48000.0);
        auto* t = x.addTrack ("Snap", Track::Type::Midi);
        t->synth.apply (SynthPresets::find (argc > 3 ? argv[3] : "WT Fold lead"));
        SynthPanel panel (*t, [] {}, [] {});
        panel.setBounds (0, 0, panel.getWidth(), panel.getHeight());
        const auto image = panel.createComponentSnapshot (panel.getLocalBounds(), true, 1.0f);
        juce::File out (argv[2]); out.deleteFile();
        juce::FileOutputStream stream (out);
        juce::PNGImageFormat().writeImageToStream (image, stream);
        return 0;
    }
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

    // 5. the synth: every preset plays a chord; the sound must be finite, audible, not too loud, and stereo where it has unison
    {
        const double rate = 48000.0;
        bool allFine = true;
        double loudest = 0.0, quietest = 1.0;
        juce::String worst;
        for (auto& preset : SynthPresets::all())
        {
            Synth synth; synth.prepare (rate);
            SynthParams params; params.apply (preset.values);
            const int total = (int) (rate * 3.0), hold = (int) (rate * 1.5);
            std::vector<float> l ((size_t) total, 0.0f), r ((size_t) total, 0.0f);
            const std::vector<int> chord = preset.values.mono ? std::vector<int> { 48 } : std::vector<int> { 48, 52, 55 };   // (a mono sound plays one note at a time)
            for (int p : chord) synth.noteOn (p, 0.8f, params);
            int done = 0;
            while (done < total)
            {
                const int n = std::min (512, total - done);
                if (done <= hold && done + n > hold) for (int p : chord) synth.noteOff (p);
                synth.render (l.data() + done, r.data() + done, n, params);
                done += n;
            }
            double peak = 0, sumSq = 0, diff = 0; bool finite = true;
            for (int i = 0; i < total; ++i)
            {
                finite = finite && std::isfinite (l[(size_t) i]) && std::isfinite (r[(size_t) i]);
                peak = std::max ({ peak, (double) std::abs (l[(size_t) i]), (double) std::abs (r[(size_t) i]) });
                sumSq += (double) l[(size_t) i] * l[(size_t) i];
                diff += std::abs (l[(size_t) i] - r[(size_t) i]);
            }
            const double rms = std::sqrt (sumSq / total);
            const bool ok = finite && peak > 0.02 && peak < 1.2 && rms > 0.002 && (preset.values.unison == 1 || diff > 1.0);
            if (argc > 2) std::cout << "     peak " << peak << "  " << preset.name << "  (level " << preset.values.level << ")\n";
            if (! ok) std::cout << "     " << preset.name << ": peak " << peak << ", rms " << rms << ", finite " << finite << ", L-R " << diff << "\n";
            allFine = allFine && ok;
            if (peak > loudest) { loudest = peak; worst = preset.name; }
            quietest = std::min (quietest, peak);
        }
        all &= report (allFine, "synth: " + juce::String ((int) SynthPresets::all().size()) + " presets are finite, audible and not clipping (loudest peak "
                                    + juce::String (loudest, 2) + " in " + worst + ", quietest " + juce::String (quietest, 2) + ")");
    }

    // 6. the piano engine: the note has the right pitch and dies away; a mono sound with legato stays one voice
    {
        const double rate = 48000.0;
        Synth synth; synth.prepare (rate);
        SynthParams params; params.apply (SynthPresets::find ("Grand piano"));
        const int total = (int) (rate * 3.0);
        std::vector<float> l ((size_t) total, 0.0f), r ((size_t) total, 0.0f);
        synth.noteOn (69, 0.8f, params);
        int done = 0;
        while (done < total) { const int n = std::min (512, total - done); synth.render (l.data() + done, r.data() + done, n, params); done += n; }
        auto magnitudeAt = [&] (double hz, double from, double to)
        {
            double re = 0, im = 0;
            const size_t a = (size_t) (from * rate), b = (size_t) (to * rate);
            for (size_t i = a; i < b; ++i) { const double ph = 6.283185307 * hz * (double) i / rate; re += l[i] * std::cos (ph); im += l[i] * std::sin (ph); }
            return std::sqrt (re * re + im * im) / (double) (b - a);
        };
        const double at440 = magnitudeAt (440.0, 0.2, 0.7), off = std::max (magnitudeAt (400.0, 0.2, 0.7), magnitudeAt (480.0, 0.2, 0.7));
        auto rmsOf = [&] (double from, double to) { double s = 0; const size_t a = (size_t) (from * rate), b = (size_t) (to * rate); for (size_t i = a; i < b; ++i) s += (double) l[i] * l[i]; return std::sqrt (s / (double) (b - a)); };
        const double early = rmsOf (0.1, 0.3), late = rmsOf (2.5, 2.7);
        all &= report (at440 > 4.0 * off, "piano: A4 sounds at 440 Hz (" + juce::String (at440, 4) + " against " + juce::String (off, 4) + " next to it)");
        all &= report (late < early * 0.7 && late > 0.0, "piano: the note dies away (" + juce::String (early, 3) + " then " + juce::String (late, 3) + ")");

        Synth bass; bass.prepare (rate);
        SynthParams bassParams; bassParams.apply (SynthPresets::find ("Legato bass"));
        std::vector<float> bl ((size_t) total, 0.0f), br ((size_t) total, 0.0f);
        bass.noteOn (40, 0.8f, bassParams);
        bass.noteOn (47, 0.8f, bassParams);   // (while the first is held: glides, no second voice)
        done = 0;
        while (done < total) { const int n = std::min (512, total - done); bass.render (bl.data() + done, br.data() + done, n, bassParams); done += n; }
        float bassPeak = 0; for (auto v : bl) bassPeak = std::max (bassPeak, std::abs (v));
        bass.noteOff (47); bass.noteOff (40);
        all &= report (bassPeak > 0.05f && bassPeak < 0.9f, "mono bass: legato notes play as one voice (peak " + juce::String (bassPeak, 2) + ")");
    }

    // 7. the generated drums: each kind has its energy where it belongs, no sample is empty or clipping, and the variations differ from the original
    {
        StarterPack::ensureInstalled();
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        auto load = [&] (const juce::String& cat, const juce::String& name)
        {
            std::vector<float> out;
            if (std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (StarterPack::sample (cat, name))); r != nullptr)
            {
                juce::AudioBuffer<float> b (1, (int) std::min<juce::int64> (r->lengthInSamples, 96000));
                r->read (&b, 0, b.getNumSamples(), 0, true, true);
                out.assign (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
            }
            return out;
        };
        // the share of the energy of the first 60 ms below 300 Hz and above 4 kHz (a plain DFT on a log-spaced set of frequencies)
        auto shares = [] (const std::vector<float>& s)
        {
            const size_t len = std::min<size_t> (s.size(), 2880);
            double low = 0, high = 0, total = 1.0e-12;
            for (int k = 0; k < 96; ++k)
            {
                const double hz = 30.0 * std::pow (600.0, k / 95.0);   // 30 Hz .. 18 kHz
                double re = 0, im = 0;
                for (size_t i = 0; i < len; ++i) { const double ph = 6.283185307 * hz * (double) i / 48000.0; re += s[i] * std::cos (ph); im += s[i] * std::sin (ph); }
                const double e = (re * re + im * im) * hz;   // (a log-spaced grid: weight by the width of the bin)
                total += e;
                if (hz < 300.0) low += e;
                if (hz > 4000.0) high += e;
            }
            return std::make_pair (low / total, high / total);
        };
        bool drumsFine = true;
        juce::String detail;
        auto check = [&] (const char* cat, const char* name, const std::function<bool (double, double)>& expect)
        {
            const auto s = load (cat, name);
            float peak = 0; for (auto v : s) peak = std::max (peak, std::abs (v));
            const auto sh = shares (s);
            const bool ok = ! s.empty() && peak > 0.3f && peak <= 1.0f && expect (sh.first, sh.second);
            if (! ok) detail << " " << name << "(low " << juce::String (sh.first, 2) << ", high " << juce::String (sh.second, 2) << ")";
            drumsFine = drumsFine && ok;
        };
        for (auto* k : { "Kick Punch", "Kick Deep", "Kick 808", "Kick Soft" })
            check ("Kicks", k, [] (double low, double) { return low > 0.5; });
        for (auto* h : { "Hat Closed 1", "Hat Closed 2", "Hat Open 1" })
            check ("Hats", h, [] (double, double high) { return high > 0.5; });
        for (auto* s : { "Snare Tight", "Snare Snap" })
            check ("Snares", s, [] (double low, double) { return low < 0.6; });
        const auto a = load ("Snares", "Snare Tight"), b = load ("Variations", "Snare Tight b");
        double diff = 0; for (size_t i = 0; i < std::min (a.size(), b.size()); ++i) diff += std::abs (a[i] - b[i]);
        all &= report (drumsFine && diff > 1.0, "drums: kicks are low, hats are high, nothing is empty or clipping, a round-robin variation differs" + detail);
    }

    // 8. the extra drums: acoustic and electronic sounds exist, rooms and cymbals are stereo and long enough, the whole pack is made quickly
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        auto info = [&] (const char* cat, const char* name, int& channels, double& seconds, float& peak, double& lowShare)
        {
            channels = 0; seconds = 0; peak = 0; lowShare = 0;
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (StarterPack::sample (cat, name)));
            if (r == nullptr) return false;
            channels = (int) r->numChannels;
            seconds = (double) r->lengthInSamples / r->sampleRate;
            juce::AudioBuffer<float> b (channels, (int) r->lengthInSamples);
            r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
            for (int c = 0; c < channels; ++c) peak = std::max (peak, b.getMagnitude (c, 0, b.getNumSamples()));
            double low = 0, total = 1.0e-12;   // the share of the energy below 200 Hz, in the first 100 ms, by a plain DFT
            const int len = std::min (b.getNumSamples(), 4410);
            for (int k = 0; k < 64; ++k)
            {
                const double hz = 30.0 * std::pow (400.0, k / 63.0);
                double re = 0, im = 0;
                for (int i = 0; i < len; ++i) { const double ph = 6.283185307 * hz * i / r->sampleRate; re += b.getSample (0, i) * std::cos (ph); im += b.getSample (0, i) * std::sin (ph); }
                const double e = (re * re + im * im) * hz; total += e; if (hz < 200.0) low += e;
            }
            lowShare = low / total;
            return true;
        };
        bool fine = true; juce::String detail;
        auto expect = [&] (const char* cat, const char* name, int channels, double minSeconds, double minLow, double maxLow)
        {
            int ch; double sec, low; float peak;
            const bool ok = info (cat, name, ch, sec, peak, low) && ch == channels && sec >= minSeconds && peak > 0.3f && peak <= 1.0f && low >= minLow && low <= maxLow;
            if (! ok) detail << " " << name << "(ch " << ch << ", " << juce::String (sec, 2) << " s, peak " << juce::String (peak, 2) << ", low " << juce::String (low, 2) << ")";
            fine = fine && ok;
        };
        expect ("Kicks", "Kick 909", 1, 0.4, 0.5, 1.0);
        expect ("Kicks", "Kick Boom", 1, 1.0, 0.8, 1.0);
        expect ("Kicks", "Kick Acoustic", 2, 0.7, 0.5, 1.0);
        expect ("Snares", "Snare 909", 1, 0.25, 0.0, 0.5);
        expect ("Snares", "Snare Room", 2, 0.5, 0.0, 0.5);
        expect ("Claps", "Clap Room", 2, 0.5, 0.0, 0.2);
        expect ("Cymbals", "Crash", 2, 2.0, 0.0, 0.1);
        expect ("Cymbals", "Ride", 2, 1.3, 0.0, 0.2);
        expect ("Toms", "Tom Floor", 2, 0.8, 0.5, 1.0);
        expect ("Percussion", "Triangle", 2, 1.5, 0.0, 0.05);
        expect ("Hats", "Hat Pedal", 1, 0.04, 0.0, 0.1);

        auto marker = StarterPack::folder().getChildFile (".version");
        const auto keep = marker.loadFileAsString();
        marker.replaceWithText ("0");
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        StarterPack::ensureInstalled();   // (writes the whole pack again)
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        all &= report (fine, "drums: the new acoustic and electronic drums have the right length, width and weight" + detail);
        all &= report (ms < 30000.0, "starter pack: written in " + juce::String (ms, 0) + " ms");
    }

    all &= wavetableTests (workers);

   #ifdef TEST_CLAP_PATH
    all &= ClapTests::run ([] (bool ok, const juce::String& what) { return report (ok, what); }, workers);   // CLAP hosting
   #endif

    // 10. SoundFont and SFZ instruments: the players themselves, then on tracks (serial and parallel mixing identical, a copy of the track plays)
    {
        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory);
        all &= SfTests::run ([] (bool ok, const juce::String& what) { return report (ok, what); }, tmp);

        auto build = [&] (Engine& x)
        {
            auto* piano = x.addTrack ("SoundFont", Track::Type::Midi);
            auto* organ = x.addTrack ("SFZ", Track::Type::Midi);
            auto* synth = x.addTrack ("Synth", Track::Type::Midi);
            juce::String error;
            const bool a = x.loadSfInstrument (*piano, tmp.getChildFile ("sftest/test.sf2"), 0, error);
            const bool b = x.loadSfInstrument (*organ, tmp.getChildFile ("sftest/sfz/test.sfz"), 0, error);
            for (auto* t : { piano, organ })
                if (t->sfInstrument != nullptr && t->sfInstrument->getBank() != nullptr)
                    t->sfInstrument->getBank()->pool->waitForPreload (5000);
            std::vector<Note> notes;
            for (int i = 0; i < 20; ++i) notes.push_back ({ 50 + (i * 7) % 24, i * 0.25, 0.3, 0.5f + 0.02f * (float) (i % 10) });
            for (auto* t : { piano, organ, synth })
            {
                const int id = x.addMidiClip (*t, 0.0, 6.0);
                x.setMidiNotes (*t, id, notes);
                if (t == piano)   // the sustain pedal down for the first two seconds, a pitch bend sweep
                    x.setMidiCcs (*t, id, { { 0.0, 64, 1.0f }, { 2.0, 64, 0.0f }, { 3.0, CcEvent::kPitchBend, 0.5f }, { 3.5, CcEvent::kPitchBend, 0.75f }, { 4.0, CcEvent::kPitchBend, 0.5f } });
            }
            piano->pan = -0.3f; organ->pan = 0.3f;
            return a && b;
        };

        auto run = [&] (int n, std::vector<float>& out)
        {
            Engine x; x.prepareToPlay (512, 48000.0);
            const bool loaded = build (x);
            x.setWorkerThreads (n);
            x.forceParallel = n > 0;
            juce::AudioBuffer<float> b (2, 512); juce::AudioSourceChannelInfo info (&b, 0, 512);
            x.play();
            for (int blk = 0; blk < 500; ++blk) { b.clear(); x.getNextAudioBlock (info); for (int i = 0; i < 512; ++i) { out.push_back (b.getSample (0, i)); out.push_back (b.getSample (1, i)); } }
            return loaded;
        };
        std::vector<float> serial, par;
        const bool l1 = run (0, serial), l2 = run (workers, par);
        float peak = 0; for (auto v : par) peak = std::max (peak, std::abs (v));
        all &= report (l1 && l2 && serial.size() == par.size() && largestDifference (serial, par) < 1e-9 && peak > 0.05f,
                       "SoundFont + SFZ tracks: serial and " + juce::String (workers) + " workers identical (peak " + juce::String (peak, 3) + ")");

        [&]   // (a lambda: the engine would add to the stack frame of main)
        {
            Engine x; x.prepareToPlay (512, 48000.0);
            build (x);
            auto* piano = x.getTrack (0);
            Track* copy = x.duplicateTrack (*piano);
            all &= report (copy != nullptr && copy->sfInstrument != nullptr && copy->sfInstrument != piano->sfInstrument
                           && copy->sfInstrument->getBank() == piano->sfInstrument->getBank(), "SoundFont track: a copy of the track plays the same instrument with its own voices");
            const auto saved = x.captureState();
            x.setSfBank (*piano, nullptr);
            all &= report (piano->sfInstrument == nullptr, "SoundFont track: back to the built-in synth");
            x.restoreState (saved);
            piano = x.getTrack (0);
            all &= report (piano->sfInstrument != nullptr && piano->sfInstrument->getBank() != nullptr, "SoundFont track: undo brings the instrument back");
        }();
    }

    // 9. plugins in a separate process (the test plugin in a helper process of UFODAW.exe, against the same plugin in this program)
   #if JUCE_WINDOWS
    {
        const auto helper = SandboxTests::findHelper();
        if (helper.existsAsFile())
            all &= SandboxTests::run (helper, [] (bool ok, const juce::String& what) { return report (ok, what); });
        else
            all &= report (false, "sandbox: UFODAW.exe not found (build it too, or set UFODAW_HELPER)");
    }
   #endif

    std::cout << (all ? "ALL PASSED\n" : "FAILED\n");
    return all ? 0 : 1;
}
