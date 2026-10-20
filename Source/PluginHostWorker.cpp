#include "PluginHostWorker.h"

#if JUCE_WINDOWS

#include "TestPlugin.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <thread>

using namespace PluginSandbox;

namespace
{
    thread_local bool applyingFromProgram = false;   // (a parameter change the program asked for is not sent back)

    // processBlock behind a safety net (kept apart from the code that has objects with destructors: __try cannot live there)
    bool safeProcess (juce::AudioProcessor& p, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
    {
        __try
        {
            p.processBlock (buffer, midi);
            return true;
        }
        __except (1)
        {
            return false;
        }
    }
}

//==============================================================================
// The playhead the plugin sees: what the program wrote in the shared block for this block
class HostWorker::Head : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override
    {
        if (shared == nullptr || shared->hasPosition == 0)
            return {};
        PositionInfo info;
        info.setBpm (shared->bpm);
        info.setTimeSignature (TimeSignature { shared->timeSigNum, shared->timeSigDen });
        info.setTimeInSamples (shared->timeSamples);
        info.setTimeInSeconds (shared->timeSeconds);
        info.setPpqPosition (shared->ppq);
        info.setPpqPositionOfLastBarStart (shared->barStartPpq);
        info.setIsPlaying (shared->isPlaying != 0);
        info.setIsRecording (shared->isRecording != 0);
        info.setIsLooping (shared->isLooping != 0);
        if (shared->hasLoop != 0)
            info.setLoopPoints (LoopPoints { shared->loopStartPpq, shared->loopEndPpq });
        return info;
    }

    Shared* shared = nullptr;
};

//==============================================================================
class HostWorker::AudioThread : public juce::Thread
{
public:
    explicit AudioThread (HostWorker& w) : juce::Thread ("UFODAW plugin audio"), owner (w) {}

    void run() override
    {
        SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        while (! threadShouldExit())
        {
            if (WaitForSingleObject ((HANDLE) owner.requestEvent, 200) != WAIT_OBJECT_0)
                continue;
            owner.processOne();
            SetEvent ((HANDLE) owner.doneEvent);
        }
    }

private:
    HostWorker& owner;
};

//==============================================================================
// The plugin's own window, in this process
class HostWorker::EditorWindow : public juce::DocumentWindow
{
public:
    EditorWindow (HostWorker& w, juce::AudioProcessor& p, const juce::String& title)
        : juce::DocumentWindow (title, juce::Colour (0xff1c1c1c), juce::DocumentWindow::closeButton), owner (w)
    {
        setUsingNativeTitleBar (true);
        juce::AudioProcessorEditor* editor = p.hasEditor() ? p.createEditorIfNeeded() : nullptr;
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (p);
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        if (auto* peer = getPeer())   // (plugin editors are native child windows: the software renderer keeps them from turning white)
        {
            const auto engines = peer->getAvailableRenderingEngines();
            const int software = engines.indexOf ("Software Renderer");
            if (software >= 0)
                peer->setCurrentRenderingEngine (software);
        }
        toFront (true);
    }

    void closeButtonPressed() override { owner.editorClosedByUser(); }

private:
    HostWorker& owner;
};

//==============================================================================
HostWorker::HostWorker()
{
    SetErrorMode (SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);   // (a crash here must not show a Windows dialog: the program hears about it)
}

HostWorker::~HostWorker()
{
    quitting = true;
    cancelPendingUpdate();
    audio.reset();
    editor.reset();
    if (shared != nullptr) UnmapViewOfFile (shared);
    if (mapping != nullptr) CloseHandle ((HANDLE) mapping);
    if (requestEvent != nullptr) CloseHandle ((HANDLE) requestEvent);
    if (doneEvent != nullptr) CloseHandle ((HANDLE) doneEvent);
}

void HostWorker::send (Msg type, const juce::MemoryBlock& payload)
{
    juce::MemoryOutputStream out;
    out.writeByte ((char) type);
    out.writeInt (0);
    out.write (payload.getData(), payload.getSize());
    sendMessageToCoordinator (out.getMemoryBlock());
}

void HostWorker::reply (uint32_t seq, bool ok, const juce::MemoryBlock& data)
{
    juce::MemoryOutputStream out;
    out.writeByte ((char) Reply);
    out.writeInt ((int) seq);
    out.writeByte (ok ? 1 : 0);
    out.write (data.getData(), data.getSize());
    sendMessageToCoordinator (out.getMemoryBlock());
}

