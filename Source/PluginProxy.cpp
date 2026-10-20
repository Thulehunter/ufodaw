#include "PluginProxy.h"
#include "PluginSandbox.h"
#include <map>
#include <thread>

juce::File PluginProxy::helperExecutable;

#if JUCE_WINDOWS

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace PluginSandbox;

//==============================================================================
// What the helper tells about the plugin it loaded
struct PluginProxy::Info
{
    struct P { juce::String name, label; float def = 0, value = 0; int steps = 0; bool discrete = false, boolean = false, automatable = true, meta = false; int category = 0; };

    juce::String name;
    bool acceptsMidi = false, producesMidi = false, midiEffect = false, hasEditor = false;
    int latency = 0, numPrograms = 1;
    double tail = 0.0;
    juce::AudioProcessor::BusesLayout layout;
    juce::StringArray inNames, outNames;
    std::vector<bool> inEnabled, outEnabled;
    std::vector<P> params;

    bool read (juce::MemoryInputStream& in)
    {
        name = in.readString();
        acceptsMidi = in.readBool(); producesMidi = in.readBool(); midiEffect = in.readBool(); hasEditor = in.readBool();
        latency = in.readInt(); tail = in.readDouble(); numPrograms = in.readInt();
        layout = readLayout (in);
        for (int i = 0, n = in.readInt(); i < n; ++i) { inNames.add (in.readString()); inEnabled.push_back (in.readBool()); }
        for (int i = 0, n = in.readInt(); i < n; ++i) { outNames.add (in.readString()); outEnabled.push_back (in.readBool()); }
        const int count = in.readInt();
        if (count < 0 || count > 200000)
            return false;
        params.resize ((size_t) count);
        for (auto& p : params)
        {
            p.name = in.readString(); p.label = in.readString();
            p.def = in.readFloat(); p.value = in.readFloat(); p.steps = in.readInt();
            p.discrete = in.readBool(); p.boolean = in.readBool(); p.automatable = in.readBool(); p.meta = in.readBool(); p.category = in.readInt();
        }
        return true;
    }
};

namespace
{
    HANDLE killJob()   // helper processes are ended with the program, whatever happens to it
    {
        static HANDLE job = []
        {
            HANDLE h = CreateJobObjectW (nullptr, nullptr);
            if (h != nullptr)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject (h, JobObjectExtendedLimitInformation, &limits, sizeof (limits));
            }
            return h;
        }();
        return job;
    }

    juce::MemoryBlock block (const juce::MemoryOutputStream& s) { return s.getMemoryBlock(); }
}

//==============================================================================
namespace PluginSandbox
{
// The program's end of one helper process: the pipe, the shared block, the events and the process handle.
class Session : public juce::ChildProcessCoordinator
{
public:
    struct Pending
    {
        juce::WaitableEvent event;
        std::atomic<bool> done { false };
        bool ok = false;
        juce::MemoryBlock data;
    };

    static std::shared_ptr<Session> launch (const juce::File& exe, juce::String& error)
    {
        std::shared_ptr<Session> s (new Session());
        static std::atomic<int> counter { 0 };
        const auto base = "Local\\ufodaw-ph-" + juce::String ((int) GetCurrentProcessId()) + "-" + juce::String (++counter) + "-" + juce::String::toHexString (juce::Random().nextInt (0x7fffffff));
        s->mapping = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD) sizeof (Shared), (base + "-mem").toWideCharPointer());
        if (s->mapping != nullptr)
            s->shm = static_cast<Shared*> (MapViewOfFile (s->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (Shared)));
        s->requestEvent = CreateEventW (nullptr, FALSE, FALSE, (base + "-req").toWideCharPointer());
        s->doneEvent = CreateEventW (nullptr, FALSE, FALSE, (base + "-done").toWideCharPointer());
        if (s->shm == nullptr || s->requestEvent == nullptr || s->doneEvent == nullptr)
        {
            error = "Could not create the shared memory for the plugin process";
            return nullptr;
        }
        std::memset (s->shm, 0, sizeof (Shared));
        s->shm->magic = sharedMagic;
        s->names = { base + "-mem", base + "-req", base + "-done" };

