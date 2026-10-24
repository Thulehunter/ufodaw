#include "ClapFormat.h"
#include <clap/all.h>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace
{
//==============================================================================
// A loaded .clap file (shared by every plugin of that file; the entry point is initialised once and ended with the last user)
struct ClapLibrary
{
    juce::DynamicLibrary lib;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_factory_t* factory = nullptr;
    bool initialised = false;

    ~ClapLibrary()
    {
        if (initialised && entry != nullptr)
            entry->deinit();
    }

    static std::shared_ptr<ClapLibrary> open (const juce::File& file, juce::String& error)
    {
        static std::mutex mutex;
        static std::map<juce::String, std::weak_ptr<ClapLibrary>> cache;
        const std::lock_guard<std::mutex> lock (mutex);

        const auto path = file.getFullPathName();
        if (auto it = cache.find (path); it != cache.end())
            if (auto existing = it->second.lock())
                return existing;

        auto library = std::make_shared<ClapLibrary>();
       #if JUCE_WINDOWS
        SetDllDirectoryW (file.getParentDirectory().getFullPathName().toWideCharPointer());   // (the plugin's own DLLs sit next to it)
        const bool opened = library->lib.open (path);
        SetDllDirectoryW (nullptr);
       #else
        const bool opened = library->lib.open (path);
       #endif
        if (! opened)
        {
            error = "The file could not be loaded: " + file.getFileName();
            return nullptr;
        }
        library->entry = static_cast<const clap_plugin_entry_t*> (library->lib.getFunction ("clap_entry"));
        if (library->entry == nullptr || library->entry->init == nullptr || library->entry->get_factory == nullptr)
        {
            error = file.getFileName() + " is not a CLAP plugin.";
            return nullptr;
        }
        if (! library->entry->init (path.toRawUTF8()))
        {
            error = "The plugin did not start.";
            return nullptr;
        }
        library->initialised = true;
        library->factory = static_cast<const clap_plugin_factory_t*> (library->entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
        if (library->factory == nullptr)
        {
            error = file.getFileName() + " has no plugins.";
            return nullptr;
        }
        cache[path] = library;
        return library;
    }
};

int idHash (const char* id) { return juce::String (id != nullptr ? id : "").hashCode(); }

//==============================================================================
// The host the plugins talk to
class ClapPluginInstance;
struct HostCalls;

const clap_host_t* makeHost (void* data);

//==============================================================================
class ClapPluginInstance : public juce::AudioPluginInstance, private juce::AsyncUpdater
{
public:
    ClapPluginInstance (const juce::AudioProcessor::BusesProperties& buses, std::shared_ptr<ClapLibrary> l, uint32_t index, const juce::PluginDescription& d)
        : juce::AudioPluginInstance (buses), library (std::move (l)), descriptorIndex (index), description (d)
    {
        host = *makeHost (this);
    }

    static std::unique_ptr<ClapPluginInstance> create (std::shared_ptr<ClapLibrary> l, uint32_t index, const juce::PluginDescription& d)
    {
        BusesProperties buses;
        if (d.numInputChannels > 0)
            buses = buses.withInput ("Input", juce::AudioChannelSet::canonicalChannelSet (juce::jlimit (1, 2, d.numInputChannels)), true);
        if (d.numOutputChannels > 0)
            buses = buses.withOutput ("Output", juce::AudioChannelSet::canonicalChannelSet (juce::jlimit (1, 2, d.numOutputChannels)), true);
        return std::make_unique<ClapPluginInstance> (buses, std::move (l), index, d);
    }

    ~ClapPluginInstance() override
    {
        cancelPendingUpdate();
        if (plugin != nullptr)
        {
            if (processing)
                plugin->stop_processing (plugin);
            if (active)
                plugin->deactivate (plugin);
            plugin->destroy (plugin);
            plugin = nullptr;
        }
    }

    // Makes the plugin; returns an error text, empty when it worked.
    juce::String init()
    {
        const auto* desc = library->factory->get_plugin_descriptor (library->factory, descriptorIndex);
        if (desc == nullptr)
            return "The plugin is not in the file any more.";
        plugin = library->factory->create_plugin (library->factory, &host, desc->id);
        if (plugin == nullptr)
            return "The plugin could not be created.";
        if (! plugin->init (plugin))
        {
            plugin->destroy (plugin);
            plugin = nullptr;
            return "The plugin did not start.";
        }
        name = desc->name != nullptr ? desc->name : "CLAP plugin";

        audioPorts = static_cast<const clap_plugin_audio_ports_t*> (plugin->get_extension (plugin, CLAP_EXT_AUDIO_PORTS));
        notePorts = static_cast<const clap_plugin_note_ports_t*> (plugin->get_extension (plugin, CLAP_EXT_NOTE_PORTS));
        paramsExt = static_cast<const clap_plugin_params_t*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
        stateExt = static_cast<const clap_plugin_state_t*> (plugin->get_extension (plugin, CLAP_EXT_STATE));
        latencyExt = static_cast<const clap_plugin_latency_t*> (plugin->get_extension (plugin, CLAP_EXT_LATENCY));
        tailExt = static_cast<const clap_plugin_tail_t*> (plugin->get_extension (plugin, CLAP_EXT_TAIL));
        guiExt = static_cast<const clap_plugin_gui_t*> (plugin->get_extension (plugin, CLAP_EXT_GUI));

        readPorts();
        if (notePorts != nullptr && notePorts->count (plugin, true) > 0)
        {
            clap_note_port_info_t info {};
            if (notePorts->get (plugin, 0, true, &info))
            {
                midiNotes = (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) == 0 && (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0;
                midiDialect = (info.supported_dialects & (CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE)) != 0;
            }
            takesNotes = true;
        }
        else
        {
            takesNotes = description.isInstrument;
            midiDialect = false;
        }

        if (paramsExt != nullptr)
        {
            const uint32_t count = paramsExt->count (plugin);
            for (uint32_t i = 0; i < count; ++i)
            {
                clap_param_info_t info {};
                if (! paramsExt->get_info (plugin, i, &info) || (info.flags & CLAP_PARAM_IS_HIDDEN) != 0)
                    continue;
                auto* p = new Param (*this, info);
                double value = info.default_value;
                paramsExt->get_value (plugin, info.id, &value);
                p->current = value;
                byId[info.id] = p;
                juce::AudioProcessor::addParameter (p);
            }
        }

        if (guiExt != nullptr)
        {
            if (guiExt->is_api_supported (plugin, CLAP_WINDOW_API_WIN32, false))
                editorMode = EditorMode::embedded;
            else if (guiExt->is_api_supported (plugin, CLAP_WINDOW_API_WIN32, true))
                editorMode = EditorMode::floating;
        }
        return {};
    }

    //==============================================================================
    const juce::String getName() const override { return name; }
    double getTailLengthSeconds() const override
    {
        if (tailExt == nullptr || plugin == nullptr || sampleRate <= 0.0)
            return 0.0;
        const uint32_t t = tailExt->get (plugin);
        return t == UINT32_MAX ? 10.0 : (double) t / sampleRate;
    }
    bool acceptsMidi() const override { return takesNotes; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        for (auto& b : layouts.inputBuses)  if (b.size() < 1 || b.size() > 2) return false;
        for (auto& b : layouts.outputBuses) if (b.size() < 1 || b.size() > 2) return false;
        return true;
    }

    void fillInPluginDescription (juce::PluginDescription& d) const override { d = description; }

    void prepareToPlay (double rate, int maxBlock) override
    {
        if (plugin == nullptr)
            return;
        if (processing) { plugin->stop_processing (plugin); processing = false; }
        if (active)     { plugin->deactivate (plugin); active = false; }

        sampleRate = rate;
        maxFrames = juce::jmax (16, maxBlock);
        allocateBuffers();
        failed = ! plugin->activate (plugin, rate, 1, (uint32_t) maxFrames);
        active = ! failed;
        events.reserve (4096);
        if (latencyExt != nullptr && active)
            setLatencySamples ((int) latencyExt->get (plugin));
        steady = 0;
    }

    void releaseResources() override
    {
        if (plugin == nullptr)
            return;
        if (processing) { plugin->stop_processing (plugin); processing = false; }
        if (active)     { plugin->deactivate (plugin); active = false; }
    }

    void reset() override
    {
        if (plugin != nullptr && active && plugin->reset != nullptr)
            plugin->reset (plugin);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;
        const int n = buffer.getNumSamples();
        if (! active || failed || n > maxFrames || n <= 0)
        {
            if (getTotalNumInputChannels() == 0)
                buffer.clear();
            return;
        }
        if (! processing)
        {
            if (plugin->start_processing != nullptr && ! plugin->start_processing (plugin))
            {
                failed = true;
                return;
            }
            processing = true;
        }

        // ---- events: the parameter changes made outside the plugin, then the notes and controllers of this block
        events.clear();
        {
            ParamChange change;
            while (popParamChange (change))
            {
                Event e {};
                e.param.header = { sizeof (clap_event_param_value_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
                e.param.param_id = change.id;
                e.param.cookie = change.cookie;
                e.param.note_id = -1; e.param.port_index = -1; e.param.channel = -1; e.param.key = -1;
                e.param.value = change.value;
                push (e);
            }
        }
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            const uint32_t at = (uint32_t) juce::jlimit (0, n - 1, meta.samplePosition);
            Event e {};
            if ((m.isNoteOn() || m.isNoteOff()) && ! midiNotes)
            {
                e.note.header = { sizeof (clap_event_note_t), at, CLAP_CORE_EVENT_SPACE_ID, (uint16_t) (m.isNoteOn() ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), CLAP_EVENT_IS_LIVE };
                e.note.note_id = -1;
                e.note.port_index = 0;
                e.note.channel = (int16_t) juce::jmax (0, m.getChannel() - 1);
                e.note.key = (int16_t) m.getNoteNumber();
                e.note.velocity = m.isNoteOn() ? (double) m.getFloatVelocity() : 0.0;
                push (e);
            }
            else if (midiDialect && m.getRawDataSize() >= 1 && m.getRawDataSize() <= 3 && ! m.isSysEx())
            {
                e.midi.header = { sizeof (clap_event_midi_t), at, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, CLAP_EVENT_IS_LIVE };
                e.midi.port_index = 0;
                std::memcpy (e.midi.data, m.getRawData(), (size_t) m.getRawDataSize());
                push (e);
            }
        }
        midi.clear();

        // ---- audio
        for (size_t p = 0; p < inPorts.size(); ++p)
        {
            auto& port = inPorts[p];
            for (int c = 0; c < port.channels; ++c)
            {
                float* dest = port.data[(size_t) c];
                const int inCh = getTotalNumInputChannels();
                if (p == 0 && inCh > 0)
                    std::memcpy (dest, buffer.getReadPointer (juce::jmin (c, inCh - 1)), sizeof (float) * (size_t) n);
                else
                    std::fill (dest, dest + n, 0.0f);
            }
        }
        for (auto& port : outPorts)
            for (int c = 0; c < port.channels; ++c)
                std::fill (port.data[(size_t) c], port.data[(size_t) c] + n, 0.0f);

        clap_event_transport_t transport {};
        const clap_event_transport_t* transportPtr = nullptr;
        if (auto* head = getPlayHead())
            if (auto pos = head->getPosition())
            {
                transport.header = { sizeof (clap_event_transport_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
                if (auto bpm = pos->getBpm())
                {
                    transport.flags |= CLAP_TRANSPORT_HAS_TEMPO;
                    transport.tempo = *bpm;
                }
                if (auto ppq = pos->getPpqPosition())
                {
                    transport.flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
                    transport.song_pos_beats = (clap_beattime) std::llround (*ppq * (double) CLAP_BEATTIME_FACTOR);
                }
                if (auto secs = pos->getTimeInSeconds())
                {
                    transport.flags |= CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
                    transport.song_pos_seconds = (clap_sectime) std::llround (*secs * (double) CLAP_SECTIME_FACTOR);
                }
                if (auto sig = pos->getTimeSignature())
                {
                    transport.flags |= CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
                    transport.tsig_num = (uint16_t) sig->numerator;
                    transport.tsig_denom = (uint16_t) sig->denominator;
                }
                if (pos->getIsPlaying())   transport.flags |= CLAP_TRANSPORT_IS_PLAYING;
                if (pos->getIsRecording()) transport.flags |= CLAP_TRANSPORT_IS_RECORDING;
                transportPtr = &transport;
            }

        clap_input_events_t in {};
        in.ctx = this;
        in.size = [] (const clap_input_events_t* l) { return (uint32_t) static_cast<ClapPluginInstance*> (l->ctx)->events.size(); };
        in.get = [] (const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t*
        {
            auto& list = static_cast<ClapPluginInstance*> (l->ctx)->events;
            return i < list.size() ? &list[i].header : nullptr;
        };
        clap_output_events_t out {};
        out.ctx = this;
        out.try_push = [] (const clap_output_events_t* l, const clap_event_header_t* h)
        {
            static_cast<ClapPluginInstance*> (l->ctx)->outputEvent (*h);
            return true;
        };

        clap_process_t process {};
        process.steady_time = steady;
        process.frames_count = (uint32_t) n;
        process.transport = transportPtr;
        process.audio_inputs = inBuffers.data();
        process.audio_outputs = outBuffers.data();
        process.audio_inputs_count = (uint32_t) inBuffers.size();
        process.audio_outputs_count = (uint32_t) outBuffers.size();
        process.in_events = &in;
        process.out_events = &out;
        const auto status = plugin->process (plugin, &process);
        steady += n;

        if (status == CLAP_PROCESS_ERROR)
        {
            if (getTotalNumInputChannels() == 0)
                buffer.clear();
            return;
        }
        if (! outPorts.empty() && getTotalNumOutputChannels() > 0)
        {
            auto& port = outPorts[0];
            for (int c = 0; c < getTotalNumOutputChannels(); ++c)
                std::memcpy (buffer.getWritePointer (c), port.data[(size_t) juce::jmin (c, port.channels - 1)], sizeof (float) * (size_t) n);
        }
        else if (getTotalNumInputChannels() == 0)
        {
            buffer.clear();
        }
    }

    //==============================================================================
    bool hasEditor() const override { return editorMode != EditorMode::none; }
    juce::AudioProcessorEditor* createEditor() override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    // The parameter changes that have not reached the plugin yet (it is not processing: the transport stands still) are handed over now, so that a
    // state that is saved straight after a change has it.
    void flushPendingParams()
    {
        if (processing || plugin == nullptr || paramsExt == nullptr || paramsExt->flush == nullptr)
            return;
        events.clear();
        events.reserve (4096);
        ParamChange change;
        while (popParamChange (change))
        {
            Event e {};
            e.param.header = { sizeof (clap_event_param_value_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
            e.param.param_id = change.id;
            e.param.cookie = change.cookie;
            e.param.note_id = -1; e.param.port_index = -1; e.param.channel = -1; e.param.key = -1;
            e.param.value = change.value;
            push (e);
        }
        clap_input_events_t in {};
        in.ctx = this;
        in.size = [] (const clap_input_events_t* l) { return (uint32_t) static_cast<ClapPluginInstance*> (l->ctx)->events.size(); };
        in.get = [] (const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t*
        {
            auto& list = static_cast<ClapPluginInstance*> (l->ctx)->events;
            return i < list.size() ? &list[i].header : nullptr;
        };
        clap_output_events_t out {};
        out.ctx = this;
        out.try_push = [] (const clap_output_events_t*, const clap_event_header_t*) { return true; };
        paramsExt->flush (plugin, &in, &out);
    }

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        if (plugin == nullptr || stateExt == nullptr)
            return;
        flushPendingParams();
        juce::MemoryOutputStream stream (dest, false);
        clap_ostream_t os {};
        os.ctx = &stream;
        os.write = [] (const clap_ostream_t* s, const void* data, uint64_t size) -> int64_t
        {
            return static_cast<juce::MemoryOutputStream*> (s->ctx)->write (data, (size_t) size) ? (int64_t) size : -1;
        };
        stateExt->save (plugin, &os);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (plugin == nullptr || stateExt == nullptr || size <= 0)
            return;
        juce::MemoryInputStream stream (data, (size_t) size, false);
        clap_istream_t is {};
        is.ctx = &stream;
        is.read = [] (const clap_istream_t* s, void* buffer, uint64_t n) -> int64_t
        {
            return (int64_t) static_cast<juce::MemoryInputStream*> (s->ctx)->read (buffer, (int) n);
        };
        stateExt->load (plugin, &is);
        if (paramsExt != nullptr)   // (the values the plugin has now)
            for (auto& kv : byId)
            {
                double v = kv.second->current;
                if (paramsExt->get_value (plugin, kv.first, &v))
                    kv.second->current = v;
            }
    }

    //==============================================================================
    // Called by the host callbacks
    void hostLatencyChanged() { latencyChanged = true; triggerAsyncUpdate(); }
    void hostCallbackRequested() { callbackRequested = true; triggerAsyncUpdate(); }
    bool hostRequestResize (uint32_t w, uint32_t h);
    void editorClosed (juce::AudioProcessorEditor* e) { if (e == editor) editor = nullptr; }

    const clap_plugin_t* getPlugin() const { return plugin; }
    const clap_plugin_gui_t* getGui() const { return guiExt; }
    bool isEmbedded() const { return editorMode == EditorMode::embedded; }

private:
    //==============================================================================
    union Event
    {
        clap_event_header_t header;
        clap_event_note_t note;
        clap_event_midi_t midi;
        clap_event_param_value_t param;
    };

    struct ParamChange { clap_id id = 0; void* cookie = nullptr; double value = 0.0; };

    class Param : public juce::AudioProcessorParameter
    {
    public:
        Param (ClapPluginInstance& o, const clap_param_info_t& info)
            : owner (o), id (info.id), cookie (info.cookie), flags (info.flags), lo (info.min_value), hi (info.max_value), def (info.default_value),
              name (juce::String::fromUTF8 (info.name)), module (juce::String::fromUTF8 (info.module))
        {
            if (hi <= lo)
                hi = lo + 1.0;
        }

        float getValue() const override { return (float) juce::jlimit (0.0, 1.0, (current.load() - lo) / (hi - lo)); }
        void setValue (float v) override
        {
            const double plain = lo + (double) v * (hi - lo);
            current = plain;
            owner.pushParamChange ({ id, cookie, plain });
        }
        float getDefaultValue() const override { return (float) ((def - lo) / (hi - lo)); }
        juce::String getName (int maxLength) const override
        {
            const auto full = module.isNotEmpty() && ! name.startsWithIgnoreCase (module) ? module.replaceCharacter ('/', ' ') + " " + name : name;
            return full.substring (0, maxLength);
        }
        juce::String getLabel() const override { return {}; }
        juce::String getText (float v, int maxLength) const override
        {
            char text[256] = {};
            const double plain = lo + (double) v * (hi - lo);
            if (owner.paramsExt != nullptr && owner.plugin != nullptr && owner.paramsExt->value_to_text (owner.plugin, id, plain, text, (uint32_t) sizeof (text)))
                return juce::String::fromUTF8 (text).substring (0, maxLength);
            return juce::String (plain, 2).substring (0, maxLength);
        }
        float getValueForText (const juce::String& text) const override
        {
            double plain = 0.0;
            if (owner.paramsExt != nullptr && owner.plugin != nullptr && owner.paramsExt->text_to_value (owner.plugin, id, text.toRawUTF8(), &plain))
                return (float) juce::jlimit (0.0, 1.0, (plain - lo) / (hi - lo));
            return (float) juce::jlimit (0.0, 1.0, (text.getDoubleValue() - lo) / (hi - lo));
        }
        int getNumSteps() const override
        {
            if ((flags & CLAP_PARAM_IS_STEPPED) != 0 && hi - lo >= 1.0 && hi - lo < 10000.0)
                return (int) std::lround (hi - lo) + 1;
            return AudioProcessorParameter::getNumSteps();
        }
        bool isDiscrete() const override { return (flags & CLAP_PARAM_IS_STEPPED) != 0; }
        bool isBoolean() const override { return (flags & CLAP_PARAM_IS_STEPPED) != 0 && hi - lo == 1.0; }
        bool isAutomatable() const override { return (flags & CLAP_PARAM_IS_READONLY) == 0; }

        std::atomic<double> current { 0.0 };

    private:
        ClapPluginInstance& owner;
        clap_id id;
        void* cookie;
        uint32_t flags;
        double lo, hi, def;
        juce::String name, module;
    };

    struct PortBuffers
    {
        int channels = 0;
        std::vector<std::vector<float>> storage;
        std::vector<float*> data;
    };

    enum class EditorMode { none, embedded, floating };

    void readPorts()
    {
        inPortChannels.clear();
        outPortChannels.clear();
        if (audioPorts == nullptr)
        {
            outPortChannels.push_back (2);
            if (! description.isInstrument)
                inPortChannels.push_back (2);
            return;
        }
        for (int dir = 0; dir < 2; ++dir)
        {
            const bool input = dir == 0;
            const uint32_t count = audioPorts->count (plugin, input);
            for (uint32_t i = 0; i < count && i < 8; ++i)
            {
                clap_audio_port_info_t info {};
                if (audioPorts->get (plugin, i, input, &info))
                    (input ? inPortChannels : outPortChannels).push_back (juce::jlimit (1, 2, (int) info.channel_count));
            }
        }
    }

    void allocateBuffers()
    {
        auto build = [this] (std::vector<PortBuffers>& ports, std::vector<clap_audio_buffer_t>& buffers, const std::vector<int>& channels)
        {
            ports.clear();
            buffers.clear();
            ports.resize (channels.size());
            buffers.resize (channels.size());
            for (size_t i = 0; i < channels.size(); ++i)
            {
                ports[i].channels = channels[i];
                ports[i].storage.assign ((size_t) channels[i], std::vector<float> ((size_t) maxFrames, 0.0f));
                for (auto& s : ports[i].storage)
                    ports[i].data.push_back (s.data());
                buffers[i].data32 = ports[i].data.data();
                buffers[i].data64 = nullptr;
                buffers[i].channel_count = (uint32_t) channels[i];
                buffers[i].latency = 0;
                buffers[i].constant_mask = 0;
            }
        };
        build (inPorts, inBuffers, inPortChannels);
        build (outPorts, outBuffers, outPortChannels);
    }

    void push (const Event& e)
    {
        if (events.size() < events.capacity())   // (no allocation on the audio thread: events past the reserve are dropped)
            events.push_back (e);
    }

    // Parameter changes from other threads (the user interface, automation): a small queue the audio thread empties
    void pushParamChange (const ParamChange& c)
    {
        const juce::SpinLock::ScopedLockType lock (paramLock);
        int s1, n1, s2, n2;
        paramFifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 > 0)      paramQueue[(size_t) s1] = c;
        else if (n2 > 0) paramQueue[(size_t) s2] = c;
        paramFifo.finishedWrite (n1 + n2);
    }

    bool popParamChange (ParamChange& c)
    {
        int s1, n1, s2, n2;
        paramFifo.prepareToRead (1, s1, n1, s2, n2);
        if (n1 > 0)      c = paramQueue[(size_t) s1];
        else if (n2 > 0) c = paramQueue[(size_t) s2];
        else             return false;
        paramFifo.finishedRead (n1 + n2);
        return true;
    }

    void outputEvent (const clap_event_header_t& h)
    {
        if (h.space_id == CLAP_CORE_EVENT_SPACE_ID && h.type == CLAP_EVENT_PARAM_VALUE)
        {
            const auto& e = reinterpret_cast<const clap_event_param_value_t&> (h);
            if (auto it = byId.find (e.param_id); it != byId.end())
                it->second->current = e.value;   // (the plugin's own window moved a knob)
        }
    }

    void handleAsyncUpdate() override
    {
        if (latencyChanged.exchange (false) && latencyExt != nullptr && plugin != nullptr)
            setLatencySamples ((int) latencyExt->get (plugin));
        if (callbackRequested.exchange (false) && plugin != nullptr && plugin->on_main_thread != nullptr)
            plugin->on_main_thread (plugin);
    }

    friend class ClapEditor;

    std::shared_ptr<ClapLibrary> library;
    uint32_t descriptorIndex;
    juce::PluginDescription description;
    clap_host_t host {};
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_audio_ports_t* audioPorts = nullptr;
    const clap_plugin_note_ports_t* notePorts = nullptr;
    const clap_plugin_params_t* paramsExt = nullptr;
    const clap_plugin_state_t* stateExt = nullptr;
    const clap_plugin_latency_t* latencyExt = nullptr;
    const clap_plugin_tail_t* tailExt = nullptr;
    const clap_plugin_gui_t* guiExt = nullptr;
    juce::String name;
    std::unordered_map<clap_id, Param*> byId;

    std::vector<int> inPortChannels, outPortChannels;
    std::vector<PortBuffers> inPorts, outPorts;
    std::vector<clap_audio_buffer_t> inBuffers, outBuffers;
    std::vector<Event> events;
    double sampleRate = 44100.0;
    int maxFrames = 512;
    int64_t steady = 0;
    bool active = false, processing = false, failed = false;
    bool takesNotes = false, midiNotes = false, midiDialect = true;
    EditorMode editorMode = EditorMode::none;
    juce::AudioProcessorEditor* editor = nullptr;
    std::atomic<bool> latencyChanged { false }, callbackRequested { false };

    juce::SpinLock paramLock;
    juce::AbstractFifo paramFifo { 1024 };
    std::array<ParamChange, 1024> paramQueue;
};

//==============================================================================
// The window of a plugin: embedded in our editor window (the plugin makes a child window of it), or its own floating window
class ClapEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    ClapEditor (ClapPluginInstance& p, bool embeddedWindow)
        : juce::AudioProcessorEditor (p), owner (p), embedded (embeddedWindow)
    {
        const auto* plugin = owner.getPlugin();
        const auto* gui = owner.getGui();
        created = gui->create (plugin, CLAP_WINDOW_API_WIN32, ! embedded);
        if (! created)
        {
            setSize (320, 60);
            return;
        }
        if (embedded)
        {
            uint32_t w = 600, h = 400;
            gui->get_size (plugin, &w, &h);
            setSize ((int) juce::jmax (50u, w), (int) juce::jmax (50u, h));
            startTimer (50);   // (the window handle exists once this component is on screen)
        }
        else
        {
            gui->suggest_title (plugin, owner.getName().toRawUTF8());
            gui->show (plugin);
            button.setButtonText ("Show the plugin's window");
            button.onClick = [this] { owner.getGui()->show (owner.getPlugin()); };
            addAndMakeVisible (button);
            setSize (320, 60);
        }
    }

    ~ClapEditor() override
    {
        stopTimer();
        if (created)
        {
            owner.getGui()->hide (owner.getPlugin());
            owner.getGui()->destroy (owner.getPlugin());
        }
        owner.editorClosed (this);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        if (! created)
        {
            g.setColour (juce::Colours::white);
            g.drawText ("The plugin's window could not be opened.", getLocalBounds(), juce::Justification::centred);
        }
    }

    void resized() override { button.setBounds (getLocalBounds().reduced (30, 14)); }

private:
    void timerCallback() override
    {
        if (parented)
        {
            stopTimer();
            return;
        }
        if (auto* peer = getPeer())
        {
            clap_window_t window {};
            window.api = CLAP_WINDOW_API_WIN32;
            window.win32 = peer->getNativeHandle();
            const auto* plugin = owner.getPlugin();
            const auto* gui = owner.getGui();
            parented = true;
            if (gui->set_parent (plugin, &window))
                gui->show (plugin);
        }
    }

    ClapPluginInstance& owner;
    bool embedded = true, created = false, parented = false;
    juce::TextButton button;
};

juce::AudioProcessorEditor* ClapPluginInstance::createEditor()
{
    if (editorMode == EditorMode::none || plugin == nullptr || guiExt == nullptr)
        return nullptr;
    editor = new ClapEditor (*this, editorMode == EditorMode::embedded);
    return editor;
}

bool ClapPluginInstance::hostRequestResize (uint32_t w, uint32_t h)
{
    if (editor == nullptr)
        return false;
    editor->setSize ((int) w, (int) h);
    return true;
}

//==============================================================================
// the host the plugins see
struct HostCalls
{
    static ClapPluginInstance* instance (const clap_host_t* h) { return static_cast<ClapPluginInstance*> (h->host_data); }

    static bool isMain (const clap_host_t*) { return juce::MessageManager::getInstance()->isThisTheMessageThread(); }
    static bool isAudio (const clap_host_t*) { return ! juce::MessageManager::getInstance()->isThisTheMessageThread(); }
    static constexpr clap_host_thread_check_t threadCheck { isMain, isAudio };

    static void paramsRescan (const clap_host_t*, clap_param_rescan_flags) {}
    static void paramsClear (const clap_host_t*, clap_id, clap_param_clear_flags) {}
    static void paramsFlush (const clap_host_t*) {}
    static constexpr clap_host_params_t params { paramsRescan, paramsClear, paramsFlush };

    static void stateDirty (const clap_host_t*) {}
    static constexpr clap_host_state_t state { stateDirty };

    static void latencyChanged (const clap_host_t* h) { if (auto* i = instance (h)) i->hostLatencyChanged(); }
    static constexpr clap_host_latency_t latency { latencyChanged };

    static void tailChanged (const clap_host_t*) {}
    static constexpr clap_host_tail_t tail { tailChanged };

    static void resizeHints (const clap_host_t*) {}
    static bool requestResize (const clap_host_t* h, uint32_t w, uint32_t hh) { auto* i = instance (h); return i != nullptr && i->hostRequestResize (w, hh); }
    static bool requestShow (const clap_host_t*) { return false; }
    static bool requestHide (const clap_host_t*) { return false; }
    static void guiClosed (const clap_host_t*, bool) {}
    static constexpr clap_host_gui_t gui { resizeHints, requestResize, requestShow, requestHide, guiClosed };

    static const void* extension (const clap_host_t*, const char* id)
    {
        if (id == nullptr) return nullptr;
        if (! std::strcmp (id, CLAP_EXT_THREAD_CHECK)) return &threadCheck;
        if (! std::strcmp (id, CLAP_EXT_PARAMS))       return &params;
        if (! std::strcmp (id, CLAP_EXT_STATE))        return &state;
        if (! std::strcmp (id, CLAP_EXT_LATENCY))      return &latency;
        if (! std::strcmp (id, CLAP_EXT_TAIL))         return &tail;
        if (! std::strcmp (id, CLAP_EXT_GUI))          return &gui;
        return nullptr;
    }
    static void requestRestart (const clap_host_t*) {}
    static void requestProcess (const clap_host_t*) {}
    static void requestCallback (const clap_host_t* h) { if (auto* i = instance (h)) i->hostCallbackRequested(); }
};

const clap_host_t* makeHost (void* data)
{
    static thread_local clap_host_t h;
    h = {};
    h.clap_version = CLAP_VERSION;
    h.host_data = data;
    h.name = "UFODAW";
    h.vendor = "UFODAW";
    h.url = "https://github.com/thulehunter/ufodaw";
    h.version = "1.0";
    h.get_extension = HostCalls::extension;
    h.request_restart = HostCalls::requestRestart;
    h.request_process = HostCalls::requestProcess;
    h.request_callback = HostCalls::requestCallback;
    return &h;
}

// How many audio channels a plugin has (made and thrown away again: for the scan)
void probePorts (const ClapLibrary& library, const char* id, bool isInstrument, int& inputs, int& outputs)
{
    inputs = isInstrument ? 0 : 2;
    outputs = 2;
    const clap_host_t* host = makeHost (nullptr);
    clap_host_t hostCopy = *host;
    const auto* plugin = library.factory->create_plugin (library.factory, &hostCopy, id);
    if (plugin == nullptr)
        return;
    if (plugin->init (plugin))
        if (auto* ports = static_cast<const clap_plugin_audio_ports_t*> (plugin->get_extension (plugin, CLAP_EXT_AUDIO_PORTS)))
        {
            auto channelsOf = [&] (bool input)
            {
                int total = 0;
                if (ports->count (plugin, input) > 0)
                {
                    clap_audio_port_info_t info {};
                    if (ports->get (plugin, 0, input, &info))
                        total = juce::jlimit (1, 2, (int) info.channel_count);
                }
                return total;
            };
            inputs = channelsOf (true);
            outputs = channelsOf (false);
        }
    plugin->destroy (plugin);
}
}   // namespace

//==============================================================================
void ClapPluginFormat::findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& fileOrIdentifier)
{
    const juce::File file (fileOrIdentifier);
    juce::String error;
    auto library = ClapLibrary::open (file, error);
    if (library == nullptr)
        return;

    const uint32_t count = library->factory->get_plugin_count (library->factory);
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto* d = library->factory->get_plugin_descriptor (library->factory, i);
        if (d == nullptr || d->id == nullptr || d->name == nullptr)
            continue;

        auto description = std::make_unique<juce::PluginDescription>();
        description->name = juce::String::fromUTF8 (d->name);
        description->descriptiveName = d->description != nullptr && *d->description != 0 ? juce::String::fromUTF8 (d->description) : description->name;
        description->pluginFormatName = formatName;
        description->manufacturerName = d->vendor != nullptr ? juce::String::fromUTF8 (d->vendor) : juce::String();
        description->version = d->version != nullptr ? juce::String::fromUTF8 (d->version) : juce::String();
        description->fileOrIdentifier = file.getFullPathName();
        description->lastFileModTime = file.getLastModificationTime();
        description->lastInfoUpdateTime = juce::Time::getCurrentTime();
        description->uniqueId = description->deprecatedUid = idHash (d->id);
        description->isInstrument = false;
        if (d->features != nullptr)
            for (const char* const* f = d->features; *f != nullptr; ++f)
            {
                if (! std::strcmp (*f, CLAP_PLUGIN_FEATURE_INSTRUMENT))
                    description->isInstrument = true;
                if (description->category.isEmpty())
                    description->category = juce::String::fromUTF8 (*f);
            }
        probePorts (*library, d->id, description->isInstrument, description->numInputChannels, description->numOutputChannels);
        results.add (description.release());
    }
}

bool ClapPluginFormat::fileMightContainThisPluginType (const juce::String& fileOrIdentifier)
{
    const juce::File f (fileOrIdentifier);
    return f.existsAsFile() && f.hasFileExtension (".clap");
}

juce::String ClapPluginFormat::getNameOfPluginFromIdentifier (const juce::String& fileOrIdentifier)
{
    return juce::File (fileOrIdentifier).getFileNameWithoutExtension();
}

bool ClapPluginFormat::pluginNeedsRescanning (const juce::PluginDescription& d)
{
    return juce::File (d.fileOrIdentifier).getLastModificationTime() != d.lastFileModTime;
}

bool ClapPluginFormat::doesPluginStillExist (const juce::PluginDescription& d)
{
    return juce::File (d.fileOrIdentifier).existsAsFile();
}

juce::StringArray ClapPluginFormat::searchPathsForPlugins (const juce::FileSearchPath& directories, bool recursive, bool)
{
    juce::StringArray result;
    for (int i = 0; i < directories.getNumPaths(); ++i)
        for (auto& f : directories[i].findChildFiles (juce::File::findFiles, recursive, "*.clap"))
            result.addIfNotAlreadyThere (f.getFullPathName());
    return result;
}

juce::FileSearchPath ClapPluginFormat::getDefaultLocationsToSearch()
{
    juce::FileSearchPath paths;
   #if JUCE_WINDOWS
    for (auto* variable : { "COMMONPROGRAMFILES", "COMMONPROGRAMFILES(X86)" })
        if (const auto dir = juce::SystemStats::getEnvironmentVariable (variable, {}); dir.isNotEmpty())
            paths.addIfNotAlreadyThere (juce::File (dir).getChildFile ("CLAP"));
    if (const auto local = juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA", {}); local.isNotEmpty())
        paths.addIfNotAlreadyThere (juce::File (local).getChildFile ("Programs/Common/CLAP"));
   #elif JUCE_MAC
    paths.add ("/Library/Audio/Plug-Ins/CLAP");
    paths.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Audio/Plug-Ins/CLAP"));
   #else
    paths.add (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".clap"));
    paths.add ("/usr/lib/clap");
   #endif
    const auto extra = juce::SystemStats::getEnvironmentVariable ("CLAP_PATH", {});
    for (auto& p : juce::StringArray::fromTokens (extra, JUCE_WINDOWS ? ";" : ":", ""))
        if (p.isNotEmpty())
            paths.addIfNotAlreadyThere (juce::File (p));
    return paths;
}

void ClapPluginFormat::createPluginInstance (const juce::PluginDescription& description, double, int, PluginCreationCallback callback)
{
    juce::String error;
    auto library = ClapLibrary::open (juce::File (description.fileOrIdentifier), error);
    if (library == nullptr)
    {
        callback (nullptr, error);
        return;
    }

    const uint32_t count = library->factory->get_plugin_count (library->factory);
    int found = -1;
    for (uint32_t i = 0; i < count && found < 0; ++i)
        if (const auto* d = library->factory->get_plugin_descriptor (library->factory, i))
            if (idHash (d->id) == description.uniqueId)
                found = (int) i;
    if (found < 0)
    {
        callback (nullptr, description.name + " is not in " + juce::File (description.fileOrIdentifier).getFileName() + " any more.");
        return;
    }

    auto instance = ClapPluginInstance::create (library, (uint32_t) found, description);
    error = instance->init();
    if (error.isNotEmpty())
    {
        callback (nullptr, error);
        return;
    }
    callback (std::move (instance), {});
}