void HostWorker::handleConnectionMade()
{
    juce::MemoryOutputStream out;
    out.writeInt ((int) GetCurrentProcessId());
    send (Hello, out.getMemoryBlock());
}

void HostWorker::handleConnectionLost()
{
    quitNow();
}

void HostWorker::quitNow()
{
    // The program is gone (or asked us to go): end at once. A watchdog makes sure that a plugin that hangs in its destructor cannot keep us here.
    std::thread ([] { Sleep (1500); TerminateProcess (GetCurrentProcess(), 0); }).detach();
    juce::MessageManager::callAsync ([this]
    {
        quitting = true;
        editor.reset();
        audio.reset();
        {
            const juce::ScopedLock sl (processLock);
            if (plugin != nullptr)
                plugin->removeListener (this);
            plugin.reset();
        }
        TerminateProcess (GetCurrentProcess(), 0);
    });
}

void HostWorker::reportFault (const juce::String& text)
{
    send (HelperFailed, [&] { juce::MemoryOutputStream o; o.writeString (text); return o.getMemoryBlock(); }());
    Sleep (100);
    TerminateProcess (GetCurrentProcess(), 3);
}

void HostWorker::handleMessageFromCoordinator (const juce::MemoryBlock& block)
{
    if (block.getSize() >= 5 && (uint8_t) ((const char*) block.getData())[0] == Ping)
    {
        // answered right here (not on the message thread): the program also uses it to see that this process lives
        juce::MemoryInputStream in (block, false);
        in.readByte();
        const auto seq = (uint32_t) in.readInt();
        PROCESS_MEMORY_COUNTERS_EX counters = {};
        GetProcessMemoryInfo (GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*) &counters, sizeof (counters));
        juce::MemoryOutputStream out;
        out.writeInt64 ((juce::int64) counters.PrivateUsage);
        out.writeInt ((int) GetGuiResources (GetCurrentProcess(), GR_GDIOBJECTS));
        out.writeInt ((int) GetGuiResources (GetCurrentProcess(), GR_USEROBJECTS));
        reply (seq, true, out.getMemoryBlock());
        return;
    }
    {
        const std::lock_guard<std::mutex> lock (queueMutex);
        queue.push (block);
    }
    triggerAsyncUpdate();   // (plugins want to be called on the message thread)
}

void HostWorker::handleAsyncUpdate()
{
    for (;;)
    {
        juce::MemoryBlock block;
        {
            const std::lock_guard<std::mutex> lock (queueMutex);
            if (queue.empty())
                return;
            block = queue.front();
            queue.pop();
        }
        juce::MemoryInputStream in (block, false);
        const int type = (uint8_t) in.readByte();
        const auto seq = (uint32_t) in.readInt();
        handle (type, seq, in);
    }
}

//==============================================================================
bool HostWorker::load (juce::MemoryInputStream& in, juce::MemoryOutputStream& out, juce::String& error)
{
    const auto xmlText = in.readString();
    const double rate = in.readDouble();
    const int block = in.readInt();
    const auto memoryName = in.readString(), requestName = in.readString(), doneName = in.readString();
    lastRate = rate;
    lastBlock = block;

    mapping = OpenFileMappingW (FILE_MAP_ALL_ACCESS, FALSE, memoryName.toWideCharPointer());
    if (mapping != nullptr)
        shared = static_cast<Shared*> (MapViewOfFile ((HANDLE) mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (Shared)));
    requestEvent = OpenEventW (EVENT_ALL_ACCESS, FALSE, requestName.toWideCharPointer());
    doneEvent = OpenEventW (EVENT_ALL_ACCESS, FALSE, doneName.toWideCharPointer());
    if (shared == nullptr || requestEvent == nullptr || doneEvent == nullptr || shared->magic != sharedMagic)
    {
        error = "The plugin process could not open the shared memory";
        return false;
    }

    juce::PluginDescription description;
    auto xml = juce::XmlDocument::parse (xmlText);
    if (xml == nullptr || ! description.loadFromXml (*xml))
    {
        error = "Unreadable plugin description";
        return false;
    }

    if (description.pluginFormatName == testFormatName)
        plugin = std::make_unique<TestPlugin> (description.fileOrIdentifier);
    else
    {
        formats.addDefaultFormats();
        plugin = formats.createPluginInstance (description, rate, block, error);
    }
    if (plugin == nullptr)
    {
        if (error.isEmpty())
            error = "The plugin could not be loaded";
        return false;
    }

    head = std::make_unique<Head>();
    head->shared = shared;
    plugin->setPlayHead (head.get());
    plugin->addListener (this);
    reportedLatency = plugin->getLatencySamples();

    out.writeString (plugin->getName());
    out.writeBool (plugin->acceptsMidi()); out.writeBool (plugin->producesMidi()); out.writeBool (plugin->isMidiEffect()); out.writeBool (plugin->hasEditor());
    out.writeInt (plugin->getLatencySamples());
    out.writeDouble (plugin->getTailLengthSeconds());
    out.writeInt (plugin->getNumPrograms());
    writeLayout (out, plugin->getBusesLayout());
    for (bool isInput : { true, false })
    {
        out.writeInt (plugin->getBusCount (isInput));
        for (int i = 0; i < plugin->getBusCount (isInput); ++i)
        {
            out.writeString (plugin->getBus (isInput, i)->getName());
            out.writeBool (plugin->getBus (isInput, i)->isEnabled());
        }
    }
    auto& params = plugin->getParameters();
    out.writeInt (params.size());
    for (auto* p : params)
    {
        out.writeString (p->getName (128));
        out.writeString (p->getLabel());
        out.writeFloat (p->getDefaultValue());
        out.writeFloat (p->getValue());
        out.writeInt (p->getNumSteps());
        out.writeBool (p->isDiscrete()); out.writeBool (p->isBoolean()); out.writeBool (p->isAutomatable()); out.writeBool (p->isMetaParameter());
        out.writeInt ((int) p->getCategory());
    }

    audio = std::make_unique<AudioThread> (*this);
    audio->startThread();
    return true;
}