        const auto file = exe.exists() ? exe : juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        if (! s->launchWorkerProcess (file, commandLineId, 30000, 0))
        {
            error = "Could not start the plugin process (" + file.getFullPathName() + ")";
            return nullptr;
        }
        if (! s->helloEvent.wait (20000) || s->helperPid == 0)
        {
            error = "The plugin process did not start";
            return nullptr;
        }
        s->process = OpenProcess (SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA, FALSE, (DWORD) s->helperPid);
        if (s->process == nullptr)
        {
            error = "Could not open the plugin process";
            return nullptr;
        }
        if (auto job = killJob())
            AssignProcessToJobObject (job, s->process);
        return s;
    }

    ~Session() override
    {
        setSink (nullptr);
        if (process != nullptr)
        {
            sendMessage (Quit, 0, {});
            killWorkerProcess();   // (the helper ends itself when it hears that; if it hangs it is ended after a few seconds)
            HANDLE h = process;
            process = nullptr;
            std::thread ([h]
            {
                if (WaitForSingleObject (h, 4000) == WAIT_TIMEOUT)
                    TerminateProcess (h, 1);
                CloseHandle (h);
            }).detach();
        }
        if (shm != nullptr) UnmapViewOfFile (shm);
        if (mapping != nullptr) CloseHandle (mapping);
        if (requestEvent != nullptr) CloseHandle (requestEvent);
        if (doneEvent != nullptr) CloseHandle (doneEvent);
    }

    // Ends a session on a thread of its own: closing the pipe to a helper that is dead or hanging can take a while
    static void retire (std::shared_ptr<Session> s)
    {
        if (s != nullptr)
            std::thread ([s = std::move (s)] () mutable { s.reset(); }).detach();
    }

    bool alive() const
    {
        return ! lost.load() && (process == nullptr || WaitForSingleObject (process, 0) == WAIT_TIMEOUT);
    }

    unsigned long exitCode() const
    {
        DWORD code = 0;
        if (process != nullptr)
            GetExitCodeProcess (process, &code);
        return code;
    }

    void kill() { if (process != nullptr) TerminateProcess (process, 1); }

    bool sendMessage (Msg type, uint32_t seq, const juce::MemoryBlock& payload)
    {
        juce::MemoryOutputStream out;
        out.writeByte ((char) type);
        out.writeInt ((int) seq);
        out.write (payload.getData(), payload.getSize());
        return sendMessageToWorker (out.getMemoryBlock());
    }

    void post (Msg type, const juce::MemoryBlock& payload = {}) { sendMessage (type, 0, payload); }

    // Asks the helper and waits for its answer. False on a time-out, a dead helper or an error answer (then `reply` holds the error text).
    bool request (Msg type, const juce::MemoryBlock& payload, juce::MemoryBlock& reply, int timeoutMs)
    {
        auto pending = std::make_shared<Pending>();
        uint32_t seq;
        {
            const std::lock_guard<std::mutex> lock (pendingMutex);
            seq = ++nextSeq;
            pendingMap[seq] = pending;
        }
        bool sent = sendMessage (type, seq, payload);
        const auto start = juce::Time::getMillisecondCounter();
        while (sent && ! pending->event.wait (50))
        {
            if (! alive() || juce::Time::getMillisecondCounter() - start > (juce::uint32) timeoutMs)
                break;
        }
        {
            const std::lock_guard<std::mutex> lock (pendingMutex);
            pendingMap.erase (seq);
        }
        if (! pending->done.load())
            return false;
        reply = pending->data;
        return pending->ok;
    }

    void setSink (std::function<void (int, const juce::MemoryBlock&)> f)
    {
        const std::lock_guard<std::mutex> lock (sinkMutex);
        sink = std::move (f);
    }

    Shared* shm = nullptr;
    HANDLE requestEvent = nullptr, doneEvent = nullptr, process = nullptr;
    Names names;
    int helperPid = 0;

