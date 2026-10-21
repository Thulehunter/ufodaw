#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <mutex>
#include <queue>

// Scanning for plugins in a second process. A plugin that crashes while it is being examined (it happens) then takes down that helper and not
// the program: the scan goes on and the plugin is marked as failed. The helper is UFODAW.exe itself, started with a special command line
// (see Main.cpp); the scan window of the plugin manager uses it automatically.
namespace PluginScan
{
    constexpr const char* commandLineId = "ufodawscan";

    // The helper process: gets "format name \n file or identifier", answers with the descriptions it found (XML), or nothing at all if it crashes.
    class Worker : public juce::ChildProcessWorker, private juce::AsyncUpdater
    {
    public:
        Worker() { formats.addDefaultFormats(); }

        void handleMessageFromCoordinator (const juce::MemoryBlock& block) override
        {
            {
                const std::lock_guard<std::mutex> lock (mutex);
                pending.push (block);
            }
            triggerAsyncUpdate();   // (plugins want to be loaded on the message thread)
        }

        void handleConnectionLost() override { juce::MessageManager::callAsync ([] { juce::JUCEApplicationBase::quit(); }); }

    private:
        void handleAsyncUpdate() override
        {
            for (;;)
            {
                juce::MemoryBlock block;
                {
                    const std::lock_guard<std::mutex> lock (mutex);
                    if (pending.empty())
                        return;
                    block = pending.front();
                    pending.pop();
                }

                const auto parts = juce::StringArray::fromTokens (block.toString(), "\n", "");
                juce::XmlElement list ("LIST");
                if (parts.size() >= 2)
                {
                    juce::OwnedArray<juce::PluginDescription> found;
                    for (auto* format : formats.getFormats())
                        if (format->getName() == parts[0])
                            format->findAllTypesForFile (found, parts[1]);
                    for (auto* description : found)
                        if (auto xml = description->createXml())
                            list.addChildElement (xml.release());
                }
                const auto text = list.toString();
                sendMessageToCoordinator ({ text.toRawUTF8(), text.getNumBytesAsUTF8() });
            }
        }

        juce::AudioPluginFormatManager formats;
        std::mutex mutex;
        std::queue<juce::MemoryBlock> pending;
    };

    // The program's side: the scanner the plugin list uses. It asks the helper, and starts a new one when the helper died or took too long.
    class Scanner : public juce::KnownPluginList::CustomScanner, private juce::ChildProcessCoordinator
    {
    public:
        bool findPluginTypesFor (juce::AudioPluginFormat& format, juce::OwnedArray<juce::PluginDescription>& result,
                                 const juce::String& fileOrIdentifier) override
        {
            if (! helperRunning && ! startHelper())
                return false;

            {
                const std::lock_guard<std::mutex> lock (replyMutex);
                reply = {};
            }
            gotReply = false;
            lost = false;
            const auto request = format.getName() + "\n" + fileOrIdentifier;
            if (! sendMessageToWorker ({ request.toRawUTF8(), request.getNumBytesAsUTF8() }))
            {
                helperRunning = false;
                return false;
            }

            // wait for the answer; a plugin that hangs or crashes ends the wait
            const auto deadline = juce::Time::getMillisecondCounter() + 180000u;   // (big instruments such as EZdrummer 3 load their sound libraries while they are scanned: that can take a minute)
            while (! gotReply && ! lost && ! shouldExit() && juce::Time::getMillisecondCounter() < deadline)
                event.wait (100);

            if (! gotReply)
            {
                killWorkerProcess();   // (the next plugin gets a fresh helper)
                helperRunning = false;
                return false;
            }

            std::unique_ptr<juce::XmlElement> xml;
            {
                const std::lock_guard<std::mutex> lock (replyMutex);
                xml = juce::XmlDocument::parse (reply);
            }
            if (xml != nullptr)
                for (auto* child : xml->getChildIterator())
                {
                    auto description = std::make_unique<juce::PluginDescription>();
                    if (description->loadFromXml (*child))
                        result.add (description.release());
                }
            return true;
        }

        void scanFinished() override
        {
            killWorkerProcess();
            helperRunning = false;
        }

        ~Scanner() override { killWorkerProcess(); }

    private:
        bool startHelper()
        {
            helperRunning = launchWorkerProcess (helper.exists() ? helper : juce::File::getSpecialLocation (juce::File::currentExecutableFile), commandLineId, 0, 0);
            return helperRunning;
        }

    public:
        juce::File helper;   // (the program itself unless a test says otherwise)

    private:

        void handleMessageFromWorker (const juce::MemoryBlock& block) override
        {
            {
                const std::lock_guard<std::mutex> lock (replyMutex);
                reply = block.toString();
            }
            gotReply = true;
            event.signal();
        }

        void handleConnectionLost() override
        {
            lost = true;
            event.signal();
        }

        std::atomic<bool> gotReply { false }, lost { false };
        bool helperRunning = false;
        juce::WaitableEvent event;
        std::mutex replyMutex;
        juce::String reply;
    };
}