void HostWorker::handle (int type, uint32_t seq, juce::MemoryInputStream& in)
{
    switch (type)
    {
        case Load:
        {
            juce::MemoryOutputStream out;
            juce::String error;
            if (load (in, out, error))
                reply (seq, true, out.getMemoryBlock());
            else
                reply (seq, false, juce::MemoryBlock (error.toRawUTF8(), error.getNumBytesAsUTF8()));
            break;
        }
        case CheckLayout:
        {
            const bool ok = plugin != nullptr && plugin->checkBusesLayoutSupported (readLayout (in));
            juce::MemoryOutputStream out;
            out.writeByte (ok ? 1 : 0);
            reply (seq, true, out.getMemoryBlock());
            break;
        }
        case ChangeLayout:
        {
            bool ok = false;
            if (plugin != nullptr)
            {
                const juce::ScopedLock sl (processLock);
                const auto layout = readLayout (in);
                ok = plugin->getBusesLayout() == layout || plugin->setBusesLayout (layout);
            }
            reply (seq, ok);
            break;
        }
        case Prepare:
        {
            lastRate = in.readDouble();
            lastBlock = in.readInt();
            juce::MemoryOutputStream out;
            if (plugin != nullptr)
            {
                const juce::ScopedLock sl (processLock);
                plugin->prepareToPlay (lastRate, lastBlock);
                reportedLatency = plugin->getLatencySamples();
                out.writeInt (plugin->getLatencySamples());
                out.writeDouble (plugin->getTailLengthSeconds());
            }
            reply (seq, plugin != nullptr, out.getMemoryBlock());
            break;
        }
        case Release:
            if (plugin != nullptr)
            {
                const juce::ScopedLock sl (processLock);
                plugin->releaseResources();
            }
            break;
        case GetState:
        {
            juce::MemoryBlock state;
            if (plugin != nullptr)
                plugin->getStateInformation (state);
            reply (seq, plugin != nullptr, state);
            break;
        }
        case SetState:
        {
            juce::MemoryBlock state;
            in.readIntoMemoryBlock (state);
            juce::MemoryOutputStream out;
            if (plugin != nullptr)
            {
                const juce::ScopedLock sl (processLock);
                applyingFromProgram = true;
                plugin->setStateInformation (state.getData(), (int) state.getSize());
                applyingFromProgram = false;
                auto& params = plugin->getParameters();
                out.writeInt (params.size());
                for (auto* p : params)
                    out.writeFloat (p->getValue());
            }
            reply (seq, plugin != nullptr, out.getMemoryBlock());
            break;
        }
        case SetParam:
        {
            const int index = in.readInt();
            const float value = in.readFloat();
            if (plugin != nullptr && juce::isPositiveAndBelow (index, plugin->getParameters().size()))
            {
                applyingFromProgram = true;
                plugin->getParameters()[index]->setValueNotifyingHost (value);
                applyingFromProgram = false;
            }
            break;
        }
        case GetParamText:
        {
            const int index = in.readInt();
            const float value = in.readFloat();
            const auto text = plugin != nullptr && juce::isPositiveAndBelow (index, plugin->getParameters().size())
                                  ? plugin->getParameters()[index]->getText (value, 64) : juce::String();
            reply (seq, true, juce::MemoryBlock (text.toRawUTF8(), text.getNumBytesAsUTF8()));
            break;
        }
        case GetParamValue:
        {
            const int index = in.readInt();
            const auto text = in.readString();
            const float value = plugin != nullptr && juce::isPositiveAndBelow (index, plugin->getParameters().size())
                                    ? plugin->getParameters()[index]->getValueForText (text) : 0.0f;
            reply (seq, true, juce::MemoryBlock (&value, sizeof (value)));
            break;
        }
        case ShowEditor:
            if (plugin != nullptr)
            {
                if (editor != nullptr)
                    editor->toFront (true);
                else
                    editor = std::make_unique<EditorWindow> (*this, *plugin, in.readString());
            }
            break;
        case CloseEditor:
            editor.reset();
            break;
        case Reset:
            if (plugin != nullptr)
            {
                const juce::ScopedLock sl (processLock);
                plugin->reset();
            }
            break;
        case SetProgram:
            if (plugin != nullptr)
                plugin->setCurrentProgram (in.readInt());
            break;
        case GetProgramName:
        {
            const int index = in.readInt();
            const auto name = plugin != nullptr ? plugin->getProgramName (index) : juce::String();
            reply (seq, true, juce::MemoryBlock (name.toRawUTF8(), name.getNumBytesAsUTF8()));
            break;
        }
        case Quit:
            quitNow();
            break;
        default:
            break;
    }
}