private:
    Session() = default;

    void handleMessageFromWorker (const juce::MemoryBlock& block) override
    {
        juce::MemoryInputStream in (block, false);
        const int type = (uint8_t) in.readByte();
        const uint32_t seq = (uint32_t) in.readInt();
        juce::MemoryBlock payload;
        in.readIntoMemoryBlock (payload);

        if (type == Reply)
        {
            std::shared_ptr<Pending> pending;
            {
                const std::lock_guard<std::mutex> lock (pendingMutex);
                if (auto it = pendingMap.find (seq); it != pendingMap.end())
                    pending = it->second;
            }
            if (pending != nullptr)
            {
                pending->ok = payload.getSize() > 0 && ((const char*) payload.getData())[0] != 0;
                if (payload.getSize() > 0)
                    pending->data = juce::MemoryBlock ((const char*) payload.getData() + 1, payload.getSize() - 1);
                pending->done = true;
                pending->event.signal();
            }
            return;
        }
        if (type == Hello)
        {
            juce::MemoryInputStream p (payload, false);
            helperPid = p.readInt();
            helloEvent.signal();
            return;
        }
        const std::lock_guard<std::mutex> lock (sinkMutex);
        if (sink)
            sink (type, payload);
    }

    void handleConnectionLost() override { lost = true; }

    std::atomic<bool> lost { false };
    juce::WaitableEvent helloEvent;
    HANDLE mapping = nullptr;
    std::mutex pendingMutex, sinkMutex;
    std::map<uint32_t, std::shared_ptr<Pending>> pendingMap;
    uint32_t nextSeq = 0;
    std::function<void (int, const juce::MemoryBlock&)> sink;
};
}

//==============================================================================
// A parameter of the plugin in the helper: a copy of its value, changes travel to the helper
class PluginProxy::Param : public juce::AudioProcessorParameter
{
public:
    Param (PluginProxy& o, int i, const Info::P& p) : owner (o), index (i), info (p), value (p.value) {}

    float getValue() const override { return value.load(); }
    void setValue (float v) override
    {
        value = v;
        owner.queueParameter (index, v);
    }
    float getDefaultValue() const override { return info.def; }
    juce::String getName (int maxLength) const override { return info.name.substring (0, maxLength); }
    juce::String getLabel() const override { return info.label; }
    juce::String getText (float v, int maxLength) const override
    {
        return owner.parameterText (index, v).substring (0, maxLength);
    }
    float getValueForText (const juce::String& text) const override { return owner.parameterValueForText (index, text); }
    int getNumSteps() const override { return info.steps > 0 ? info.steps : juce::AudioProcessor::getDefaultNumParameterSteps(); }
    bool isDiscrete() const override { return info.discrete; }
    bool isBoolean() const override { return info.boolean; }
    bool isAutomatable() const override { return info.automatable; }
    bool isMetaParameter() const override { return info.meta; }
    Category getCategory() const override { return (Category) info.category; }

    std::atomic<float> value;

private:
    PluginProxy& owner;
    int index;
    Info::P info;
};

//==============================================================================
static juce::MemoryBlock layoutBlock (const juce::AudioProcessor::BusesLayout& layout)
{
    juce::MemoryOutputStream out;
    writeLayout (out, layout);
    return out.getMemoryBlock();
}

bool PluginProxy::isSupported() { return true; }

bool PluginProxy::acceptsMidi() const { return info.acceptsMidi; }
bool PluginProxy::producesMidi() const { return info.producesMidi; }
bool PluginProxy::isMidiEffect() const { return info.midiEffect; }
int PluginProxy::getNumPrograms() { return juce::jmax (1, info.numPrograms); }

// Starts the plugin in the helper that `session` is connected to.
static bool loadInHelper (Session& session, const juce::PluginDescription& description, double sampleRate, int blockSize,
                          PluginProxy::Info& info, juce::String& error)
{
    juce::MemoryOutputStream out;
    if (auto xml = description.createXml())
        out.writeString (xml->toString());
    else
        out.writeString ({});
    out.writeDouble (sampleRate);
    out.writeInt (blockSize);
    out.writeString (session.names.memory);
    out.writeString (session.names.request);
    out.writeString (session.names.done);

    juce::MemoryBlock reply;
    if (! session.request (Load, out.getMemoryBlock(), reply, 120000))
    {
        error = session.alive() ? juce::String ("The plugin did not load in time") : juce::String ("The plugin process ended while it was loading the plugin");
        if (reply.getSize() > 0)
            error = reply.toString();
        return false;
    }
    juce::MemoryInputStream in (reply, false);
    if (! info.read (in))
    {
        error = "The plugin process gave an answer that could not be read";
        return false;
    }
    return true;
}

