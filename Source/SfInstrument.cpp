#include "SfInstrument.h"
#include <algorithm>
#include <cmath>
#include <map>

//==============================================================================
// SfPool: decodes the samples of an instrument on a helper thread

SfPool::SfPool() : juce::Thread ("Instrument sample loader")
{
    formats.registerBasicFormats();
}

SfPool::~SfPool()
{
    stopThread (10000);
}

SfSample* SfPool::add (SfSample* s)
{
    samples.emplace_back (s);
    return s;
}

void SfPool::start()
{
    if (samples.empty())
        preloadDone = true;
    else
        startThread (juce::Thread::Priority::background);
}

bool SfPool::waitForPreload (int timeoutMs) const
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (! preloadDone.load() && juce::Time::getMillisecondCounter() < end)
        juce::Thread::sleep (2);
    return preloadDone.load();
}

void SfPool::request (SfSample& s)
{
    int expected = 0;
    if (s.state.compare_exchange_strong (expected, 1))
        requested.store (true);
}

bool SfPool::load (SfSample& s)
{
    const int state = s.state.load();
    if (state == 2 || state == 3)
        return state == 2;

    std::unique_ptr<juce::AudioBuffer<float>> buffer;
    double rate = s.rate;

    if (s.byteOffset >= 0)   // 16 bit mono data inside a SoundFont
    {
        juce::FileInputStream in (s.file);
        if (in.openedOk() && s.frames > 0 && in.setPosition (s.byteOffset))
        {
            juce::HeapBlock<juce::int16> raw ((size_t) s.frames);
            if (in.read (raw, (int) (s.frames * 2)) == (int) (s.frames * 2))
            {
                buffer = std::make_unique<juce::AudioBuffer<float>> (1, (int) s.frames);
                auto* out = buffer->getWritePointer (0);
                for (juce::int64 i = 0; i < s.frames; ++i)
                    out[i] = (float) raw[(size_t) i] * (1.0f / 32768.0f);
            }
        }
    }
    else
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (s.file));
        if (reader != nullptr && reader->lengthInSamples > 0 && reader->sampleRate > 0.0)
        {
            const int channels = juce::jlimit (1, 2, (int) reader->numChannels);
            const int length = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 600.0));   // ten minutes at most
            if (bytesLoaded.load() + (juce::int64) length * channels * 4 <= totalBudget)
            {
                buffer = std::make_unique<juce::AudioBuffer<float>> (channels, length);
                if (! reader->read (buffer.get(), 0, length, 0, true, channels > 1))
                    buffer.reset();
                rate = reader->sampleRate;
            }
        }
    }

    if (buffer == nullptr || buffer->getNumSamples() < 2)
    {
        s.state = 3;
        return false;
    }

    bytesLoaded += (juce::int64) buffer->getNumSamples() * buffer->getNumChannels() * 4;
    s.rate = rate;
    s.frames = buffer->getNumSamples();
    s.data = std::move (buffer);
    s.ready.store (s.data.get(), std::memory_order_release);
    s.state = 2;
    return true;
}

void SfPool::run()
{
    for (auto& s : samples)   // what fits the budget, in the order of the regions
    {
        if (threadShouldExit())
            return;
        if (bytesLoaded.load() < preloadBudget)
            load (*s);
    }
    preloadDone = true;

    while (! threadShouldExit())   // after that, the samples a note asks for
    {
        if (requested.exchange (false))
            for (auto& s : samples)
                if (s->state.load() == 1 && ! threadShouldExit())
                    load (*s);
        wait (5);
    }
}

//==============================================================================
// SoundFont 2 files

namespace
{
struct Sf2Info
{
    struct Gen { juce::uint16 oper = 0; juce::int16 amount = 0; };
    struct Bag { int gen = 0, mod = 0; };
    struct PresetHeader { juce::String name; int number = 0, bank = 0, bag = 0; };
    struct InstHeader { juce::String name; int bag = 0; };
    struct SampleHeader
    {
        juce::String name;
        juce::uint32 start = 0, end = 0, startLoop = 0, endLoop = 0, rate = 0;
        int pitch = 60, correction = 0, type = 0;
    };