void HostWorker::editorClosedByUser()
{
    juce::MessageManager::callAsync ([this]
    {
        editor.reset();
        send (EditorClosed);
    });
}

//==============================================================================
// the audio thread

void HostWorker::processOne()
{
    Shared& s = *shared;
    s.status = 0;
    s.midiOutBytes = 0;
    if (plugin == nullptr || quitting.load())
    {
        s.status = 1;
        return;
    }

    const juce::ScopedLock sl (processLock);
    const int n = juce::jlimit (0, maxBlock, s.numSamples), channels = juce::jlimit (0, maxChannels, s.numChannels);
    float* pointers[maxChannels];
    for (int c = 0; c < channels; ++c)
        pointers[c] = s.audio[c];
    juce::AudioBuffer<float> buffer (pointers, channels, n);

    midiBuffer.clear();
    readMidi (s.midiIn, s.midiInBytes, 0, midiBuffer);

    applyingFromProgram = true;
    for (int i = 0; i < s.numParamChanges && i < maxParamChanges; ++i)
        if (juce::isPositiveAndBelow (s.params[i].index, plugin->getParameters().size()))
            plugin->getParameters()[s.params[i].index]->setValueNotifyingHost (s.params[i].value);
    applyingFromProgram = false;

    if ((s.flags & 1) != 0)
        plugin->reset();

    if (! safeProcess (*plugin, buffer, midiBuffer))
    {
        s.status = 1;
        return;
    }

    s.midiOutBytes = writeMidi (midiBuffer, 0, n, s.midiOut, midiAreaBytes);
    s.latency = plugin->getLatencySamples();
    s.tailSeconds = plugin->getTailLengthSeconds();
    ++s.blocksDone;
}

//==============================================================================
// the plugin tells the program what it does itself

void HostWorker::audioProcessorParameterChanged (juce::AudioProcessor*, int index, float value)
{
    if (applyingFromProgram)
        return;
    juce::MemoryOutputStream out;
    out.writeInt (index);
    out.writeFloat (value);
    send (ParamChanged, out.getMemoryBlock());
}

void HostWorker::audioProcessorChanged (juce::AudioProcessor* p, const ChangeDetails& details)
{
    if (details.latencyChanged && p != nullptr)
    {
        juce::MemoryOutputStream out;
        out.writeInt (p->getLatencySamples());
        out.writeDouble (p->getTailLengthSeconds());
        send (LatencyChanged, out.getMemoryBlock());
    }
}

void HostWorker::audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int index)
{
    juce::MemoryOutputStream out;
    out.writeInt (index);
    send (GestureBegin, out.getMemoryBlock());
}

void HostWorker::audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int index)
{
    juce::MemoryOutputStream out;
    out.writeInt (index);
    send (GestureEnd, out.getMemoryBlock());
}

#endif
