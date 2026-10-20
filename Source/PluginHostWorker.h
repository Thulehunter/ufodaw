#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <mutex>
#include <queue>
#include "PluginSandbox.h"

namespace PluginSandbox
{
    // The helper process (UFODAW.exe --ufodawhost:<pipe>): hosts ONE plugin for the program (see PluginProxy.h). It has no window of its own; the
    // plugin's editor opens as a window of this process when the program asks for it. If the plugin faults the helper tells the program and ends.
    class HostWorker : public juce::ChildProcessWorker, private juce::AsyncUpdater, private juce::AudioProcessorListener
    {
    public:
        HostWorker();
        ~HostWorker() override;

        void handleConnectionMade() override;
        void handleMessageFromCoordinator (const juce::MemoryBlock&) override;
        void handleConnectionLost() override;

        void reportFault (const juce::String& text);   // any thread: tells the program, then ends this process

    private:
        class EditorWindow;
        class AudioThread;
        class Head;

        void handleAsyncUpdate() override;
        void handle (int type, uint32_t seq, juce::MemoryInputStream&);
        void reply (uint32_t seq, bool ok, const juce::MemoryBlock& data = {});
        void send (Msg type, const juce::MemoryBlock& payload = {});
        bool load (juce::MemoryInputStream&, juce::MemoryOutputStream& answer, juce::String& error);
        void processOne();   // audio thread: one block from the shared memory
        void editorClosedByUser();
        void quitNow();

        // AudioProcessorListener
        void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float value) override;
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;
        void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int index) override;
        void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int index) override;

        juce::AudioPluginFormatManager formats;
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::CriticalSection processLock;   // held while a block is processed and while the plugin is prepared, reset or gets a state
        std::unique_ptr<Head> head;
        std::unique_ptr<AudioThread> audio;
        std::unique_ptr<EditorWindow> editor;
        Shared* shared = nullptr;
        void* requestEvent = nullptr;
        void* doneEvent = nullptr;
        void* mapping = nullptr;
        juce::MidiBuffer midiBuffer;
        std::mutex queueMutex;
        std::queue<juce::MemoryBlock> queue;
        std::atomic<bool> quitting { false };
        double lastRate = 48000.0;
        int lastBlock = 512;
        std::atomic<int> reportedLatency { 0 };
    };
}