std::unique_ptr<PluginProxy> PluginProxy::create (const juce::PluginDescription& description, double sampleRate, int blockSize, juce::String& error,
                                                  bool* helperFailedToStart)
{
    auto session = Session::launch (helperExecutable, error);
    if (session == nullptr)
    {
        if (helperFailedToStart != nullptr)
            *helperFailedToStart = true;
        return nullptr;
    }

    auto info = std::make_unique<Info>();
    if (! loadInHelper (*session, description, sampleRate, blockSize, *info, error))
        return nullptr;

    BusesProperties props;
    for (int i = 0; i < info->layout.inputBuses.size(); ++i)
    {
        const auto set = info->layout.inputBuses[i].isDisabled() ? juce::AudioChannelSet::stereo() : info->layout.inputBuses[i];
        props = props.withInput (info->inNames[i], set, info->inEnabled[(size_t) i]);
    }
    for (int i = 0; i < info->layout.outputBuses.size(); ++i)
    {
        const auto set = info->layout.outputBuses[i].isDisabled() ? juce::AudioChannelSet::stereo() : info->layout.outputBuses[i];
        props = props.withOutput (info->outNames[i], set, info->outEnabled[(size_t) i]);
    }

    std::unique_ptr<PluginProxy> proxy (new PluginProxy (props, session, description, std::move (info)));
    proxy->sampleRate = sampleRate;
    proxy->blockSize = blockSize;
    return proxy;
}

PluginProxy::PluginProxy (const juce::AudioProcessor::BusesProperties& props, std::shared_ptr<Session> s,
                          const juce::PluginDescription& d, std::unique_ptr<Info> i)
    : juce::AudioPluginInstance (props), infoOwner (std::move (i)), info (*infoOwner), description (d), session (std::move (s))
{
    int index = 0;
    for (auto& p : info.params)
        juce::AudioProcessor::addParameter (new Param (*this, index++, p));
    setLatencySamples (info.latency);
    reportedLatency = info.latency;
    tailSeconds = info.tail;

    session->setSink ([this, token = alive] (int type, const juce::MemoryBlock& payload)
    {
        if (token->value.load())
            handleNotification (type, payload);
    });
    startTimer (250);
}

PluginProxy::~PluginProxy()
{
    stopTimer();
    alive->value = false;
    std::shared_ptr<Session> s;
    {
        const juce::ScopedLock sl (swapLock);
        s = std::move (session);
    }
    if (s != nullptr)
        s->setSink (nullptr);
    Session::retire (std::move (s));   // (ends the helper)
}

std::shared_ptr<Session> PluginProxy::currentSession() const
{
    const juce::ScopedLock sl (swapLock);
    return session;
}

juce::String PluginProxy::failureReason() const
{
    const juce::ScopedLock sl (stateLock);
    return failureText;
}

void PluginProxy::killHelper()
{
    if (auto s = currentSession())
        s->kill();
}

int PluginProxy::getHelperProcessId() const
{
    auto s = currentSession();
    return s != nullptr ? s->helperPid : 0;
}

//==============================================================================
// failure handling

void PluginProxy::markFailed (int code)
{
    int expected = 0;
    if (failureCode.compare_exchange_strong (expected, code))
        failed = true;
}

void PluginProxy::timerCallback()
{
    auto s = currentSession();
    if (! failed.load() && s != nullptr && ! s->alive())
        markFailed (2);

    if (failed.load() && ! failureReported)
    {
        failureReported = true;
        juce::String text;
        switch (failureCode.load())
        {
            case 1:  text = description.name + " stopped answering while it was processing sound."; break;
            case 2:  text = description.name + "'s plugin process ended" + (s != nullptr && s->exitCode() != 0 && s->exitCode() != 259
                                                                              ? " (error code 0x" + juce::String::toHexString ((int) s->exitCode()).toUpperCase() + ")" : juce::String()) + "."; break;
            case 3:  text = description.name + " crashed while it was processing sound."; break;
            default: text = description.name + " could not be run."; break;
        }
        {
            const juce::ScopedLock sl (stateLock);
            if (helperText.isNotEmpty())
                text = helperText;
            failureText = text;
        }
        if (s != nullptr)
            s->kill();
        editorIsOpen = false;
        if (onFailed)
            onFailed();
    }

    const int latency = reportedLatency.load();
    if (latency != getLatencySamples())
        setLatencySamples (latency);
}

