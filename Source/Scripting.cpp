#include "Scripting.h"
#include "MainComponent.h"
#include "SynthPresets.h"
#include <algorithm>
#include <cmath>
#include <string>

extern "C"
{
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

namespace Scripting
{
namespace
{
    //==========================================================================
    // the allocator: a script cannot use more memory than this
    struct MemoryState { size_t used = 0, limit = 128u * 1024u * 1024u; };

    void* limitedAlloc (void* ud, void* ptr, size_t osize, size_t nsize)
    {
        auto* m = static_cast<MemoryState*> (ud);
        if (nsize == 0)
        {
            if (ptr != nullptr)
                m->used -= osize;
            std::free (ptr);
            return nullptr;
        }
        const size_t old = ptr != nullptr ? osize : 0;
        if (m->used - old + nsize > m->limit)
            return nullptr;
        void* p = std::realloc (ptr, nsize);
        if (p != nullptr)
            m->used += nsize - old;
        return p;
    }

    //==========================================================================
    struct Context
    {
        explicit Context (MainComponent& m) : main (m), engine (m.getEngine()) {}

        MainComponent& main;
        Engine& engine;
        bool edited = false;
        juce::StringArray output;
        juce::uint32 deadline = 0;
        double maxSeconds = 20.0;

        void edit()   // (the first change of the script starts its undo step)
        {
            if (! edited)
            {
                edited = true;
                main.scriptBeginEdit();
            }
        }
    };

    Context& ctx (lua_State* L) { return *static_cast<Context*> (lua_touserdata (L, lua_upvalueindex (1))); }

    Track* trackArg (lua_State* L, int index)
    {
        auto& c = ctx (L);
        const int i = (int) luaL_checkinteger (L, index);
        Track* t = c.engine.getTrack (i - 1);
        if (t == nullptr)
            luaL_error (L, "there is no track %d (there are %d)", i, c.engine.getNumTracks());
        return t;
    }

    juce::String kindOf (const Track& t)
    {
        if (t.bus) return "bus";
        if (t.kit != nullptr) return "drums";
        if (t.sampler != nullptr) return "sampler";
        return t.type == Track::Type::Midi ? "midi" : "audio";
    }

    void setField (lua_State* L, const char* key, const juce::String& v) { lua_pushstring (L, v.toRawUTF8()); lua_setfield (L, -2, key); }
    void setField (lua_State* L, const char* key, double v)              { lua_pushnumber (L, v); lua_setfield (L, -2, key); }
    void setField (lua_State* L, const char* key, bool v)                { lua_pushboolean (L, v); lua_setfield (L, -2, key); }
    void setIntField (lua_State* L, const char* key, long long v)        { lua_pushinteger (L, (lua_Integer) v); lua_setfield (L, -2, key); }

    double fieldNumber (lua_State* L, int table, const char* key, double fallback, bool required = false)
    {
        lua_getfield (L, table, key);
        double v = fallback;
        if (lua_isnumber (L, -1))
            v = lua_tonumber (L, -1);
        else if (required)
            luaL_error (L, "a note needs a number called '%s'", key);
        lua_pop (L, 1);
        return v;
    }

    //==========================================================================
    // the ufodaw table
    int l_log (lua_State* L)
    {
        auto& c = ctx (L);
        juce::String line;
        const int n = lua_gettop (L);
        for (int i = 1; i <= n; ++i)
        {
            size_t len = 0;
            const char* s = luaL_tolstring (L, i, &len);
            line << (i > 1 ? "\t" : "") << juce::String::fromUTF8 (s, (int) len);
            lua_pop (L, 1);
        }
        c.output.add (line);
        return 0;
    }

    int l_bpm (lua_State* L)           { lua_pushnumber (L, ctx (L).engine.bpm.load()); return 1; }
    int l_set_bpm (lua_State* L)       { auto& c = ctx (L); c.edit(); c.main.scriptSetBpm (luaL_checknumber (L, 1)); return 0; }
    int l_position (lua_State* L)      { lua_pushnumber (L, ctx (L).engine.getPositionSeconds()); return 1; }
    int l_set_position (lua_State* L)  { ctx (L).engine.setPositionSeconds (luaL_checknumber (L, 1)); return 0; }
    int l_play (lua_State* L)          { ctx (L).engine.play(); return 0; }
    int l_stop (lua_State* L)          { ctx (L).engine.stop(); return 0; }
    int l_playing (lua_State* L)       { lua_pushboolean (L, ctx (L).engine.isPlaying()); return 1; }
    int l_length (lua_State* L)        { lua_pushnumber (L, ctx (L).engine.getTotalLengthSeconds()); return 1; }

    int l_bar_to_seconds (lua_State* L)
    {
        const auto tempo = ctx (L).engine.getTempoMap();
        lua_pushnumber (L, tempo.barStartSeconds ((int) std::floor (luaL_checknumber (L, 1)) - 1)
                               + (luaL_checknumber (L, 1) - std::floor (luaL_checknumber (L, 1))) * tempo.barSecondsAt (0.0));
        return 1;
    }

    int l_seconds_to_bar (lua_State* L)
    {
        const auto tempo = ctx (L).engine.getTempoMap();
        double into = 0.0;
        const double seconds = luaL_checknumber (L, 1);
        const int bar = tempo.barAtSeconds (seconds, &into);
        lua_pushnumber (L, bar + 1 + into / juce::jmax (1.0e-6, tempo.sectionAt (seconds).beatsPerBar()));
        return 1;
    }

    int l_tracks (lua_State* L)
    {
        auto& c = ctx (L);
        lua_newtable (L);
        for (int i = 0; i < c.engine.getNumTracks(); ++i)
        {
            auto* t = c.engine.getTrack (i);
            lua_newtable (L);
            setIntField (L, "index", i + 1);
            setField (L, "name", t->name);
            setField (L, "kind", kindOf (*t));
            setField (L, "gain", (double) t->gain.load());
            setField (L, "pan", (double) t->pan.load());
            setField (L, "mute", t->mute.load());
            setField (L, "solo", t->solo.load());
            lua_rawseti (L, -2, i + 1);
        }
        return 1;
    }

    int l_track_count (lua_State* L) { lua_pushinteger (L, ctx (L).engine.getNumTracks()); return 1; }

    int l_add_track (lua_State* L)
    {
        auto& c = ctx (L);
        const juce::String kind = luaL_checkstring (L, 1);
        const juce::String name = luaL_optstring (L, 2, "");
        if (kind != "audio" && kind != "midi" && kind != "drums")
            luaL_error (L, "the kind of a track is \"audio\", \"midi\" or \"drums\", not \"%s\"", kind.toRawUTF8());
        if (c.engine.getNumTracks() >= 64)
            luaL_error (L, "there are too many tracks");
        c.edit();
        auto* t = c.main.scriptAddTrack (kind, name);
        lua_pushinteger (L, c.engine.indexOfTrack (t) + 1);
        return 1;
    }

    int l_set_track (lua_State* L)
    {
        auto& c = ctx (L);
        auto* t = trackArg (L, 1);
        luaL_checktype (L, 2, LUA_TTABLE);
        c.edit();
        lua_getfield (L, 2, "name");   if (lua_isstring (L, -1)) t->name = juce::String::fromUTF8 (lua_tostring (L, -1)); lua_pop (L, 1);
        lua_getfield (L, 2, "gain");   if (lua_isnumber (L, -1)) t->gain = (float) juce::jlimit (0.0, 2.0, lua_tonumber (L, -1)); lua_pop (L, 1);
        lua_getfield (L, 2, "pan");    if (lua_isnumber (L, -1)) t->pan = (float) juce::jlimit (-1.0, 1.0, lua_tonumber (L, -1)); lua_pop (L, 1);
        lua_getfield (L, 2, "mute");   if (lua_isboolean (L, -1)) t->mute = lua_toboolean (L, -1) != 0; lua_pop (L, 1);
        lua_getfield (L, 2, "solo");   if (lua_isboolean (L, -1)) t->solo = lua_toboolean (L, -1) != 0; lua_pop (L, 1);
        return 0;
    }

    // ---- clips and notes (bars count from 1; the time of a note is in beats from the start of its clip)
    int l_add_clip (lua_State* L)
    {
        auto& c = ctx (L);
        auto* t = trackArg (L, 1);
        if (t->type != Track::Type::Midi)
            luaL_error (L, "add_clip makes MIDI clips: use a track of kind \"midi\" or \"drums\"");
        const double bar = luaL_checknumber (L, 2), bars = luaL_checknumber (L, 3);
        if (bar < 1.0 || bars <= 0.0 || bars > 999.0)
            luaL_error (L, "the first bar is 1 or more and the clip is at least a bit long");
        const auto tempo = c.engine.getTempoMap();
        const double start = tempo.barStartSeconds ((int) std::floor (bar) - 1);
        const double end = tempo.barStartSeconds ((int) std::floor (bar) - 1 + (int) std::ceil (bars));
        c.edit();
        lua_pushinteger (L, c.engine.addMidiClip (*t, start, end - start));
        return 1;
    }

    int l_clips (lua_State* L)
    {
        auto& c = ctx (L);
        auto* t = trackArg (L, 1);
        const auto tempo = c.engine.getTempoMap();
        lua_newtable (L);
        int n = 0;
        auto addClip = [&] (int id, double start, double length, int count, const juce::String& file)
        {
            lua_newtable (L);
            setIntField (L, "id", id);
            setField (L, "bar", tempo.barAtSeconds (start) + 1.0);
            setField (L, "start", start);
            setField (L, "length", length);
            if (count >= 0) setIntField (L, "notes", count);
            if (file.isNotEmpty()) setField (L, "file", file);
            lua_rawseti (L, -2, ++n);
        };
        if (t->type == Track::Type::Midi)
            for (auto& m : c.engine.getMidiClips (*t))
                addClip (m.id, m.start, m.length, (int) m.notes.size(), {});
        else
            for (auto& m : c.engine.getClips (*t))
                addClip (m.id, m.start, m.length, -1, m.sourceFile.getFileName());
        return 1;
    }

    const MidiClip* findMidiClip (Context& c, Track& t, int id, std::vector<MidiClip>& keep)
    {
        keep = c.engine.getMidiClips (t);
        for (auto& m : keep)
            if (m.id == id)
                return &m;
        return nullptr;
    }

    int l_get_notes (lua_State* L)
    {
        auto& c = ctx (L);
        auto* t = trackArg (L, 1);
        const int id = (int) luaL_checkinteger (L, 2);
        std::vector<MidiClip> keep;
        const auto* clip = findMidiClip (c, *t, id, keep);
        if (clip == nullptr)
            luaL_error (L, "track %d has no MIDI clip %d", c.engine.indexOfTrack (t) + 1, id);
        const double beat = c.engine.getTempoMap().beatSecondsAt (clip->start);
        lua_newtable (L);
        int n = 0;
        for (auto& note : clip->notes)
        {
            lua_newtable (L);
            setIntField (L, "pitch", note.pitch);
            setField (L, "beat", note.start / beat);
            setField (L, "length", note.length / beat);
            setField (L, "velocity", (double) note.velocity);
            lua_rawseti (L, -2, ++n);
        }
        return 1;
    }

    int l_set_notes (lua_State* L)
    {
        auto& c = ctx (L);
        auto* t = trackArg (L, 1);
        const int id = (int) luaL_checkinteger (L, 2);
        luaL_checktype (L, 3, LUA_TTABLE);
        std::vector<MidiClip> keep;
        const auto* clip = findMidiClip (c, *t, id, keep);
        if (clip == nullptr)
            luaL_error (L, "track %d has no MIDI clip %d", c.engine.indexOfTrack (t) + 1, id);
        const double beat = c.engine.getTempoMap().beatSecondsAt (clip->start);

        const lua_Unsigned count = lua_rawlen (L, 3);
        if (count > 100000)
            luaL_error (L, "a clip can hold at most 100000 notes");
        std::vector<Note> notes;
        notes.reserve ((size_t) count);
        for (lua_Unsigned i = 1; i <= count; ++i)
        {
            lua_rawgeti (L, 3, (lua_Integer) i);
            if (! lua_istable (L, -1))
                luaL_error (L, "note %d is not a table like {pitch = 60, beat = 0, length = 1, velocity = 0.8}", (int) i);
            Note note;
            note.pitch = (int) fieldNumber (L, lua_gettop (L), "pitch", 60.0, true);
            note.start = juce::jmax (0.0, fieldNumber (L, lua_gettop (L), "beat", 0.0, true)) * beat;
            note.length = juce::jmax (0.01, fieldNumber (L, lua_gettop (L), "length", 1.0)) * beat;
            note.velocity = (float) juce::jlimit (0.0, 1.0, fieldNumber (L, lua_gettop (L), "velocity", 0.8));
            if (note.pitch < 0 || note.pitch > 127)
                luaL_error (L, "note %d: a pitch is 0 to 127 (middle C is 60)", (int) i);
            notes.push_back (note);
            lua_pop (L, 1);
        }
        std::stable_sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
        c.edit();
        lua_pushboolean (L, c.engine.setMidiNotes (*t, id, std::move (notes)));
        return 1;
    }

    // ---- the synth
    Track* synthTrack (lua_State* L, int index)
    {
        auto* t = trackArg (L, index);
        if (t->type != Track::Type::Midi || t->voices == nullptr || t->kit != nullptr || t->sampler != nullptr)
            luaL_error (L, "track %d has no built-in synth (only tracks of kind \"midi\" have one)", ctx (L).engine.indexOfTrack (t) + 1);
        return t;
    }

    int l_synth_preset (lua_State* L)
    {
        auto& c = ctx (L);
        auto* t = synthTrack (L, 1);
        const juce::String name = luaL_checkstring (L, 2);
        for (auto& p : SynthPresets::all())
            if (name.equalsIgnoreCase (p.name))
            {
                c.edit();
                t->synth.apply (p.values);
                return 0;
            }
        luaL_error (L, "there is no sound called \"%s\" (see ufodaw.synth_presets())", name.toRawUTF8());
        return 0;
    }

    int l_synth_presets (lua_State* L)
    {
        lua_newtable (L);
        int n = 0;
        for (auto& p : SynthPresets::all())
        {
            lua_pushstring (L, p.name);
            lua_rawseti (L, -2, ++n);
        }
        return 1;
    }

    int l_set_synth (lua_State* L)
    {
        auto& c = ctx (L);
        auto* t = synthTrack (L, 1);
        luaL_checktype (L, 2, LUA_TTABLE);
        c.edit();
        lua_pushnil (L);
        while (lua_next (L, 2) != 0)
        {
            const juce::String key = lua_isstring (L, -2) ? lua_tostring (L, -2) : "";
            bool known = false;
            for (auto& info : SynthParams::floatInfo())
                if (key == info.name)
                {
                    if (! lua_isnumber (L, -1))
                        luaL_error (L, "synth setting '%s' is a number", key.toRawUTF8());
                    (t->synth.*(info.member)) = (float) juce::jlimit ((double) info.lo, (double) info.hi, lua_tonumber (L, -1));
                    known = true;
                }
            auto choice = [&] (std::atomic<int>& target, std::initializer_list<const char*> names, int maxValue)
            {
                if (lua_isnumber (L, -1))
                    target = juce::jlimit (0, maxValue, (int) lua_tointeger (L, -1));
                else if (lua_isstring (L, -1))
                {
                    int i = 0;
                    for (auto* n : names)
                    {
                        if (juce::String (lua_tostring (L, -1)).equalsIgnoreCase (n)) target = i;
                        ++i;
                    }
                }
                known = true;
            };
            if (key == "waveform")      choice (t->synth.waveform, { "sine", "saw", "square", "triangle" }, 3);
            else if (key == "osc2Wave") choice (t->synth.osc2Wave, { "sine", "saw", "square", "triangle" }, 3);
            else if (key == "lfoShape") choice (t->synth.lfoShape, { "sine", "triangle", "square", "random" }, 3);
            else if (key == "engine")   choice (t->synth.engine, { "synth", "piano" }, 1);
            else if (key == "mono")     { if (lua_isboolean (L, -1)) t->synth.mono = lua_toboolean (L, -1) ? 1 : 0; else choice (t->synth.mono, { "poly", "mono" }, 1); known = true; }
            else if (key == "unison")   { t->synth.unison = juce::jlimit (1, 5, (int) lua_tointeger (L, -1)); known = true; }
            if (! known)
                luaL_error (L, "there is no synth setting called '%s'", key.toRawUTF8());
            lua_pop (L, 1);
        }
        return 0;
    }

    // ---- selection, rendering, messages
    int l_selected (lua_State* L)
    {
        auto& c = ctx (L);
        const auto sel = c.main.scriptSelection();
        if (sel.first == nullptr || sel.second < 0)
        {
            lua_pushnil (L);
            return 1;
        }
        lua_newtable (L);
        setIntField (L, "track", c.engine.indexOfTrack (sel.first) + 1);
        setIntField (L, "clip", sel.second);
        return 1;
    }

    int l_render (lua_State* L)
    {
        auto& c = ctx (L);
        const juce::String path = luaL_checkstring (L, 1);
        const auto ext = juce::File (path).getFileExtension().toLowerCase();
        if (! juce::StringArray { ".wav", ".flac", ".aiff", ".aif", ".mp3", ".ogg" }.contains (ext))
            luaL_error (L, "render writes .wav, .flac, .aiff, .mp3 or .ogg files");
        if (c.engine.getTotalLengthSeconds() <= 0.0)
            luaL_error (L, "there is nothing to render");
        const auto file = juce::File::isAbsolutePath (path) ? juce::File (path)
                                                              : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (path);
        c.engine.stop();
        lua_pushboolean (L, c.main.renderForCommandLine (file));
        lua_pushstring (L, file.getFullPathName().toRawUTF8());
        return 2;
    }

    void hook (lua_State* L, lua_Debug*)
    {
        lua_getfield (L, LUA_REGISTRYINDEX, "ufodaw.deadline");
        const auto deadline = (juce::uint32) lua_tointeger (L, -1);
        lua_pop (L, 1);
        if (juce::Time::getMillisecondCounter() > deadline)
            luaL_error (L, "the script ran for too long and was stopped");
    }

    //==========================================================================
    // the music library, in Lua
    const char* preludeSource = R"LUA(
music = {}
local semitone = { C = 0, D = 2, E = 4, F = 5, G = 7, A = 9, B = 11 }

-- "C4" -> 60, "Bb3" -> 58, "F#2" -> 42; a number stays as it is
function music.note(name)
  if type(name) == "number" then return name end
  local letter, accidental, octave = tostring(name):match("^([A-Ga-g])([#b]?)(-?%d+)$")
  if not letter then error("not a note name: " .. tostring(name), 2) end
  local n = semitone[letter:upper()] + (tonumber(octave) + 1) * 12
  if accidental == "#" then n = n + 1 elseif accidental == "b" then n = n - 1 end
  return n
end

music.scales = {
  major = {0, 2, 4, 5, 7, 9, 11}, minor = {0, 2, 3, 5, 7, 8, 10}, dorian = {0, 2, 3, 5, 7, 9, 10}, phrygian = {0, 1, 3, 5, 7, 8, 10},
  lydian = {0, 2, 4, 6, 7, 9, 11}, mixolydian = {0, 2, 4, 5, 7, 9, 10}, harmonic_minor = {0, 2, 3, 5, 7, 8, 11},
  pentatonic_major = {0, 2, 4, 7, 9}, pentatonic_minor = {0, 3, 5, 7, 10}, blues = {0, 3, 5, 6, 7, 10}, chromatic = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
}

-- the notes of a scale from `root` (a note name or number), `octaves` octaves up, with the root once more on top
function music.scale(root, name, octaves)
  local steps = music.scales[name or "major"]
  if not steps then error("unknown scale: " .. tostring(name), 2) end
  local r, out = music.note(root), {}
  for o = 0, (octaves or 1) - 1 do
    for _, s in ipairs(steps) do out[#out + 1] = r + o * 12 + s end
  end
  out[#out + 1] = r + (octaves or 1) * 12
  return out
end

music.chords = {
  maj = {0, 4, 7}, min = {0, 3, 7}, dim = {0, 3, 6}, aug = {0, 4, 8}, sus2 = {0, 2, 7}, sus4 = {0, 5, 7},
  ["7"] = {0, 4, 7, 10}, maj7 = {0, 4, 7, 11}, min7 = {0, 3, 7, 10}, dim7 = {0, 3, 6, 9}, min7b5 = {0, 3, 6, 10},
  maj9 = {0, 4, 7, 11, 14}, min9 = {0, 3, 7, 10, 14}, ["9"] = {0, 4, 7, 10, 14}, ["13"] = {0, 4, 7, 10, 14, 21}, add9 = {0, 4, 7, 14},
}

-- the notes of a chord: music.chord("D3", "min9")
function music.chord(root, quality)
  local steps = music.chords[quality or "maj"]
  if not steps then error("unknown chord: " .. tostring(quality), 2) end
  local r, out = music.note(root), {}
  for i, s in ipairs(steps) do out[i] = r + s end
  return out
end

-- an euclidean rhythm: `pulses` hits spread evenly over `steps`, as a list of true / false (rotated by `rotation` steps)
function music.euclid(steps, pulses, rotation)
  local pattern, bucket = {}, 0
  for i = 1, steps do
    bucket = bucket + pulses
    if bucket >= steps then bucket = bucket - steps; pattern[i] = true else pattern[i] = false end
  end
  rotation = (rotation or 0) % steps
  local out = {}
  for i = 1, steps do out[i] = pattern[(i - 1 - rotation) % steps + 1] end
  return out
end

-- a number between 0 and 1 that does not change for the same seed and index (for repeatable "random" patterns)
function music.hash(seed, i)
  local x = math.sin(seed * 12.9898 + i * 78.233) * 43758.5453
  return x - math.floor(x)
end
)LUA";

    //==========================================================================
    void registerApi (lua_State* L, Context* c)
    {
        struct Fn { const char* name; lua_CFunction fn; };
        static const Fn fns[] = {
            { "log", l_log }, { "bpm", l_bpm }, { "set_bpm", l_set_bpm }, { "position", l_position }, { "set_position", l_set_position },
            { "play", l_play }, { "stop", l_stop }, { "playing", l_playing }, { "length", l_length },
            { "bar_to_seconds", l_bar_to_seconds }, { "seconds_to_bar", l_seconds_to_bar },
            { "tracks", l_tracks }, { "track_count", l_track_count }, { "add_track", l_add_track }, { "set_track", l_set_track },
            { "add_clip", l_add_clip }, { "clips", l_clips }, { "get_notes", l_get_notes }, { "set_notes", l_set_notes },
            { "synth_preset", l_synth_preset }, { "synth_presets", l_synth_presets }, { "set_synth", l_set_synth },
            { "selected", l_selected }, { "render", l_render },
        };
        lua_newtable (L);
        for (auto& f : fns)
        {
            lua_pushlightuserdata (L, c);
            lua_pushcclosure (L, f.fn, 1);
            lua_setfield (L, -2, f.name);
        }
    }

    // A fresh global table with only the safe parts of Lua in it
    void pushSandbox (lua_State* L, Context* c, const std::map<juce::String, juce::String>& params)
    {
        lua_newtable (L);
        const int env = lua_gettop (L);

        for (auto* name : { "assert", "error", "ipairs", "next", "pairs", "pcall", "select", "tonumber", "tostring", "type", "xpcall",
                            "rawequal", "rawget", "rawset", "rawlen", "setmetatable", "getmetatable" })
        {
            lua_getglobal (L, name);
            lua_setfield (L, env, name);
        }

        for (auto* lib : { "math", "string", "table", "utf8" })
        {
            lua_getglobal (L, lib);
            lua_newtable (L);
            lua_pushnil (L);
            while (lua_next (L, -3) != 0)
            {
                if (! (juce::String (lib) == "string" && juce::String (lua_tostring (L, -2)) == "dump"))
                {
                    lua_pushvalue (L, -2);
                    lua_pushvalue (L, -2);
                    lua_settable (L, -5);
                }
                lua_pop (L, 1);
            }
            lua_remove (L, -2);          // the library table; the copy stays
            lua_setfield (L, env, lib);
        }

        lua_newtable (L);   // os: only the clock
        lua_getglobal (L, "os");
        for (auto* name : { "time", "clock", "date" })
        {
            lua_getfield (L, -1, name);
            lua_setfield (L, -3, name);
        }
        lua_pop (L, 1);
        lua_setfield (L, env, "os");

        lua_pushvalue (L, env);
        lua_setfield (L, env, "_G");
        lua_pushstring (L, "Lua 5.4 (UFODAW)");
        lua_setfield (L, env, "_VERSION");

        lua_pushlightuserdata (L, c);
        lua_pushcclosure (L, l_log, 1);
        lua_setfield (L, env, "print");

        registerApi (L, c);
        lua_setfield (L, env, "ufodaw");

        lua_newtable (L);
        for (auto& p : params)
        {
            const auto text = p.second.trim();
            if (text.isNotEmpty() && (text.containsOnly ("0123456789.-+eE") && text.getDoubleValue() == text.getDoubleValue()
                                      && (text.containsAnyOf ("0123456789"))))
            {
                if (text.containsOnly ("0123456789-+") && text.length() < 16)
                    lua_pushinteger (L, (lua_Integer) text.getLargeIntValue());   // (8, not 8.0)
                else
                    lua_pushnumber (L, text.getDoubleValue());
            }
            else if (text.equalsIgnoreCase ("true") || text.equalsIgnoreCase ("false"))
                lua_pushboolean (L, text.equalsIgnoreCase ("true"));
            else
                lua_pushstring (L, p.second.toRawUTF8());
            lua_setfield (L, -2, p.first.toRawUTF8());
        }
        lua_setfield (L, env, "params");
    }

    int messageHandler (lua_State* L)
    {
        const char* msg = lua_tostring (L, 1);
        if (msg == nullptr)
            msg = "(an error that is not a text)";
        luaL_traceback (L, L, msg, 1);
        return 1;
    }

    // Loads text as a chunk whose globals are the sandbox (the table at `envIndex`). Leaves the function on the stack, or the error message.
    int loadSandboxed (lua_State* L, const std::string& text, const std::string& name, int envIndex)
    {
        const int status = luaL_loadbufferx (L, text.data(), text.size(), name.c_str(), "t");   // (text only: no precompiled code)
        if (status == LUA_OK)
        {
            lua_pushvalue (L, envIndex);
            lua_setupvalue (L, -2, 1);
        }
        return status;
    }
}

//==============================================================================
std::vector<Param> readParams (const juce::String& source)
{
    std::vector<Param> out;
    for (auto line : juce::StringArray::fromLines (source))
    {
        line = line.trim();
        if (! line.startsWith ("--@param"))
            continue;
        auto parts = juce::StringArray::fromTokens (line.fromFirstOccurrenceOf ("--@param", false, false).trim(), " \t", "");
        parts.removeEmptyStrings();
        if (parts.size() < 2)
            continue;
        Param p;
        p.key = parts[0];
        p.defaultValue = parts[1];
        p.label = parts.size() > 2 ? juce::StringArray (parts.begin() + 2, parts.size() - 2).joinIntoString (" ") : p.key;
        out.push_back (p);
    }
    return out;
}

Result run (MainComponent& main, const juce::String& source, const juce::String& chunkName, const std::map<juce::String, juce::String>& params, double maxSeconds)
{
    Result result;
    Context c (main);
    c.maxSeconds = maxSeconds;
    MemoryState memory;

    lua_State* L = lua_newstate (limitedAlloc, &memory);
    if (L == nullptr)
    {
        result.ok = false;
        result.error = "Lua could not start";
        return result;
    }

    // the standard libraries, so that the sandbox can copy the safe parts of them
    luaL_requiref (L, "_G", luaopen_base, 1);       lua_pop (L, 1);
    luaL_requiref (L, "math", luaopen_math, 1);     lua_pop (L, 1);
    luaL_requiref (L, "string", luaopen_string, 1); lua_pop (L, 1);
    luaL_requiref (L, "table", luaopen_table, 1);   lua_pop (L, 1);
    luaL_requiref (L, "utf8", luaopen_utf8, 1);     lua_pop (L, 1);
    luaL_requiref (L, "os", luaopen_os, 1);         lua_pop (L, 1);

    c.deadline = juce::Time::getMillisecondCounter() + (juce::uint32) (maxSeconds * 1000.0);
    lua_pushinteger (L, (lua_Integer) c.deadline);
    lua_setfield (L, LUA_REGISTRYINDEX, "ufodaw.deadline");

    pushSandbox (L, &c, params);
    const int env = lua_gettop (L);

    // the globals that the sandbox leaves out are removed from the real global table as well, so nothing can reach them by accident
    for (auto* name : { "dofile", "loadfile", "load", "require", "collectgarbage", "io", "os", "debug", "package" })
    {
        lua_pushnil (L);
        lua_setglobal (L, name);
    }

    lua_pushcfunction (L, messageHandler);
    const int handler = lua_gettop (L);
    lua_sethook (L, hook, LUA_MASKCOUNT, 10000);

    auto runChunk = [&] (const std::string& text, const std::string& name) -> bool
    {
        if (loadSandboxed (L, text, name, env) != LUA_OK)
        {
            result.ok = false;
            result.error = juce::String::fromUTF8 (lua_tostring (L, -1));
            lua_pop (L, 1);
            return false;
        }
        if (lua_pcall (L, 0, 0, handler) != LUA_OK)
        {
            result.ok = false;
            result.error = juce::String::fromUTF8 (lua_tostring (L, -1));
            lua_pop (L, 1);
            return false;
        }
        return true;
    };

    if (runChunk (preludeSource, "=music"))
        runChunk (source.toStdString(), ("=" + chunkName).toStdString());

    lua_close (L);
    result.output = c.output.joinIntoString ("\n");
    if (c.edited)
        main.scriptDone();
    return result;
}

//==============================================================================
juce::File scriptsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("UFODAW").getChildFile ("Scripts");
}

juce::Array<juce::File> listScripts()
{
    auto files = scriptsFolder().findChildFiles (juce::File::findFiles, false, "*.lua");
    files.sort();
    return files;
}

namespace
{
    struct Example { const char* name; const char* text; };

    const Example examples[] = {
        { "Hello (tour of the project).lua", R"LUA(-- Prints what is in the project. A good first script to read.
print("Tempo: " .. ufodaw.bpm() .. " BPM")
print("Length: " .. string.format("%.1f", ufodaw.length()) .. " seconds")
for _, t in ipairs(ufodaw.tracks()) do
  print(t.index .. ". " .. t.name .. " (" .. t.kind .. "), volume " .. string.format("%.2f", t.gain))
  for _, clip in ipairs(ufodaw.clips(t.index)) do
    print("     clip " .. clip.id .. " from bar " .. string.format("%.1f", clip.bar) .. (clip.notes and (", " .. clip.notes .. " notes") or ""))
  end
end
)LUA" },

        { "Chord progression.lua", R"LUA(--@param key C Key (a note name like C, F#, Bb)
--@param scale minor Scale (major, minor, dorian, ...)
--@param bars 8 Bars
--@param sound Warm_pad Synth sound (spaces as _)
--@param seed 1 Random seed (another number, another progression)
-- Writes a progression of seventh chords from the scale on a new synth track: one chord per bar.
math.randomseed(params.seed)
local scale = music.scale(params.key .. "3", params.scale, 3)
local track = ufodaw.add_track("midi", "Chords")
ufodaw.synth_preset(track, (params.sound:gsub("_", " ")))
local clip = ufodaw.add_clip(track, 1, params.bars)
local notes = {}
local degree = 1
for bar = 0, params.bars - 1 do
  if bar > 0 then degree = math.random(1, #music.scales[params.scale]) end
  for _, step in ipairs({0, 2, 4, 6}) do
    notes[#notes + 1] = { pitch = scale[degree + step], beat = bar * 4 + step * 0.03, length = 3.8, velocity = 0.55 }
  end
end
ufodaw.set_notes(track, clip, notes)
print("Wrote " .. #notes .. " notes: " .. params.bars .. " bars in " .. params.key .. " " .. params.scale)
)LUA" },

        { "Euclidean drums.lua", R"LUA(--@param steps 16 Steps per bar
--@param kick 4 Kick hits
--@param snare 3 Snare hits
--@param hat 9 Hat hits
--@param bars 4 Bars
-- Spreads hits evenly over the steps (euclidean rhythms) on a new drum track.
local track = ufodaw.add_track("drums", "Euclid drums")
local clip = ufodaw.add_clip(track, 1, params.bars)
local notes = {}
local function lane(pitch, pulses, rotation, velocity)
  local pattern = music.euclid(params.steps, pulses, rotation)
  for bar = 0, params.bars - 1 do
    for i, on in ipairs(pattern) do
      if on then notes[#notes + 1] = { pitch = pitch, beat = bar * 4 + (i - 1) * 4 / params.steps, length = 0.1, velocity = velocity } end
    end
  end
end
lane(36, params.kick, 0, 0.95)                          -- kick
lane(38, params.snare, params.steps // 4, 0.9)           -- snare
lane(42, params.hat, 0, 0.65)                           -- closed hat
ufodaw.set_notes(track, clip, notes)
print("Wrote " .. #notes .. " drum hits")
)LUA" },

        { "Humanize selected clip.lua", R"LUA(--@param timing 0.02 Timing (beats, 0.02 is subtle)
--@param velocity 0.1 Velocity (0 to 1)
-- Moves every note of the selected MIDI clip a little and changes its velocity, so it sounds played.
local sel = ufodaw.selected()
if not sel then error("Select a MIDI clip first (click it in the arrangement).") end
local notes = ufodaw.get_notes(sel.track, sel.clip)
for _, n in ipairs(notes) do
  n.beat = math.max(0, n.beat + (math.random() * 2 - 1) * params.timing)
  n.velocity = math.min(1, math.max(0.05, n.velocity + (math.random() * 2 - 1) * params.velocity))
end
ufodaw.set_notes(sel.track, sel.clip, notes)
print(#notes .. " notes humanized")
)LUA" },

        { "Arpeggiate selected clip.lua", R"LUA(--@param rate 0.5 Note length in beats (0.25 = sixteenths)
-- Turns every chord of the selected MIDI clip (notes that start together) into an up-and-down arpeggio.
local sel = ufodaw.selected()
if not sel then error("Select a MIDI clip with chords first.") end
local notes = ufodaw.get_notes(sel.track, sel.clip)
local groups, order = {}, {}
for _, n in ipairs(notes) do
  local key = string.format("%.3f", n.beat)
  if not groups[key] then groups[key] = {}; order[#order + 1] = key end
  table.insert(groups[key], n)
end
local out = {}
for _, key in ipairs(order) do
  local g = groups[key]
  table.sort(g, function(a, b) return a.pitch < b.pitch end)
  local sequence = {}
  for i = 1, #g do sequence[#sequence + 1] = g[i] end
  for i = #g - 1, 2, -1 do sequence[#sequence + 1] = g[i] end   -- and back down
  local start, length = g[1].beat, g[1].length
  local t, i = 0, 1
  while t + params.rate <= length + 1e-6 do
    local n = sequence[(i - 1) % #sequence + 1]
    out[#out + 1] = { pitch = n.pitch, beat = start + t, length = params.rate * 0.9, velocity = n.velocity }
    t = t + params.rate
    i = i + 1
  end
end
ufodaw.set_notes(sel.track, sel.clip, out)
print(#out .. " notes from " .. #notes)
)LUA" },

        { "Random melody.lua", R"LUA(--@param key A Key
--@param scale pentatonic_minor Scale
--@param bars 4 Bars
--@param density 0.6 How busy (0 to 1)
--@param seed 7 Random seed
-- A melody that wanders up and down the scale, on a new synth track.
math.randomseed(params.seed)
local scale = music.scale(params.key .. "4", params.scale, 2)
local track = ufodaw.add_track("midi", "Melody")
ufodaw.synth_preset(track, "Soft lead")
local clip = ufodaw.add_clip(track, 1, params.bars)
local notes, position = {}, 4
for beat = 0, params.bars * 4 - 0.5, 0.5 do
  if math.random() < params.density then
    position = math.max(1, math.min(#scale, position + math.random(-2, 2)))
    notes[#notes + 1] = { pitch = scale[position], beat = beat, length = 0.45 * math.random(1, 3), velocity = 0.5 + math.random() * 0.3 }
  end
end
ufodaw.set_notes(track, clip, notes)
print(#notes .. " notes")
)LUA" },
    };
}

void ensureExamples()
{
    const auto folder = scriptsFolder();
    const auto marker = folder.getChildFile (".examples");
    if (marker.existsAsFile())
        return;
    folder.createDirectory();
    for (auto& e : examples)
    {
        const auto file = folder.getChildFile (e.name);
        if (! file.existsAsFile())
            file.replaceWithText (juce::String::fromUTF8 (e.text));
    }
    marker.replaceWithText ("1");
}
}