    juce::File file;
    juce::String name;
    juce::int64 smplOffset = -1, smplSize = 0;
    std::vector<PresetHeader> presets;   // (with the terminal record)
    std::vector<Bag> pbag, ibag;
    std::vector<Gen> pgen, igen;
    std::vector<InstHeader> insts;
    std::vector<SampleHeader> shdr;
};

juce::String readName (juce::InputStream& in)
{
    char buf[20];
    in.read (buf, 20);
    int len = 0;
    while (len < 20 && buf[len] != 0)
        ++len;
    return juce::String::fromUTF8 (buf, len).trim();
}

bool readChunks (juce::FileInputStream& in, juce::int64 end, Sf2Info& info, bool inPdta)
{
    while (in.getPosition() + 8 <= end)
    {
        char id[4];
        in.read (id, 4);
        const juce::int64 size = (juce::uint32) in.readInt();
        const juce::int64 data = in.getPosition();
        const juce::String tag (id, 4);
        const juce::int64 next = data + size + (size & 1);
        if (size < 0 || next > end + 1)
            return false;

        if (tag == "LIST")
        {
            char type[4];
            in.read (type, 4);
            const juce::String listType (type, 4);
            if (listType == "pdta")
                readChunks (in, data + size, info, true);
            else if (listType == "sdta" || listType == "INFO")
                readChunks (in, data + size, info, false);
        }
        else if (tag == "smpl")
        {
            info.smplOffset = data;
            info.smplSize = size;
        }
        else if (tag == "INAM")
        {
            juce::MemoryBlock mb;
            in.readIntoMemoryBlock (mb, (juce::ssize_t) juce::jmin<juce::int64> (size, 256));
            info.name = juce::String::fromUTF8 ((const char*) mb.getData(), (int) mb.getSize()).trim();
        }
        else if (inPdta)
        {
            const int count = (int) size;
            if (tag == "phdr")
                for (int i = 0; i < count / 38; ++i)
                {
                    Sf2Info::PresetHeader p;
                    p.name = readName (in);
                    p.number = (juce::uint16) in.readShort();
                    p.bank = (juce::uint16) in.readShort();
                    p.bag = (juce::uint16) in.readShort();
                    in.skipNextBytes (12);
                    info.presets.push_back (p);
                }
            else if (tag == "pbag" || tag == "ibag")
            {
                auto& bags = tag == "pbag" ? info.pbag : info.ibag;
                for (int i = 0; i < count / 4; ++i)
                {
                    Sf2Info::Bag b;
                    b.gen = (juce::uint16) in.readShort();
                    b.mod = (juce::uint16) in.readShort();
                    bags.push_back (b);
                }
            }
            else if (tag == "pgen" || tag == "igen")
            {
                auto& gens = tag == "pgen" ? info.pgen : info.igen;
                for (int i = 0; i < count / 4; ++i)
                {
                    Sf2Info::Gen g;
                    g.oper = (juce::uint16) in.readShort();
                    g.amount = in.readShort();
                    gens.push_back (g);
                }
            }
            else if (tag == "inst")
                for (int i = 0; i < count / 22; ++i)
                {
                    Sf2Info::InstHeader h;
                    h.name = readName (in);
                    h.bag = (juce::uint16) in.readShort();
                    info.insts.push_back (h);
                }
            else if (tag == "shdr")
                for (int i = 0; i < count / 46; ++i)
                {
                    Sf2Info::SampleHeader h;
                    h.name = readName (in);
                    h.start = (juce::uint32) in.readInt();
                    h.end = (juce::uint32) in.readInt();
                    h.startLoop = (juce::uint32) in.readInt();
                    h.endLoop = (juce::uint32) in.readInt();
                    h.rate = (juce::uint32) in.readInt();
                    h.pitch = (juce::uint8) in.readByte();
                    h.correction = (juce::int8) in.readByte();
                    in.skipNextBytes (2);   // the linked sample
                    h.type = (juce::uint16) in.readShort();
                    info.shdr.push_back (h);
                }
        }
        in.setPosition (next);
    }
    return true;
}

// The generators of one zone
struct GenTable
{
    bool has[64] = {};
    int v[64] = {};
    void overlay (const GenTable& o) { for (int i = 0; i < 64; ++i) if (o.has[i]) { has[i] = true; v[i] = o.v[i]; } }
};

GenTable zoneTable (const std::vector<Sf2Info::Gen>& gens, const std::vector<Sf2Info::Bag>& bags, int zone)
{
    GenTable t;
    if (zone < 0 || zone + 1 >= (int) bags.size())
        return t;
    for (int i = bags[(size_t) zone].gen; i < bags[(size_t) zone + 1].gen && i < (int) gens.size(); ++i)
        if (gens[(size_t) i].oper < 64)
        {
            t.has[gens[(size_t) i].oper] = true;
            t.v[gens[(size_t) i].oper] = gens[(size_t) i].amount;
        }
    return t;
}

int defaultGen (int n)
{
    switch (n)
    {
        case 8: return 13500;
        case 21: case 23: case 25: case 26: case 27: case 28: case 30: case 33: case 34: case 35: case 36: case 38: return -12000;
        case 56: return 100;
        case 46: case 47: case 58: return -1;
        default: return 0;
    }
}

float timeOf (int timecents) { return timecents <= -12000 ? 0.0f : (float) std::exp2 ((double) timecents / 1200.0); }

void buildSf2 (SfBank& bank, const Sf2Info& info, int presetIndex)
{
    bank.regions.clear();
    if (presetIndex < 0 || presetIndex + 1 >= (int) info.presets.size())
        return;

    std::vector<SfSample*> samples (info.shdr.size(), nullptr);
    const auto& ph = info.presets[(size_t) presetIndex];
    const int presetBagEnd = info.presets[(size_t) presetIndex + 1].bag;
    GenTable presetGlobal;

    for (int pz = ph.bag; pz < presetBagEnd; ++pz)
    {
        const auto pt = zoneTable (info.pgen, info.pbag, pz);
        if (! pt.has[41])
        {
            if (pz == ph.bag)
                presetGlobal = pt;
            continue;
        }
        GenTable pre = presetGlobal;
        pre.overlay (pt);

        const int instIndex = pt.v[41];
        if (instIndex < 0 || instIndex + 1 >= (int) info.insts.size())
            continue;

        const int instBagStart = info.insts[(size_t) instIndex].bag, instBagEnd = info.insts[(size_t) instIndex + 1].bag;
        GenTable instGlobal;
        for (int iz = instBagStart; iz < instBagEnd; ++iz)
        {
            const auto it = zoneTable (info.igen, info.ibag, iz);
            if (! it.has[53])
            {
                if (iz == instBagStart)
                    instGlobal = it;
                continue;
            }
            GenTable inst = instGlobal;
            inst.overlay (it);

            auto gen = [&] (int n) { return (inst.has[n] ? inst.v[n] : defaultGen (n)) + (pre.has[n] ? pre.v[n] : 0); };
            auto range = [&] (int n, int& lo, int& hi)
            {
                auto apply = [&] (const GenTable& t)
                {
                    if (t.has[n])
                    {
                        lo = juce::jmax (lo, t.v[n] & 0xff);
                        hi = juce::jmin (hi, (t.v[n] >> 8) & 0xff);
                    }
                };
                apply (inst);
                apply (pre);
            };

            SfRegion r;
            r.loKey = 0; r.hiKey = 127; r.loVel = 0; r.hiVel = 127;
            range (43, r.loKey, r.hiKey);
            range (44, r.loVel, r.hiVel);
            r.loVel = juce::jmax (1, r.loVel);
            if (r.hiKey < r.loKey || r.hiVel < r.loVel)
                continue;

            const int sampleId = inst.v[53];
            if (sampleId < 0 || sampleId >= (int) info.shdr.size())
                continue;
            const auto& sh = info.shdr[(size_t) sampleId];
            if ((sh.type & 0x8000) != 0 || sh.end <= sh.start || (juce::int64) sh.end * 2 > info.smplSize || sh.rate == 0)
                continue;

            if (samples[(size_t) sampleId] == nullptr)
            {
                auto* s = new SfSample();
                s->file = info.file;
                s->byteOffset = info.smplOffset + (juce::int64) sh.start * 2;
                s->frames = (juce::int64) sh.end - sh.start;
                s->rate = (double) sh.rate;
                samples[(size_t) sampleId] = bank.pool->add (s);
            }
            r.sample = samples[(size_t) sampleId];
            const juce::int64 frames = r.sample->frames;

            r.keyCenter = inst.has[58] && inst.v[58] >= 0 ? inst.v[58] : (sh.pitch <= 127 ? sh.pitch : 60);
            r.tuneCents = (float) (gen (51) * 100 + gen (52) + sh.correction);
            r.keyTrack = (float) gen (56);
            r.volumeDb = -(float) juce::jlimit (0, 1440, gen (48)) / 10.0f;
            r.pan = juce::jlimit (-1.0f, 1.0f, (float) gen (17) / 500.0f);

            r.offset = juce::jlimit<juce::int64> (0, frames - 1, (juce::int64) gen (0) + 32768ll * gen (4));
            r.end = juce::jlimit<juce::int64> (r.offset + 1, frames, frames + (juce::int64) gen (1) + 32768ll * gen (12));
            const int mode = inst.has[54] ? inst.v[54] & 3 : 0;
            r.loopMode = mode == 1 ? SfRegion::Continuous : mode == 3 ? SfRegion::Sustain : SfRegion::NoLoop;
            r.loopStart = (juce::int64) sh.startLoop - sh.start + gen (2) + 32768ll * gen (45);
            r.loopEnd = (juce::int64) sh.endLoop - sh.start + gen (3) + 32768ll * gen (50);
            if (r.loopMode != SfRegion::NoLoop && (r.loopStart < 0 || r.loopEnd > frames || r.loopEnd - r.loopStart < 8))
                r.loopMode = SfRegion::NoLoop;

            r.delay = timeOf (gen (33));
            r.attack = timeOf (gen (34));
            r.hold = timeOf (gen (35));
            r.decay = timeOf (gen (36));
            r.release = timeOf (gen (38));
            r.sustain = (float) std::pow (10.0, -(double) juce::jlimit (0, 1000, gen (37)) / 200.0);
            r.keyToHold = (float) gen (39);
            r.keyToDecay = (float) gen (40);
            if (gen (8) < 13500)
            {
                r.cutoffHz = (float) (8.176 * std::exp2 ((double) gen (8) / 1200.0));
                r.resonanceDb = juce::jlimit (0.0f, 40.0f, (float) gen (9) / 10.0f);
            }
            r.group = r.offBy = inst.has[57] ? inst.v[57] : 0;
            r.offFast = true;
            bank.regions.push_back (r);
        }
    }
}
}   // namespace