void PluginProxy::handleNotification (int type, const juce::MemoryBlock& payload)
{
    juce::MemoryInputStream in (payload, false);
    auto token = alive;
    switch (type)
    {
        case ParamChanged:
        {
            const int index = in.readInt();
            const float value = in.readFloat();
            if (juce::isPositiveAndBelow (index, getParameters().size()))
            {
                static_cast<Param*> (getParameters()[index])->value = value;
                juce::MessageManager::callAsync ([this, token, index, value]
                {
                    if (token->value.load() && juce::isPositiveAndBelow (index, getParameters().size()))
                        getParameters()[index]->sendValueChangedMessageToListeners (value);
                });
            }
            break;
        }
        case GestureBegin:
        case GestureEnd:
        {
            const int index = in.readInt();
            const bool begin = type == GestureBegin;
            juce::MessageManager::callAsync ([this, token, index, begin]
            {
                if (token->value.load() && juce::isPositiveAndBelow (index, getParameters().size()))
                {
                    if (begin) getParameters()[index]->beginChangeGesture();
                    else       getParameters()[index]->endChangeGesture();
                }
            });
            break;
        }
        case LatencyChanged:
            reportedLatency = in.readInt();
            tailSeconds = in.readDouble();
            break;
        case EditorClosed:
            juce::MessageManager::callAsync ([this, token]
            {
                if (! token->value.load())
                    return;
                editorIsOpen = false;
                if (onEditorClosed)
                    onEditorClosed();
            });
            break;
        case HelperFailed:
        {
            {
                const juce::ScopedLock sl (stateLock);
                helperText = in.readString();
            }
            markFailed (3);
            break;
        }
        default: break;
    }
}

//==============================================================================
// parameters

void PluginProxy::queueParameter (int index, float value)
{
    if (failed.load())
        return;
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        if (auto s = currentSession())
        {
            juce::MemoryOutputStream out;
            out.writeInt (index);
            out.writeFloat (value);
            s->post (SetParam, out.getMemoryBlock());
        }
        return;
    }
    // another thread (the audio thread, automation): travels with the next block. Never wait for the message thread.
    std::unique_lock<std::mutex> lock (paramMutex, std::try_to_lock);
    if (! lock.owns_lock())
        return;
    for (auto& p : pendingParams)
        if (p.first == index) { p.second = value; return; }
    if ((int) pendingParams.size() < maxParamChanges)
        pendingParams.push_back ({ index, value });
}

juce::String PluginProxy::parameterText (int index, float value)
{
    if (! failed.load() && juce::MessageManager::getInstance()->isThisTheMessageThread())
        if (auto s = currentSession())
        {
            juce::MemoryOutputStream out;
            out.writeInt (index);
            out.writeFloat (value);
            juce::MemoryBlock reply;
            if (s->request (GetParamText, out.getMemoryBlock(), reply, 300))
                return reply.toString();
        }
    return juce::String (value, 2);
}

float PluginProxy::parameterValueForText (int index, const juce::String& text)
{
    if (! failed.load() && juce::MessageManager::getInstance()->isThisTheMessageThread())
        if (auto s = currentSession())
        {
            juce::MemoryOutputStream out;
            out.writeInt (index);
            out.writeString (text);
            juce::MemoryBlock reply;
            if (s->request (GetParamValue, out.getMemoryBlock(), reply, 300) && reply.getSize() >= 4)
            {
                float v;
                std::memcpy (&v, reply.getData(), 4);
                return v;
            }
        }
    return text.getFloatValue();
}

// (after a state was loaded: the helper tells every parameter's value)
void PluginProxy::applyValues (const juce::MemoryBlock& reply)
{
    juce::MemoryInputStream in (reply, false);
    const int count = in.readInt();
    for (int i = 0; i < count && i < getParameters().size() && ! in.isExhausted(); ++i)
    {
        const float v = in.readFloat();
        auto* p = static_cast<Param*> (getParameters()[i]);
        if (p->value.load() != v)
        {
            p->value = v;
            p->sendValueChangedMessageToListeners (v);
        }
    }
}

//==============================================================================
// the AudioProcessor interface (message thread)

