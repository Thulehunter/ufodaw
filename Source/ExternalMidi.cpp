#include "ExternalMidi.h"

#if JUCE_WINDOWS
 // Windows' default timer tick is ~15 ms; ask for 1 ms while the sender runs so MIDI timing stays tight.
 extern "C"
 {
     __declspec (dllimport) unsigned int __stdcall timeBeginPeriod (unsigned int);
     __declspec (dllimport) unsigned int __stdcall timeEndPeriod (unsigned int);
 }
#endif

ExternalMidiSender::ExternalMidiSender() : juce::Thread ("MiniDAW external MIDI")
{
    startThread (juce::Thread::Priority::high);
}

ExternalMidiSender::~ExternalMidiSender()
{
    stopThread (1000);
}

int ExternalMidiSender::openDevice (const juce::String& identifier)
{
    if (identifier.isEmpty())
        return -1;

    const juce::ScopedLock sl (portsLock);
    for (size_t i = 0; i < ports.size(); ++i)
        if (ports[i].first == identifier)
            return ports[i].second != nullptr ? (int) i : -1;

    auto output = juce::MidiOutput::openDevice (identifier);
    ports.emplace_back (identifier, std::move (output));
    return ports.back().second != nullptr ? (int) ports.size() - 1 : -1;
}

int ExternalMidiSender::openDeviceByName (const juce::String& name)
{
    for (auto& d : juce::MidiOutput::getAvailableDevices())
        if (d.name == name)
            return openDevice (d.identifier);
    return -1;
}

juce::String ExternalMidiSender::getDeviceName (int port) const
{
    const juce::ScopedLock sl (portsLock);
    if (juce::isPositiveAndBelow (port, (int) ports.size()) && ports[(size_t) port].second != nullptr)
        return ports[(size_t) port].second->getName();
    return {};
}

void ExternalMidiSender::send (int port, const juce::MidiMessage& message, double atMs)
{
    if (port < 0 || message.getRawDataSize() > 3)
        return;

    Item item;
    item.port = port;
    item.length = message.getRawDataSize();
    for (int i = 0; i < item.length; ++i)
        item.bytes[i] = message.getRawData()[i];
    item.timeMs = atMs;

    const juce::SpinLock::ScopedLockType sl (writeLock);
    int s1, n1, s2, n2;
    fifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0)      ring[(size_t) s1] = item;
    else if (n2 > 0) ring[(size_t) s2] = item;
    fifo.finishedWrite (n1 + n2);   // (a full queue drops the message rather than block the audio thread)
}

void ExternalMidiSender::deliver (const Item& item)
{
    const juce::MidiMessage message (item.bytes, item.length, 0.0);
    if (testSink)
    {
        testSink (item.port, message);
        return;
    }

    const juce::ScopedLock sl (portsLock);
    if (juce::isPositiveAndBelow (item.port, (int) ports.size()) && ports[(size_t) item.port].second != nullptr)
        ports[(size_t) item.port].second->sendMessageNow (message);
}

void ExternalMidiSender::run()
{
#if JUCE_WINDOWS
    timeBeginPeriod (1);
#endif

    std::multimap<double, Item> pending;   // sorted by the time they are due

    while (! threadShouldExit())
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (fifo.getNumReady(), s1, n1, s2, n2);
        for (int i = 0; i < n1; ++i) pending.emplace (ring[(size_t) (s1 + i)].timeMs, ring[(size_t) (s1 + i)]);
        for (int i = 0; i < n2; ++i) pending.emplace (ring[(size_t) (s2 + i)].timeMs, ring[(size_t) (s2 + i)]);
        fifo.finishedRead (n1 + n2);

        while (! pending.empty() && pending.begin()->first <= juce::Time::getMillisecondCounterHiRes())
        {
            deliver (pending.begin()->second);
            pending.erase (pending.begin());
        }

        if (pending.empty())
            wait (2);
        else if (pending.begin()->first - juce::Time::getMillisecondCounterHiRes() > 2.5)
            wait (1);   // otherwise spin for the last couple of milliseconds
    }

#if JUCE_WINDOWS
    timeEndPeriod (1);
#endif
}