//==============================================================================
// SFZ files

namespace
{
int noteNumber (const juce::String& text, int fallback)
{
    const auto t = text.trim().toLowerCase();
    if (t.isEmpty())
        return fallback;
    if (t[0] == '-' || juce::CharacterFunctions::isDigit (t[0]))
        return juce::jlimit (0, 127, t.getIntValue());
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };   // a b c d e f g
    if (t[0] < 'a' || t[0] > 'g')
        return fallback;
    int n = base[t[0] - 'a'], i = 1;
    if (i < t.length() && t[i] == '#') { ++n; ++i; }
    else if (i < t.length() && t[i] == 'b') { --n; ++i; }
    return juce::jlimit (0, 127, (t.substring (i).getIntValue() + 1) * 12 + n);
}

using Opcodes = std::map<juce::String, juce::String>;

class SfzReader
{
public:
    SfzReader (SfBank& b, const juce::File& root) : bank (b), rootFile (root) {}

    void read()
    {
        const auto text = preprocess (rootFile, 0);
        tokenize (text);
        flush();
    }

private:
    SfBank& bank;
    juce::File rootFile;
    std::map<juce::String, juce::String> defines;
    Opcodes control, global, master, group, region;
    juce::String header;
    std::map<juce::String, SfSample*> samples;