void PluginProxy::prepareToPlay (double rate, int block)
{
    sampleRate = rate;
    blockSize = block;
    blocksSincePrepare = 0;
    auto s = currentSession();
    if (failed.load() || s == nullptr)
        return;

    juce::MemoryOutputStream out;
    out.writeDouble (rate);
    out.writeInt (block);
    ++controlBusy;
    juce::MemoryBlock reply;
    const bool ok = s->request (Prepare, out.getMemoryBlock(), reply, 60000);
    --controlBusy;
    if (ok && reply.getSize() >= 12)
    {
        juce::MemoryInputStream in (reply, false);
        reportedLatency = in.readInt();
        tailSeconds = in.readDouble();
        setLatencySamples (reportedLatency.load());
    }
}

void PluginProxy::releaseResources()
{
    if (auto s = currentSession(); s != nullptr && ! failed.load())
        s->post (Release);
}

void PluginProxy::reset()
{
    resetPending = true;   // (applied with the next block: no waiting on the audio thread)
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        if (auto s = currentSession(); s != nullptr && ! failed.load())
            s->post (Reset);
}

void PluginProxy::setCurrentProgram (int index)
{
    currentProgram = index;
    if (auto s = currentSession(); s != nullptr && ! failed.load())
    {
        juce::MemoryOutputStream out;
        out.writeInt (index);
        s->post (SetProgram, out.getMemoryBlock());
    }
}

const juce::String PluginProxy::getProgramName (int index)
{
    if (! failed.load() && juce::MessageManager::getInstance()->isThisTheMessageThread())
        if (auto s = currentSession())
        {
            juce::MemoryOutputStream out;
            out.writeInt (index);
            juce::MemoryBlock reply;
            if (s->request (GetProgramName, out.getMemoryBlock(), reply, 300))
                return reply.toString();
        }
    return {};
}

void PluginProxy::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto s = currentSession(); s != nullptr && ! failed.load())
    {
        juce::MemoryBlock reply;
        if (s->request (GetState, {}, reply, 30000))
        {
            {
                const juce::ScopedLock sl (stateLock);
                cachedState = reply;
            }
            dest = reply;
            return;
        }
    }
    const juce::ScopedLock sl (stateLock);   // the helper is gone: the last state it was known to have
    dest = cachedState;
}

void PluginProxy::setStateInformation (const void* data, int size)
{
    {
        const juce::ScopedLock sl (stateLock);
        cachedState = juce::MemoryBlock (data, (size_t) size);
    }
    if (auto s = currentSession(); s != nullptr && ! failed.load())
    {
        ++controlBusy;
        juce::MemoryBlock reply;
        const bool ok = s->request (SetState, juce::MemoryBlock (data, (size_t) size), reply, 60000);
        --controlBusy;
        if (ok)
            applyValues (reply);
    }
}

bool PluginProxy::isBusesLayoutSupported (const BusesLayout& layout) const
{
    const auto key = layoutBlock (layout).toBase64Encoding();
    {
        const juce::ScopedLock sl (layoutLock);
        if (auto it = layoutCache.find (key); it != layoutCache.end())
            return it->second;
    }
    bool result = true;
    if (auto s = currentSession(); s != nullptr && ! failed.load())
    {
        juce::MemoryBlock reply;
        if (s->request (CheckLayout, layoutBlock (layout), reply, 20000))
            result = reply.getSize() > 0 && ((const char*) reply.getData())[0] != 0;
    }
    const juce::ScopedLock sl (layoutLock);
    layoutCache[key] = result;
    return result;
}

void PluginProxy::processorLayoutsChanged()
{
    if (auto s = currentSession(); s != nullptr && ! failed.load())
    {
        ++controlBusy;
        juce::MemoryBlock reply;
        s->request (ChangeLayout, layoutBlock (getBusesLayout()), reply, 20000);
        --controlBusy;
    }
}

void PluginProxy::showEditor (const juce::String& title)
{
    if (auto s = currentSession(); s != nullptr && ! failed.load())
    {
        editorIsOpen = true;
        AllowSetForegroundWindow ((DWORD) s->helperPid);   // (the window of the other process may come to the front)
        juce::MemoryOutputStream out;
        out.writeString (title);
        s->post (ShowEditor, out.getMemoryBlock());
    }
}

