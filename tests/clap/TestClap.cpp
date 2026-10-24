// A tiny CLAP plugin file for the tests of UFODAW's CLAP hosting (EngineTest): two plugins in one file.
//   "UFODAW Test Synth"  - instrument: a sine for every held note, parameter "Level", state, latency of 64 samples
//   "UFODAW Test Gain"   - effect: stereo in / out, parameter "Gain"
// It does only what the tests need and is not meant to be used for music.
#include <clap/all.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace
{
const char* const synthFeatures[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, nullptr };
const char* const gainFeatures[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, CLAP_PLUGIN_FEATURE_STEREO, nullptr };

const clap_plugin_descriptor_t synthDescriptor = { CLAP_VERSION, "io.github.ufodaw.test-synth", "UFODAW Test Synth", "UFODAW", "", "", "", "1.0", "Test instrument", synthFeatures };
const clap_plugin_descriptor_t gainDescriptor = { CLAP_VERSION, "io.github.ufodaw.test-gain", "UFODAW Test Gain", "UFODAW", "", "", "", "1.0", "Test effect", gainFeatures };

struct Voice { bool on = false; int key = 0; double phase = 0.0, inc = 0.0, level = 0.0; bool released = false; };

struct Plugin
{
    clap_plugin_t clap {};
    const clap_host_t* host = nullptr;
    bool instrument = false;
    double rate = 44100.0;
    float value = 0.5f;   // "Level" (0..1) or "Gain" (0..2)
    Voice voices[16];
};

// ---- extensions
uint32_t audioCount (const clap_plugin_t* p, bool input)
{
    const auto* self = static_cast<Plugin*> (p->plugin_data);
    return (input && self->instrument) ? 0u : 1u;
}
bool audioGet (const clap_plugin_t*, uint32_t index, bool input, clap_audio_port_info_t* info)
{
    if (index != 0) return false;
    info->id = input ? 1 : 2;
    std::snprintf (info->name, sizeof (info->name), input ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t audioPorts = { audioCount, audioGet };

uint32_t noteCount (const clap_plugin_t* p, bool input) { return (input && static_cast<Plugin*> (p->plugin_data)->instrument) ? 1u : 0u; }
bool noteGet (const clap_plugin_t*, uint32_t index, bool, clap_note_port_info_t* info)
{
    if (index != 0) return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf (info->name, sizeof (info->name), "Notes");
    return true;
}
const clap_plugin_note_ports_t notePorts = { noteCount, noteGet };

uint32_t paramCount (const clap_plugin_t*) { return 1; }
bool paramInfo (const clap_plugin_t* p, uint32_t index, clap_param_info_t* info)
{
    if (index != 0) return false;
    const bool synth = static_cast<Plugin*> (p->plugin_data)->instrument;
    info->id = 1;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    info->cookie = nullptr;
    std::snprintf (info->name, sizeof (info->name), synth ? "Level" : "Gain");
    info->module[0] = 0;
    info->min_value = 0.0;
    info->max_value = synth ? 1.0 : 2.0;
    info->default_value = synth ? 0.5 : 1.0;
    return true;
}
bool paramValue (const clap_plugin_t* p, clap_id id, double* out) { if (id != 1) return false; *out = static_cast<Plugin*> (p->plugin_data)->value; return true; }
bool paramText (const clap_plugin_t*, clap_id id, double v, char* out, uint32_t size) { if (id != 1) return false; std::snprintf (out, size, "%.2f units", v); return true; }
bool paramFromText (const clap_plugin_t*, clap_id id, const char* text, double* out) { if (id != 1) return false; *out = std::atof (text); return true; }
void applyEvent (Plugin* self, const clap_event_header_t* h);
void paramFlush (const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*)
{
    auto* self = static_cast<Plugin*> (p->plugin_data);
    for (uint32_t i = 0; i < in->size (in); ++i) applyEvent (self, in->get (in, i));
}
const clap_plugin_params_t params = { paramCount, paramInfo, paramValue, paramText, paramFromText, paramFlush };

bool stateSave (const clap_plugin_t* p, const clap_ostream_t* s)
{
    const float v = static_cast<Plugin*> (p->plugin_data)->value;
    return s->write (s, &v, sizeof (v)) == (int64_t) sizeof (v);
}
bool stateLoad (const clap_plugin_t* p, const clap_istream_t* s)
{
    float v = 0;
    if (s->read (s, &v, sizeof (v)) != (int64_t) sizeof (v)) return false;
    static_cast<Plugin*> (p->plugin_data)->value = v;
    return true;
}
const clap_plugin_state_t state = { stateSave, stateLoad };

uint32_t latencyGet (const clap_plugin_t* p) { return static_cast<Plugin*> (p->plugin_data)->instrument ? 64u : 0u; }
const clap_plugin_latency_t latency = { latencyGet };

const void* getExtension (const clap_plugin_t*, const char* id)
{
    if (! std::strcmp (id, CLAP_EXT_AUDIO_PORTS)) return &audioPorts;
    if (! std::strcmp (id, CLAP_EXT_NOTE_PORTS))  return &notePorts;
    if (! std::strcmp (id, CLAP_EXT_PARAMS))      return &params;
    if (! std::strcmp (id, CLAP_EXT_STATE))       return &state;
    if (! std::strcmp (id, CLAP_EXT_LATENCY))     return &latency;
    return nullptr;
}

// ---- processing
void noteOn (Plugin* self, int key, double velocity)
{
    for (auto& v : self->voices)
        if (! v.on)
        {
            v.on = true; v.released = false; v.key = key; v.phase = 0.0; v.level = velocity;
            v.inc = 440.0 * std::pow (2.0, (key - 69) / 12.0) / self->rate;
            return;
        }
}
void noteOff (Plugin* self, int key) { for (auto& v : self->voices) if (v.on && ! v.released && v.key == key) { v.released = true; return; } }

void applyEvent (Plugin* self, const clap_event_header_t* h)
{
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    switch (h->type)
    {
        case CLAP_EVENT_NOTE_ON:  { auto* e = (const clap_event_note_t*) h; noteOn (self, e->key, e->velocity); break; }
        case CLAP_EVENT_NOTE_OFF: { auto* e = (const clap_event_note_t*) h; noteOff (self, e->key); break; }
        case CLAP_EVENT_MIDI:
        {
            auto* e = (const clap_event_midi_t*) h;
            if ((e->data[0] & 0xf0) == 0x90 && e->data[2] > 0) noteOn (self, e->data[1], e->data[2] / 127.0);
            else if ((e->data[0] & 0xf0) == 0x80 || (e->data[0] & 0xf0) == 0x90) noteOff (self, e->data[1]);
            break;
        }
        case CLAP_EVENT_PARAM_VALUE: { auto* e = (const clap_event_param_value_t*) h; if (e->param_id == 1) self->value = (float) e->value; break; }
        default: break;
    }
}

clap_process_status process (const clap_plugin_t* p, const clap_process_t* pr)
{
    auto* self = static_cast<Plugin*> (p->plugin_data);
    const uint32_t n = pr->frames_count, count = pr->in_events->size (pr->in_events);
    float* outL = pr->audio_outputs[0].data32[0];
    float* outR = pr->audio_outputs[0].data32[1];
    uint32_t next = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        while (next < count && pr->in_events->get (pr->in_events, next)->time <= i)
            applyEvent (self, pr->in_events->get (pr->in_events, next++));
        if (self->instrument)
        {
            float sum = 0.0f;
            for (auto& v : self->voices)
                if (v.on)
                {
                    sum += (float) (std::sin (6.283185307179586 * v.phase) * v.level * 0.3);
                    v.phase += v.inc; if (v.phase >= 1.0) v.phase -= 1.0;
                    if (v.released) { v.level *= 0.999; if (v.level < 1.0e-4) v.on = false; }
                }
            outL[i] = outR[i] = sum * self->value * 2.0f;
        }
        else
        {
            outL[i] = pr->audio_inputs[0].data32[0][i] * self->value;
            outR[i] = pr->audio_inputs[0].data32[1][i] * self->value;
        }
    }
    while (next < count) applyEvent (self, pr->in_events->get (pr->in_events, next++));
    return CLAP_PROCESS_CONTINUE;
}

bool init (const clap_plugin_t*) { return true; }
void destroy (const clap_plugin_t* p) { delete static_cast<Plugin*> (p->plugin_data); }
bool activate (const clap_plugin_t* p, double rate, uint32_t, uint32_t) { static_cast<Plugin*> (p->plugin_data)->rate = rate; return true; }
void deactivate (const clap_plugin_t*) {}
bool startProcessing (const clap_plugin_t*) { return true; }
void stopProcessing (const clap_plugin_t*) {}
void reset (const clap_plugin_t* p) { for (auto& v : static_cast<Plugin*> (p->plugin_data)->voices) v.on = false; }
void onMainThread (const clap_plugin_t*) {}

// ---- factory
uint32_t factoryCount (const clap_plugin_factory_t*) { return 2; }
const clap_plugin_descriptor_t* factoryDescriptor (const clap_plugin_factory_t*, uint32_t i) { return i == 0 ? &synthDescriptor : i == 1 ? &gainDescriptor : nullptr; }
const clap_plugin_t* factoryCreate (const clap_plugin_factory_t*, const clap_host_t* host, const char* id)
{
    const bool synth = ! std::strcmp (id, synthDescriptor.id);
    if (! synth && std::strcmp (id, gainDescriptor.id)) return nullptr;
    auto* self = new Plugin();
    self->host = host;
    self->instrument = synth;
    self->value = synth ? 0.5f : 1.0f;
    self->clap = { synth ? &synthDescriptor : &gainDescriptor, self, init, destroy, activate, deactivate, startProcessing, stopProcessing, reset, process, getExtension, onMainThread };
    return &self->clap;
}
const clap_plugin_factory_t factory = { factoryCount, factoryDescriptor, factoryCreate };

bool entryInit (const char*) { return true; }
void entryDeinit() {}
const void* entryFactory (const char* id) { return ! std::strcmp (id, CLAP_PLUGIN_FACTORY_ID) ? &factory : nullptr; }
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION, entryInit, entryDeinit, entryFactory };