    juce::String preprocess (const juce::File& file, int depth)
    {
        if (depth > 16 || ! file.existsAsFile())
            return {};
        bank.sourceFiles.addIfNotAlreadyThere (file);

        auto text = file.loadFileAsString();
        juce::String out;
        bool inBlock = false;
        for (auto line : juce::StringArray::fromLines (text))
        {
            juce::String kept;   // the line without comments
            for (int i = 0; i < line.length(); ++i)
            {
                if (inBlock)
                {
                    if (line[i] == '*' && line[i + 1] == '/') { inBlock = false; ++i; }
                    continue;
                }
                if (line[i] == '/' && line[i + 1] == '/')
                    break;
                if (line[i] == '/' && line[i + 1] == '*') { inBlock = true; ++i; continue; }
                kept += line[i];
            }
            line = kept.trim();

            if (line.startsWith ("#define"))
            {
                auto rest = line.substring (7).trim();
                const int space = rest.indexOfAnyOf (" \t");
                if (space > 0)
                    defines[rest.substring (0, space)] = rest.substring (space).trim();
                continue;
            }
            if (line.startsWith ("#include"))
            {
                auto name = line.substring (8).trim().unquoted().replaceCharacter ('\\', '/');
                auto inc = file.getParentDirectory().getChildFile (name);
                if (! inc.existsAsFile())
                    inc = rootFile.getParentDirectory().getChildFile (name);
                out += preprocess (inc, depth + 1) + "\n";
                continue;
            }

            if (! defines.empty() && line.contains ("$"))
            {
                std::vector<juce::String> names;
                for (auto& d : defines) names.push_back (d.first);
                std::sort (names.begin(), names.end(), [] (const juce::String& a, const juce::String& b) { return a.length() > b.length(); });
                for (auto& n : names)
                    line = line.replace (n, defines[n]);
            }
            out += line + "\n";
        }
        return out;
    }

    static bool isNameChar (juce::juce_wchar c) { return juce::CharacterFunctions::isLetterOrDigit (c) || c == '_' || c == '$'; }

    void tokenize (const juce::String& text)
    {
        const int n = text.length();
        Opcodes* target = nullptr;
        Opcodes ignored;
        int i = 0;
        while (i < n)
        {
            const auto c = text[i];
            if (juce::CharacterFunctions::isWhitespace (c)) { ++i; continue; }
            if (c == '<')
            {
                const int close = text.indexOfChar (i, '>');
                if (close < 0)
                    break;
                flush();
                header = text.substring (i + 1, close).trim().toLowerCase();
                i = close + 1;
                if (header == "control")      { control.clear(); target = &control; }
                else if (header == "global")  { global.clear(); master.clear(); group.clear(); target = &global; }
                else if (header == "master")  { master.clear(); group.clear(); target = &master; }
                else if (header == "group")   { group.clear(); target = &group; }
                else if (header == "region")  { region.clear(); target = &region; }
                else                          { ignored.clear(); target = &ignored; }
                continue;
            }

            const int s = i;
            while (i < n && isNameChar (text[i]))
                ++i;
            if (i >= n || text[i] != '=')
            {
                while (i < n && ! juce::CharacterFunctions::isWhitespace (text[i]))
                    ++i;
                continue;
            }
            const auto name = text.substring (s, i).toLowerCase();
            ++i;
            const int valueStart = i;
            int k = i;
            while (k < n)
            {
                if (juce::CharacterFunctions::isWhitespace (text[k]))
                {
                    int m = k;
                    while (m < n && juce::CharacterFunctions::isWhitespace (text[m]))
                        ++m;
                    if (m >= n || text[m] == '<')
                        break;
                    int q = m;
                    while (q < n && isNameChar (text[q]))
                        ++q;
                    if (q > m && q < n && text[q] == '=')
                        break;   // the next opcode begins here
                    k = m;
                }
                else
                    ++k;
            }
            if (target != nullptr)
                (*target)[name] = text.substring (valueStart, k).trim();
            i = k;
        }
    }

    void flush()
    {
        if (header != "region")
            return;
        header = {};

        Opcodes o = global;
        for (auto* layer : { &master, &group, &region })
            for (auto& kv : *layer)
                o[kv.first] = kv.second;
        emit (o);
    }