void PluginProxy::closeEditor()
{
    if (editorIsOpen.exchange (false))
        if (auto s = currentSession(); s != nullptr && ! failed.load())
            s->post (CloseEditor);
}

bool PluginProxy::getHelperStats (juce::int64& privateBytes, int& gdi, int& user)
{
    auto s = currentSession();
    if (s == nullptr || failed.load())
        return false;
    juce::MemoryBlock reply;
    if (! s->request (Ping, {}, reply, 1000))
        return false;
    juce::MemoryInputStream in (reply, false);
    privateBytes = in.readInt64();
    gdi = in.readInt();
    user = in.readInt();
    return true;
}

PluginProxy::Timing PluginProxy::getTiming() const
{
    Timing t;
    t.blocks = timingBlocks.load();
    const double toMicros = 1.0e6 / (double) juce::Time::getHighResolutionTicksPerSecond();
    t.averageMicros = t.blocks > 0 ? (double) timingTicks.load() * toMicros / (double) t.blocks : 0.0;
    t.worstMicros = (double) timingWorst.load() * toMicros;
    return t;
}

void PluginProxy::resetTiming()
{
    timingTicks = 0; timingWorst = 0; timingBlocks = 0;
}

bool PluginProxy::restart (juce::String& error)
{
    juce::String why;
    auto fresh = Session::launch (helperExecutable, why);
    if (fresh == nullptr)
    {
        error = why;
        return false;
    }

    Info loaded;
    if (! loadInHelper (*fresh, description, sampleRate, blockSize, loaded, why))
    {
        error = why;
        return false;
    }
    if (loaded.params.size() != info.params.size())
    {
        error = "The plugin changed its parameters";
        return false;
    }

    juce::MemoryBlock reply;
    fresh->request (ChangeLayout, layoutBlock (getBusesLayout()), reply, 20000);
    {
        juce::MemoryBlock state;
        {
            const juce::ScopedLock sl (stateLock);
            state = cachedState;
        }
        if (state.getSize() > 0 && fresh->request (SetState, state, reply, 60000))
            applyValues (reply);
    }
    juce::MemoryOutputStream out;
    out.writeDouble (sampleRate);
    out.writeInt (blockSize);
    if (fresh->request (Prepare, out.getMemoryBlock(), reply, 60000) && reply.getSize() >= 12)
    {
        juce::MemoryInputStream in (reply, false);
        reportedLatency = in.readInt();
        tailSeconds = in.readDouble();
    }
    // (parameters moved since the state was saved)
    if (currentProgram != 0)
    {
        juce::MemoryOutputStream program;
        program.writeInt (currentProgram);
        fresh->post (SetProgram, program.getMemoryBlock());
    }

    fresh->setSink ([this, token = alive] (int type, const juce::MemoryBlock& payload)
    {
        if (token->value.load())
            handleNotification (type, payload);
    });

    std::shared_ptr<Session> old;
    {
        const juce::ScopedLock sl (swapLock);   // (waits for a block that is being processed)
        old = std::move (session);
        session = fresh;
        blocksSincePrepare = 0;
        failureCode = 0;
        failureReported = false;
        failed = false;
        editorIsOpen = false;
    }
    {
        const juce::ScopedLock sl (stateLock);
        failureText.clear();
        helperText.clear();
    }
    if (old != nullptr)
        old->setSink (nullptr);
    Session::retire (std::move (old));
    setLatencySamples (reportedLatency.load());
    return true;
}

//==============================================================================
// the audio thread

