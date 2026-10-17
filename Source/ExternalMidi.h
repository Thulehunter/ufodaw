#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <functional>
#include <map>

// Sends MIDI to hardware (a synth on the interface's MIDI OUT, a USB MIDI device) at the right moment.
// The audio thread only drops messages into a lock-free queue, each stamped with the time it should leave;
// a high-priority thread of its own waits for that time and writes them to the device.
class ExternalMidiSender : private juce::Thread
{
public:
    ExternalMidiSender();
    ~ExternalMidiSender() override;

    // Message thread. Returns the port number of the output (opened on first use; ports are never renumbered),
    // or -1 if it cannot be opened (not connected, or in use by another program).
    int openDevice (const juce::String& identifier);
    int openDeviceByName (const juce::String& name);   // when the identifier has changed since the project was saved
    juce::String getDeviceName (int port) const;

    // Any thread. `atMillisecondCounter` is on the juce::Time::getMillisecondCounterHiRes() clock; a time
    // in the past means "now".
    void send (int port, const juce::MidiMessage&, double atMillisecondCounter);

    // For tests: when set, messages go here instead of to a device.
    std::function<void (int port, const juce::MidiMessage&)> testSink;

private:
    struct Item
    {
        int port = 0;
        juce::uint8 bytes[3] = {};
        int length = 0;
        double timeMs = 0.0;
    };

    void run() override;
    void deliver (const Item&);

    mutable juce::CriticalSection portsLock;
    std::vector<std::pair<juce::String, std::unique_ptr<juce::MidiOutput>>> ports;   // append-only

    juce::SpinLock writeLock;   // one producer at a time: the audio thread, or the message thread for all-notes-off
    juce::AbstractFifo fifo { 2048 };
    std::array<Item, 2048> ring;
};