    void emit (Opcodes& o)
    {
        // aliases of older files
        auto alias = [&] (const char* from, const char* to) { if (o.count (from) && ! o.count (to)) o[to] = o[from]; };
        alias ("loopmode", "loop_mode"); alias ("loopstart", "loop_start"); alias ("loopend", "loop_end");
        alias ("pitch_keycenter", "pitch_keycenter"); alias ("tune", "tune"); alias ("ampeg_sustain", "ampeg_sustain");

        auto text = [&] (const char* k) { auto it = o.find (k); return it == o.end() ? juce::String() : it->second; };
        auto has = [&] (const char* k) { return o.count (k) != 0; };
        auto num = [&] (const char* k, double def) { auto it = o.find (k); return it == o.end() || it->second.isEmpty() ? def : it->second.getDoubleValue(); };

        auto sampleName = text ("sample").replaceCharacter ('\\', '/');
        if (sampleName.isEmpty() || sampleName.startsWith ("*"))   // (generated waveforms are not supported)
            return;

        SfRegion r;
        r.loKey = noteNumber (text ("lokey"), 0);
        r.hiKey = noteNumber (text ("hikey"), 127);
        r.keyCenter = 60;
        if (has ("key"))
            r.loKey = r.hiKey = r.keyCenter = noteNumber (text ("key"), 60);
        if (has ("pitch_keycenter"))
            r.keyCenter = noteNumber (text ("pitch_keycenter"), 60);
        r.loVel = juce::jlimit (1, 127, (int) num ("lovel", 1));
        r.hiVel = juce::jlimit (1, 127, (int) num ("hivel", 127));
        if (r.hiKey < r.loKey || r.hiVel < r.loVel)
            return;

        r.keyTrack = (float) num ("pitch_keytrack", 100.0);
        r.tuneCents = (float) (num ("tune", 0.0) + 100.0 * num ("transpose", 0.0));
        r.volumeDb = (float) num ("volume", 0.0);
        r.pan = juce::jlimit (-1.0f, 1.0f, (float) (num ("pan", 0.0) / 100.0));
        r.ampVelTrack = juce::jlimit (0.0f, 1.0f, (float) (num ("amp_veltrack", 100.0) / 100.0));

        const auto loop = text ("loop_mode");
        r.loopMode = loop == "one_shot" ? SfRegion::OneShot : loop == "loop_continuous" ? SfRegion::Continuous
                   : loop == "loop_sustain" ? SfRegion::Sustain : SfRegion::NoLoop;
        r.offset = (juce::int64) num ("offset", 0.0);
        if (has ("end"))
        {
            const auto e = (juce::int64) num ("end", -1.0);
            if (e < 0)
                return;   // end=-1 means: do not play
            r.end = e + 1;
        }
        if (has ("loop_start")) r.loopStart = (juce::int64) num ("loop_start", 0.0);
        if (has ("loop_end"))   r.loopEnd = (juce::int64) num ("loop_end", 0.0) + 1;   // (the last sample of the loop, inclusive)

        r.delay = (float) (num ("delay", 0.0) + num ("ampeg_delay", 0.0));
        r.attack = (float) num ("ampeg_attack", 0.0);
        r.hold = (float) num ("ampeg_hold", 0.0);
        r.decay = (float) num ("ampeg_decay", 0.0);
        r.sustain = juce::jlimit (0.0f, 1.0f, (float) (num ("ampeg_sustain", 100.0) / 100.0));
        r.release = (float) num ("ampeg_release", 0.0);

        const auto filter = text ("fil_type");
        if (has ("cutoff") && (filter.isEmpty() || filter.startsWith ("lpf")))
        {
            r.cutoffHz = (float) num ("cutoff", 0.0);
            r.resonanceDb = juce::jlimit (0.0f, 40.0f, (float) num ("resonance", 0.0));
        }

        const auto trigger = text ("trigger");
        r.triggerRelease = trigger == "release" || trigger == "release_key";
        r.triggerMode = trigger == "first" ? 1 : trigger == "legato" ? 2 : 0;
        r.seqLength = juce::jmax (1, (int) num ("seq_length", 1));
        r.seqPosition = juce::jlimit (1, r.seqLength, (int) num ("seq_position", 1));
        r.loRand = (float) num ("lorand", 0.0);
        r.hiRand = (float) num ("hirand", 1.0);
        r.group = (int) num ("group", 0);
        r.offBy = (int) num ("off_by", 0);
        r.offFast = text ("off_mode") != "normal";
        if (has ("sw_last"))
            r.swLast = noteNumber (text ("sw_last"), -1);
        if (has ("sw_lokey") && has ("sw_hikey"))
        {
            bank.swLo = noteNumber (text ("sw_lokey"), -1);
            bank.swHi = noteNumber (text ("sw_hikey"), -1);
        }
        if (has ("sw_default"))
            bank.swDefault = noteNumber (text ("sw_default"), -1);

        // the sample file
        auto path = rootFile.getParentDirectory();
        const auto defaultPath = control.count ("default_path") ? control["default_path"].replaceCharacter ('\\', '/') : juce::String();
        if (defaultPath.isNotEmpty())
            path = path.getChildFile (defaultPath);
        const auto file = path.getChildFile (sampleName);
        const auto key = file.getFullPathName();
        auto it = samples.find (key);
        if (it == samples.end())
        {
            auto* s = new SfSample();
            s->file = file;
            it = samples.emplace (key, bank.pool->add (s)).first;
            if (! file.existsAsFile())
                bank.missingFiles.add (file.getFullPathName());
        }
        r.sample = it->second;
        if (bank.regions.size() < 200000)
            bank.regions.push_back (r);
    }
};

void finishBank (SfBank& bank)
{
    for (auto& k : bank.byKey)
        k.clear();
    for (int i = 0; i < (int) bank.regions.size(); ++i)
        for (int key = bank.regions[(size_t) i].loKey; key <= bank.regions[(size_t) i].hiKey && key < 128; ++key)
            bank.byKey[(size_t) key].push_back (i);
}
}   // namespace

//==============================================================================
// SfBank