void PluginProxy::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    if (failed.load())
    {
        buffer.clear();
        midi.clear();
        return;
    }
    if (controlBusy.load() > 0)   // the helper is busy loading a state or preparing: this block is skipped (the sound is not changed; MIDI is lost)
    {
        midi.clear();
        return;
    }
    const juce::ScopedTryLock tryLock (swapLock);
    if (! tryLock.isLocked() || session == nullptr || session->shm == nullptr)
    {
        buffer.clear();
        midi.clear();
        return;
    }

    Session& s = *session;
    Shared& shared = *s.shm;
    const int total = buffer.getNumSamples();
    const int channels = juce::jmin (buffer.getNumChannels(), maxChannels);

    juce::Optional<juce::AudioPlayHead::PositionInfo> position;
    if (auto* head = getPlayHead())
        position = head->getPosition();

    midiScratch.clear();
    const auto started = juce::Time::getHighResolutionTicks();

    for (int start = 0; start < total; start += maxBlock)
    {
        const int n = juce::jmin (maxBlock, total - start);
        for (int c = 0; c < channels; ++c)
            std::memcpy (shared.audio[c], buffer.getReadPointer (c) + start, (size_t) n * sizeof (float));

        shared.numSamples = n;
        shared.numChannels = channels;
        shared.midiInBytes = writeMidi (midi, start, n, shared.midiIn, midiAreaBytes);
        shared.flags = 0;
        shared.numParamChanges = 0;
        if (start == 0)
        {
            if (resetPending.exchange (false))
                shared.flags |= 1;
            std::unique_lock<std::mutex> lock (paramMutex, std::try_to_lock);
            if (lock.owns_lock())
            {
                for (auto& p : pendingParams)
                    shared.params[shared.numParamChanges++] = { p.first, p.second };
                pendingParams.clear();
            }
        }
        shared.hasPosition = position.hasValue() ? 1 : 0;
        if (position.hasValue())
        {
            const auto& p = *position;
            shared.isPlaying = p.getIsPlaying(); shared.isRecording = p.getIsRecording(); shared.isLooping = p.getIsLooping();
            shared.bpm = p.getBpm().orFallback (120.0);
            const auto sig = p.getTimeSignature().orFallback (juce::AudioPlayHead::TimeSignature());
            shared.timeSigNum = sig.numerator; shared.timeSigDen = sig.denominator;
            const juce::int64 samples = p.getTimeInSamples().orFallback (0);
            shared.timeSamples = samples + start;
            shared.timeSeconds = p.getTimeInSeconds().orFallback (0.0) + (sampleRate > 0 ? (double) start / sampleRate : 0.0);
            shared.ppq = p.getPpqPosition().orFallback (0.0);
            if (sampleRate > 0 && shared.bpm > 0)
                shared.ppq += (double) start / sampleRate * shared.bpm / 60.0;
            shared.barStartPpq = p.getPpqPositionOfLastBarStart().orFallback (0.0);
            const auto loop = p.getLoopPoints();
            shared.hasLoop = loop.hasValue();
            shared.loopStartPpq = loop.hasValue() ? loop->ppqStart : 0.0;
            shared.loopEndPpq = loop.hasValue() ? loop->ppqEnd : 0.0;
        }

        SetEvent (s.requestEvent);
        HANDLE handles[2] = { s.doneEvent, s.process };
        const int timeout = blocksSincePrepare.load() == 0 ? juce::jmax (3000, blockTimeoutMs.load()) : blockTimeoutMs.load();
        const DWORD result = WaitForMultipleObjects (2, handles, FALSE, (DWORD) timeout);
        if (result != WAIT_OBJECT_0)
        {
            markFailed (result == WAIT_TIMEOUT ? 1 : 2);
            s.kill();
            buffer.clear();
            midi.clear();
            return;
        }
        if (shared.status != 0)
        {
            markFailed (3);
            s.kill();
            buffer.clear();
            midi.clear();
            return;
        }

        for (int c = 0; c < channels; ++c)
            std::memcpy (buffer.getWritePointer (c) + start, shared.audio[c], (size_t) n * sizeof (float));
        readMidi (shared.midiOut, shared.midiOutBytes, start, midiScratch);
        reportedLatency = shared.latency;
        tailSeconds = shared.tailSeconds;
    }

    midi.swapWith (midiScratch);
    ++blocksSincePrepare;
    ++blocksProcessed;

    const auto took = juce::Time::getHighResolutionTicks() - started;
    timingTicks += took;
    ++timingBlocks;
    if (took > timingWorst.load())
        timingWorst = took;
}

#else   // not Windows: plugins stay in the program

bool PluginProxy::isSupported() { return false; }
std::unique_ptr<PluginProxy> PluginProxy::create (const juce::PluginDescription&, double, int, juce::String& error, bool* helperFailedToStart)
{
    error = "Plugins in a separate process are only available on Windows";
    if (helperFailedToStart != nullptr)
        *helperFailedToStart = true;
    return nullptr;
}

#endif
