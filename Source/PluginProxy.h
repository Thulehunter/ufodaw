#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include "Plugin.h"

namespace PluginSandbox { class Session; }

// A plugin that runs in a helper process, presented to the program as an ordinary juce::AudioPluginInstance: the engine, the project files and the
// automation lanes cannot tell the difference. See PluginSandbox.h for how the two processes talk.
//  - processBlock: audio and MIDI go through shared memory; no answer in time (or a dead helper) = silence and hasFailed().
//  - parameters: the proxy has its own parameter objects (copies of the plugin's); changes go both ways.
//  - state: getStateInformation asks the helper; if the helper is gone it answers with the last state it knew, so saving a project still works.
//  - the editor is a window of the helper process (showEditor).
//  - restart(): a new helper with the same plugin and the last known state.
class PluginProxy : public juce::AudioPluginInstance, public RemotePlugin, private juce::Timer
{
public:
    // The helper is the program itself unless a test says otherwise.
    static juce::File helperExecutable;

    // Starts a helper process, loads the plugin in it and returns the proxy (message thread). Null with `error` filled when that fails.
    static std::unique_ptr<PluginProxy> create (const juce::PluginDescription&, double sampleRate, int blockSize, juce::String& error,
                                                 bool* helperFailedToStart = nullptr);   // (set when no helper process could be started at all)

    static bool isSupported();   // Windows only (shared memory and events are the Windows ones)

    // How long the audio thread waits for the helper to finish one block before it gives up on the plugin (the very first block after prepareToPlay gets longer)
    static inline std::atomic<int> blockTimeoutMs { 1000 };

    ~PluginProxy() override;

    // RemotePlugin
    bool hasFailed() const override { return failed.load(); }
    juce::String failureReason() const override;
    bool restart (juce::String& error) override;
    void showEditor (const juce::String& title) override;
    void closeEditor() override;
    bool isEditorOpen() const override { return editorIsOpen.load(); }

    // AudioProcessor
    const juce::String getName() const override { return description.name; }
    void fillInPluginDescription (juce::PluginDescription& d) const override { d = description; }
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void reset() override;
    double getTailLengthSeconds() const override { return tailSeconds.load(); }
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }   // (the window is the helper's: see showEditor)
    bool hasEditor() const override { return true; }
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processorLayoutsChanged() override;

    // What it costs (measured on the audio thread): microseconds per block for the round trip to the helper
    struct Timing { double averageMicros = 0, worstMicros = 0; juce::int64 blocks = 0; };
    Timing getTiming() const;
    void resetTiming();

    // The helper's own numbers (asks it): private memory in bytes and GDI/USER objects. False if it does not answer.
    bool getHelperStats (juce::int64& privateBytes, int& gdiObjects, int& userObjects);
    int getHelperProcessId() const;

    void killHelper() override;
    int getBlocksProcessed() const { return (int) blocksProcessed.load(); }

    struct Info;   // what the helper told when it loaded the plugin

private:
    class Param;
    friend class Param;
    friend class PluginSandbox::Session;

    PluginProxy (const juce::AudioProcessor::BusesProperties&, std::shared_ptr<PluginSandbox::Session>, const juce::PluginDescription&, std::unique_ptr<Info>);

    void timerCallback() override;
    std::shared_ptr<PluginSandbox::Session> currentSession() const;
    void markFailed (int code);
    void handleNotification (int type, const juce::MemoryBlock& payload);   // pipe thread
    void queueParameter (int index, float value);
    juce::String parameterText (int index, float value);
    float parameterValueForText (int index, const juce::String& text);
    void applyValues (const juce::MemoryBlock& reply);

    std::unique_ptr<Info> infoOwner;
    Info& info;
    juce::PluginDescription description;

    mutable juce::CriticalSection swapLock;   // the audio thread only try-locks it; restart() holds it for the swap
    std::shared_ptr<PluginSandbox::Session> session;

    std::atomic<bool> failed { false };
    std::atomic<int> failureCode { 0 };           // 1 no answer in time, 2 helper ended, 3 the plugin faulted, 4 could not start
    bool failureReported = false;
    juce::String failureText, helperText;   // (helperText: what the helper itself said when its plugin faulted)
    std::atomic<int> controlBusy { 0 };           // > 0: a request that holds the plugin is in flight (state, prepare...): blocks are skipped, not failed
    std::atomic<bool> editorIsOpen { false };
    std::atomic<double> tailSeconds { 0.0 };
    std::atomic<int> reportedLatency { 0 };
    std::atomic<juce::int64> blocksProcessed { 0 };
    std::atomic<juce::int64> blocksSincePrepare { 0 };
    std::atomic<bool> resetPending { false };
    double sampleRate = 48000.0;
    int blockSize = 512;
    int currentProgram = 0;

    mutable juce::CriticalSection layoutLock;
    mutable std::map<juce::String, bool> layoutCache;   // (bus layouts the helper's plugin accepts or not)

    mutable juce::CriticalSection stateLock;
    juce::MemoryBlock cachedState;                // the last state the plugin gave (or was given)

    std::mutex paramMutex;                        // parameter changes made on other threads than the message thread, sent with the next block
    std::vector<std::pair<int, float>> pendingParams;

    std::atomic<juce::int64> timingTicks { 0 }, timingWorst { 0 }, timingBlocks { 0 };
    juce::MidiBuffer midiScratch;

    struct Alive { std::atomic<bool> value { true }; };
    std::shared_ptr<Alive> alive = std::make_shared<Alive>();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProxy)
};