std::shared_ptr<SfBank> SfBank::load (const juce::File& file, int presetIndex, juce::String& error)
{
    if (! file.existsAsFile())
    {
        error = "The file does not exist.";
        return nullptr;
    }
    const auto ext = file.getFileExtension().toLowerCase();

    auto bank = std::make_shared<SfBank>();
    bank->file = file;
    bank->name = file.getFileNameWithoutExtension();
    bank->pool = std::make_shared<SfPool>();

    if (ext == ".sf2")
    {
        auto info = std::make_shared<Sf2Info>();
        info->file = file;
        juce::FileInputStream in (file);
        char riff[4], form[4];
        if (! in.openedOk() || in.getTotalLength() < 12 || in.read (riff, 4) != 4 || juce::String (riff, 4) != "RIFF")
        {
            error = "This is not a SoundFont file.";
            return nullptr;
        }
        in.readInt();
        in.read (form, 4);
        if (juce::String (form, 4) != "sfbk" || ! readChunks (in, in.getTotalLength(), *info, false)
            || info->smplOffset < 0 || info->presets.size() < 2 || info->shdr.empty())
        {
            error = "This SoundFont is damaged or uses an unsupported layout (SF3 compressed files are not supported).";
            return nullptr;
        }
        bank->format = Sf2;
        if (info->name.isNotEmpty())
            bank->name = info->name;
        for (size_t i = 0; i + 1 < info->presets.size(); ++i)
            bank->presets.push_back ({ info->presets[i].name, info->presets[i].bank, info->presets[i].number });
        bank->presetIndex = juce::jlimit (0, (int) bank->presets.size() - 1, presetIndex);
        bank->sf2Info = info;
        buildSf2 (*bank, *info, bank->presetIndex);
    }
    else if (ext == ".sfz")
    {
        bank->format = Sfz;
        bank->presets.push_back ({ bank->name, 0, 0 });
        SfzReader (*bank, file).read();
    }
    else
    {
        error = "Choose a .sf2 (SoundFont) or .sfz file.";
        return nullptr;
    }

    if (bank->regions.empty())
    {
        error = bank->format == Sf2 ? "This preset has no samples." : "This SFZ file has no regions with a sample.";
        return nullptr;
    }
    finishBank (*bank);
    bank->pool->start();
    return bank;
}

std::shared_ptr<SfBank> SfBank::withPreset (int index) const
{
    if (format != Sf2 || sf2Info == nullptr || index < 0 || index >= (int) presets.size())
        return nullptr;

    auto bank = std::make_shared<SfBank>();
    bank->format = Sf2;
    bank->file = file;
    bank->name = name;
    bank->presets = presets;
    bank->presetIndex = index;
    bank->sf2Info = sf2Info;
    bank->pool = std::make_shared<SfPool>();
    buildSf2 (*bank, *static_cast<const Sf2Info*> (sf2Info.get()), index);
    if (bank->regions.empty())
        return nullptr;
    finishBank (*bank);
    bank->pool->start();
    return bank;
}

juce::String SfBank::describe() const
{
    if (format == Sf2 && presetIndex >= 0 && presetIndex < (int) presets.size())
        return name + " - " + presets[(size_t) presetIndex].name;
    return name;
}

//==============================================================================
// SfInstrument

SfInstrument::SfInstrument() = default;

void SfInstrument::setBank (std::shared_ptr<const SfBank> b)
{
    reset();
    bank = std::move (b);
    lastSwitch = bank != nullptr ? bank->swDefault : -1;
}

void SfInstrument::applyState (const State& s)
{
    setBank (s.bank);
    volume = s.volume;
}

bool SfInstrument::isActive() const
{
    for (auto& v : voices)
        if (v.stage != Voice::Idle)
            return true;
    return false;
}

void SfInstrument::reset()
{
    for (auto& v : voices)
        v.stage = Voice::Idle;
    std::fill (std::begin (keyDown), std::end (keyDown), false);
    std::fill (std::begin (sustained), std::end (sustained), false);
    std::fill (std::begin (seqCount), std::end (seqCount), 0);
    numDown = 0;
    pedal = false;
    bendSemitones = 0.0f;
    ccVolume = ccExpression = 1.0f;
    ccPan = 0.0f;
}

void SfInstrument::allNotesOff()
{
    pedal = false;
    std::fill (std::begin (keyDown), std::end (keyDown), false);
    std::fill (std::begin (sustained), std::end (sustained), false);
    numDown = 0;
    for (auto& v : voices)
        if (! v.ignoreRelease)
            releaseVoice (v, false);
}

void SfInstrument::releaseVoice (Voice& v, bool fast)
{
    if (v.stage == Voice::Idle || (v.released && ! fast))
        return;
    v.released = true;
    if (v.region != nullptr && v.region->loopMode == SfRegion::Sustain)
        v.looping = false;   // the rest of the sample plays out
    if (fast)
        v.releaseCoef = std::exp (-std::log (1000.0) / (0.006 * outRate));
    v.stage = Voice::Release;
}

void SfInstrument::releaseKey (int note)
{
    for (auto& v : voices)
        if (v.stage != Voice::Idle && v.note == note && ! v.released && ! v.ignoreRelease)
            releaseVoice (v, false);
}

void SfInstrument::noteOn (int note, float velocity)
{
    if (bank == nullptr || note < 0 || note > 127)
        return;
    const int vel = juce::jlimit (1, 127, (int) std::lround (velocity * 127.0f));

    if (bank->swLo >= 0 && note >= bank->swLo && note <= bank->swHi)
        lastSwitch = note;

    const bool legato = numDown > 0;
    if (! keyDown[note])
        ++numDown;
    keyDown[note] = true;
    sustained[note] = false;
    lastVelocity[note] = vel;
    const int seq = seqCount[note]++;
    randomState = randomState * 1664525u + 1013904223u;
    const float random = (float) (randomState >> 8) / 16777216.0f;

    for (int idx : bank->byKey[(size_t) note])
    {
        const auto& r = bank->regions[(size_t) idx];
        if (r.triggerRelease || vel < r.loVel || vel > r.hiVel)
            continue;
        if ((r.triggerMode == 1 && legato) || (r.triggerMode == 2 && ! legato))
            continue;
        if (r.swLast >= 0 && r.swLast != lastSwitch)
            continue;
        if (random < r.loRand || random >= r.hiRand)
            continue;
        if (r.seqLength > 1 && (seq % r.seqLength) + 1 != r.seqPosition)
            continue;
        startVoice (r, note, vel, false);
    }
}

void SfInstrument::noteOff (int note)
{
    if (bank == nullptr || note < 0 || note > 127 || ! keyDown[note])
        return;
    keyDown[note] = false;
    --numDown;

    if (pedal)
        sustained[note] = true;
    else
        releaseKey (note);

    const int vel = lastVelocity[note];
    const int seq = juce::jmax (0, seqCount[note] - 1);
    for (int idx : bank->byKey[(size_t) note])
    {
        const auto& r = bank->regions[(size_t) idx];
        if (r.triggerRelease && vel >= r.loVel && vel <= r.hiVel && (r.seqLength <= 1 || (seq % r.seqLength) + 1 == r.seqPosition))
            startVoice (r, note, vel, true);
    }
}

void SfInstrument::control (int controller, float value)
{
    switch (controller)
    {
        case 64:
        {
            const bool down = value >= 0.5f;
            if (pedal && ! down)
            {
                pedal = false;
                for (int k = 0; k < 128; ++k)
                    if (sustained[k])
                    {
                        sustained[k] = false;
                        releaseKey (k);
                    }
            }
            else
                pedal = down;
            break;
        }
        case 7:   ccVolume = value * value; break;
        case 11:  ccExpression = value * value; break;
        case 10:  ccPan = juce::jlimit (-1.0f, 1.0f, value * 2.0f - 1.0f); break;
        case 128: bendSemitones = juce::jlimit (-1.0f, 1.0f, value * 2.0f - 1.0f) * 2.0f; break;
        case 120: case 123: allNotesOff(); break;
        default: break;
    }
}

void SfInstrument::startVoice (const SfRegion& r, int note, int velocity, bool fromRelease)
{
    if (r.sample == nullptr || bank == nullptr)
        return;
    const auto* buffer = r.sample->ready.load (std::memory_order_acquire);
    if (buffer == nullptr)
    {
        if (bank->pool != nullptr)
            bank->pool->request (*r.sample);   // (silent this time; the next note finds it)
        return;
    }

    if (r.group != 0)   // a note of this group cuts the ones it is `off_by`
        for (auto& v : voices)
            if (v.stage != Voice::Idle && v.region != nullptr && v.region->offBy == r.group)
                releaseVoice (v, v.region->offFast);

    Voice* slot = nullptr;
    for (auto& v : voices)
        if (v.stage == Voice::Idle) { slot = &v; break; }
    if (slot == nullptr)   // steal: the oldest released voice, else the oldest
    {
        for (auto& v : voices)
            if (v.released && (slot == nullptr || v.age < slot->age))
                slot = &v;
        if (slot == nullptr)
            for (auto& v : voices)
                if (slot == nullptr || v.age < slot->age)
                    slot = &v;
    }

    const double frames = (double) buffer->getNumSamples();
    Voice v;
    v.region = &r;
    v.buffer = buffer;
    v.note = note;
    v.ignoreRelease = fromRelease || r.loopMode == SfRegion::OneShot;
    v.age = ++counter;

    const double cents = (double) (note - r.keyCenter) * (double) r.keyTrack + (double) r.tuneCents;
    v.step = r.sample->rate / outRate * std::exp2 (cents / 1200.0);
    v.position = (double) juce::jlimit<juce::int64> (0, (juce::int64) frames - 2, r.offset);
    v.end = r.end >= 0 ? juce::jmin ((double) r.end, frames) : frames;
    if (v.end < v.position + 2.0)
        return;
    v.looping = r.loopMode == SfRegion::Continuous || r.loopMode == SfRegion::Sustain;
    v.loopStart = r.loopStart >= 0 ? (double) r.loopStart : 0.0;
    v.loopEnd = r.loopEnd >= 0 ? juce::jmin ((double) r.loopEnd, frames) : v.end;
    if (v.loopEnd - v.loopStart < 4.0)
        v.looping = false;

    const float vn = (float) velocity / 127.0f;
    const float velocityGain = (1.0f - r.ampVelTrack) + r.ampVelTrack * vn * vn;
    const float gain = std::pow (10.0f, r.volumeDb / 20.0f) * velocityGain;
    const float angle = (juce::jlimit (-1.0f, 1.0f, r.pan) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    v.gainL = gain * std::cos (angle) * juce::MathConstants<float>::sqrt2;
    v.gainR = gain * std::sin (angle) * juce::MathConstants<float>::sqrt2;

    const double keyShift = (double) (60 - note) / 1200.0;
    const double hold = (double) r.hold * std::exp2 (keyShift * (double) r.keyToHold);
    const double decay = (double) r.decay * std::exp2 (keyShift * (double) r.keyToDecay);
    const double attack = juce::jmax (0.0005, (double) r.attack);
    v.attackStep = 1.0 / (attack * outRate);
    v.decayCoef = decay > 0.0 ? std::exp (-std::log (1000.0) / (decay * outRate)) : 0.0;
    v.releaseCoef = std::exp (-std::log (1000.0) / (juce::jmax (0.003, (double) r.release) * outRate));
    v.sustainLevel = r.sustain;
    v.level = 0.0;
    if (r.delay > 0.0f)
    {
        v.stage = Voice::Delay;
        v.counter = (int) (r.delay * outRate);
    }
    else
        v.stage = Voice::Attack;
    // (the hold time is applied when the attack ends)
    v.z1L = v.z2L = v.z1R = v.z2R = 0.0f;
    if (r.cutoffHz > 20.0f && r.cutoffHz < 0.45f * (float) outRate)
    {
        const double q = 0.70710678 * std::pow (10.0, (double) r.resonanceDb / 20.0);
        const double w0 = 2.0 * juce::MathConstants<double>::pi * (double) r.cutoffHz / outRate;
        const double alpha = std::sin (w0) / (2.0 * q), c = std::cos (w0), a0 = 1.0 + alpha;
        v.b0 = (float) ((1.0 - c) * 0.5 / a0);
        v.b1 = (float) ((1.0 - c) / a0);
        v.b2 = v.b0;
        v.a1 = (float) (-2.0 * c / a0);
        v.a2 = (float) ((1.0 - alpha) / a0);
        v.filter = true;
    }
    v.fast = false;
    v.holdLen = (int) (hold * outRate);
    *slot = v;
}

void SfInstrument::renderVoice (Voice& v, float* outL, float* outR, int n, double bendFactor, float ccGain, float ccPan)
{
    const auto* d0 = v.buffer->getReadPointer (0);
    const auto* d1 = v.buffer->getNumChannels() > 1 ? v.buffer->getReadPointer (1) : d0;
    const bool stereo = v.buffer->getNumChannels() > 1;
    const double step = v.step * bendFactor;
    const float panL = ccPan > 0.0f ? 1.0f - ccPan : 1.0f, panR = ccPan < 0.0f ? 1.0f + ccPan : 1.0f;
    const int endInt = (int) v.end, loopStartInt = (int) v.loopStart, loopEndInt = (int) v.loopEnd;

    for (int i = 0; i < n; ++i)
    {
        switch (v.stage)
        {
            case Voice::Delay:
                if (--v.counter <= 0)
                    v.stage = Voice::Attack;
                continue;
            case Voice::Attack:
                v.level += v.attackStep;
                if (v.level >= 1.0)
                {
                    v.level = 1.0;
                    v.counter = v.holdLen;
                    v.stage = v.counter > 0 ? Voice::Hold : Voice::Decay;
                }
                break;
            case Voice::Hold:
                if (--v.counter <= 0)
                    v.stage = Voice::Decay;
                break;
            case Voice::Decay:
                if (v.sustainLevel >= 1.0)
                {
                    v.stage = Voice::Sustain;
                }
                else if (v.decayCoef <= 0.0)
                {
                    v.level = v.sustainLevel;
                    v.stage = Voice::Sustain;
                }
                else
                {
                    v.level *= v.decayCoef;
                    if (v.level <= v.sustainLevel)
                    {
                        v.level = v.sustainLevel;
                        v.stage = Voice::Sustain;
                    }
                }
                if (v.level < 1.0e-5)
                    v.stage = Voice::Idle;
                break;
            case Voice::Sustain:
                if (v.sustainLevel < 1.0e-5)
                    v.stage = Voice::Idle;
                break;
            case Voice::Release:
                v.level *= v.releaseCoef;
                if (v.level < 1.0e-5)
                    v.stage = Voice::Idle;
                break;
            case Voice::Idle:
                return;
        }
        if (v.stage == Voice::Idle)
            return;

        const int i0 = (int) v.position;
        int i1 = i0 + 1;
        if (v.looping && i1 >= loopEndInt)
            i1 = loopStartInt;
        else if (i1 >= endInt)
            i1 = i0;
        const float frac = (float) (v.position - (double) i0);
        float l = d0[i0] + frac * (d0[i1] - d0[i0]);
        float r = stereo ? d1[i0] + frac * (d1[i1] - d1[i0]) : l;

        if (v.filter)
        {
            const float yl = v.b0 * l + v.z1L;
            v.z1L = v.b1 * l - v.a1 * yl + v.z2L;
            v.z2L = v.b2 * l - v.a2 * yl;
            const float yr = v.b0 * r + v.z1R;
            v.z1R = v.b1 * r - v.a1 * yr + v.z2R;
            v.z2R = v.b2 * r - v.a2 * yr;
            l = yl;
            r = yr;
        }

        const float g = (float) v.level * ccGain;
        outL[i] += l * g * v.gainL * panL;
        outR[i] += r * g * v.gainR * panR;

        v.position += step;
        if (v.looping)
        {
            while (v.position >= v.loopEnd)
                v.position -= v.loopEnd - v.loopStart;
        }
        else if (v.position >= v.end)
        {
            v.stage = Voice::Idle;
            return;
        }
    }
}

void SfInstrument::render (float* outL, float* outR, int numSamples)
{
    if (bank == nullptr)
        return;
    const double bend = std::exp2 ((double) bendSemitones / 12.0);
    const float gain = volume.load() * ccVolume * ccExpression;
    for (auto& v : voices)
        if (v.stage != Voice::Idle)
            renderVoice (v, outL, outR, numSamples, bend, gain, ccPan);
}
