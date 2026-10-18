#include "Sampler.h"
#include <cmath>

void Sampler::setSample (std::shared_ptr<const juce::AudioBuffer<float>> buffer, double rate, const juce::File& newFile, const juce::String& newName)
{
    reset();
    original = buffer;
    stretchRatio = 1.0;
    sample = std::move (buffer);
    sampleRate = rate > 0.0 ? rate : 44100.0;
    file = newFile;
    name = newName;
}

void Sampler::reset()
{
    for (auto& v : voices)
        v.active = false;
}

bool Sampler::isActive() const
{
    for (auto& v : voices)
        if (v.active)
            return true;
    return false;
}

void Sampler::allNotesOff()
{
    for (auto& v : voices)
        if (v.active)
        {
            v.released = true;
            v.stage = Voice::Release;
        }
}

void Sampler::noteOn (int note, float velocity)
{
    if (sample == nullptr || sample->getNumSamples() < 2)
        return;
    start (note, velocity);
}

void Sampler::noteOff (int note)
{
    for (auto& v : voices)
        if (v.active && v.note == note && ! v.released && ! v.oneShotLike)
        {
            v.released = true;
            v.stage = Voice::Release;
        }
}

void Sampler::start (int note, float velocity)
{
    const int total = sample->getNumSamples();
    const int mode = params.mode.load();
    const double s = juce::jlimit (0.0f, 1.0f, params.start.load()), e = juce::jlimit (0.0f, 1.0f, params.end.load());
    const double from = juce::jmin (s, e) * (double) total, to = juce::jmax (s, e) * (double) total;
    if (to - from < 2.0)
        return;

    Voice v;
    v.active = true;
    v.note = note;
    v.velocity = juce::jlimit (0.0f, 1.0f, velocity);
    v.age = ++counter;
    v.oneShotLike = mode != Classic;
    v.level = 0.0f;
    v.stage = Voice::Attack;

    double ratio = sampleRate / outRate;   // source frames per output frame at the original pitch
    double regionStart = from, regionEnd = to;

    if (mode == Slice)
    {
        const int marked = params.markCount.load();
        const int count = marked > 0 ? marked : juce::jlimit (1, 64, params.slices.load());
        const int index = note - params.sliceNote.load();
        if (index < 0 || index >= count)
            return;
        if (marked > 0)   // slices that follow the hits
        {
            regionStart = juce::jlimit (from, to, (double) params.marks[index].load() * (double) total);
            regionEnd = index + 1 < marked ? juce::jlimit (from, to, (double) params.marks[index + 1].load() * (double) total) : to;
            if (regionEnd - regionStart < 2.0)
                return;
        }
        else
        {
            const double length = (to - from) / (double) count;
            regionStart = from + length * (double) index;
            regionEnd = regionStart + length;
        }
    }
    else
    {
        const double semitones = (double) (note - params.rootNote.load()) + (double) params.tuneCents.load() / 100.0;
        ratio *= std::pow (2.0, semitones / 12.0);
    }

    v.regionStart = regionStart;
    v.regionEnd = regionEnd;
    const bool reverse = params.reverse.load();
    v.step = reverse ? -ratio : ratio;
    v.position = reverse ? regionEnd - 1.001 : regionStart;   // (just inside the last frame: interpolating needs the one after it)

    if (mode == Classic && params.loop.load())
    {
        const double ls = juce::jlimit (0.0f, 1.0f, params.loopStart.load()), le = juce::jlimit (0.0f, 1.0f, params.loopEnd.load());
        v.loopStart = juce::jmax (regionStart, juce::jmin (ls, le) * (double) total);
        v.loopEnd = juce::jmin (regionEnd, juce::jmax (ls, le) * (double) total);
        v.looping = v.loopEnd - v.loopStart > 16.0;
    }

    // a free voice, or the oldest one
    Voice* target = nullptr;
    for (auto& slot : voices)
        if (! slot.active)
        {
            target = &slot;
            break;
        }
    if (target == nullptr)
    {
        target = &voices[0];
        for (auto& slot : voices)
            if (slot.age < target->age)
                target = &slot;
    }
    *target = v;
}

void Sampler::render (float* outL, float* outR, int n)
{
    if (sample == nullptr)
        return;

    const auto& buf = *sample;
    const int total = buf.getNumSamples();
    const float* left = buf.getReadPointer (0);
    const float* right = buf.getNumChannels() > 1 ? buf.getReadPointer (1) : left;

    const float attack = juce::jmax (0.0005f, params.attack.load()), decay = juce::jmax (0.001f, params.decay.load());
    const float sustain = juce::jlimit (0.0f, 1.0f, params.sustain.load()), release = juce::jmax (0.002f, params.release.load());
    const float attackInc = 1.0f / (attack * (float) outRate);
    const float decayDec = (1.0f - sustain) / (decay * (float) outRate);
    const float releaseDec = 1.0f / (release * (float) outRate);
    const float chokeDec = 1.0f / (0.006f * (float) outRate);   // a choked voice is gone in about 6 ms
    const float volume = params.volume.load(), sens = juce::jlimit (0.0f, 1.0f, params.velocitySens.load());

    const float cutoff = juce::jlimit (40.0f, 20000.0f, params.cutoff.load());
    const bool filtering = cutoff < 19000.0f;
    const float a = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi * cutoff / (float) outRate);

    for (auto& v : voices)
    {
        if (! v.active)
            continue;

        const float gain = volume * (1.0f - sens + sens * v.velocity * v.velocity);

        for (int i = 0; i < n; ++i)
        {
            // end of the region (or the loop)
            if (v.looping && ! v.released)
            {
                if (v.step > 0.0 && v.position >= v.loopEnd)
                    v.position -= (v.loopEnd - v.loopStart);
                else if (v.step < 0.0 && v.position <= v.loopStart)
                    v.position += (v.loopEnd - v.loopStart);
            }
            if ((v.step > 0.0 && v.position >= v.regionEnd - 1.0) || (v.step < 0.0 && v.position <= v.regionStart) || v.position < 0.0
                || v.position >= (double) (total - 1))
            {
                v.active = false;
                break;
            }

            // envelope
            switch (v.stage)
            {
                case Voice::Attack:  v.level += attackInc;  if (v.level >= 1.0f) { v.level = 1.0f; v.stage = Voice::Decay; } break;
                case Voice::Decay:   v.level -= decayDec;   if (v.level <= sustain) { v.level = sustain; v.stage = Voice::Sustain; } break;
                case Voice::Sustain: v.level = sustain; break;
                case Voice::Release: v.level -= v.choked ? chokeDec : releaseDec; if (v.level <= 0.0f) { v.level = 0.0f; v.active = false; } break;
            }
            if (! v.active)
                break;

            // the sample, interpolated
            const int i0 = (int) v.position;
            const float frac = (float) (v.position - (double) i0);
            float l = left[i0] + frac * (left[i0 + 1] - left[i0]);
            float r = right[i0] + frac * (right[i0 + 1] - right[i0]);

            // a short fade at the very end of the region keeps a cut sample from clicking
            const double remaining = v.step > 0.0 ? v.regionEnd - v.position : v.position - v.regionStart;
            float edge = 1.0f;
            if (! v.looping && remaining < 64.0 * std::abs (v.step))
                edge = (float) (remaining / (64.0 * std::abs (v.step)));

            if (filtering)
            {
                v.lowL += a * (l - v.lowL);   v.low2L += a * (v.lowL - v.low2L);   l = v.low2L;
                v.lowR += a * (r - v.lowR);   v.low2R += a * (v.lowR - v.low2R);   r = v.low2R;
            }

            const float g = gain * v.level * edge;
            outL[i] += l * g;
            outR[i] += r * g;
            v.position += v.step;
        }
    }
}

void Sampler::choke()
{
    for (auto& v : voices)
        if (v.active)
        {
            v.released = true;
            v.choked = true;
            v.stage = Voice::Release;
        }
}

void Sampler::setMarks (const std::vector<float>& starts)
{
    const int n = juce::jmin ((int) starts.size(), Params::maxMarks);
    params.markCount = 0;   // (the audio thread sees either the old list or none while it is rewritten)
    for (int i = 0; i < n; ++i)
        params.marks[i].store (starts[(size_t) i]);
    params.markCount = n;
    if (n > 0)
        params.slices = n;
}

std::vector<std::pair<double, double>> Sampler::sliceTimes() const
{
    std::vector<std::pair<double, double>> out;
    if (sample == nullptr || sampleRate <= 0.0)
        return out;
    const double total = (double) sample->getNumSamples();
    const double from = juce::jmin (params.start.load(), params.end.load()) * total, to = juce::jmax (params.start.load(), params.end.load()) * total;
    const int marked = params.markCount.load();
    const int count = marked > 0 ? marked : juce::jlimit (1, 64, params.slices.load());
    std::vector<double> starts;
    for (int i = 0; i < count; ++i)
        starts.push_back (marked > 0 ? juce::jlimit (from, to, (double) params.marks[i].load() * total) : from + (to - from) * i / count);
    for (int i = 0; i < count; ++i)
    {
        const double end = i + 1 < count ? starts[(size_t) i + 1] : to;
        if (end - starts[(size_t) i] >= 2.0)
            out.push_back ({ (starts[(size_t) i] - starts.front()) / sampleRate, (end - starts[(size_t) i]) / sampleRate });
        else
            out.push_back ({ (starts[(size_t) i] - starts.front()) / sampleRate, 0.0 });
    }
    return out;
}

Sampler::State Sampler::captureState() const
{
    State s;
    for (int i = 0, n = params.markCount.load(); i < n; ++i)
        s.marks.push_back (params.marks[i].load());
    s.mode = params.mode;            s.rootNote = params.rootNote;      s.slices = params.slices;       s.sliceNote = params.sliceNote;
    s.tuneCents = params.tuneCents;  s.start = params.start;            s.end = params.end;
    s.loopStart = params.loopStart;  s.loopEnd = params.loopEnd;        s.loop = params.loop;           s.reverse = params.reverse;
    s.attack = params.attack;        s.decay = params.decay;            s.sustain = params.sustain;     s.release = params.release;
    s.cutoff = params.cutoff;        s.volume = params.volume;          s.velocitySens = params.velocitySens;
    s.sample = sample;
    s.original = original;
    s.sampleRate = sampleRate;
    s.stretch = stretchRatio;
    s.file = file;
    s.name = name;
    return s;
}

void Sampler::applyState (const State& s)
{
    reset();
    params.mode = s.mode;            params.rootNote = s.rootNote;      params.slices = s.slices;       params.sliceNote = s.sliceNote;
    params.tuneCents = s.tuneCents;  params.start = s.start;            params.end = s.end;
    params.loopStart = s.loopStart;  params.loopEnd = s.loopEnd;        params.loop = s.loop;           params.reverse = s.reverse;
    params.attack = s.attack;        params.decay = s.decay;            params.sustain = s.sustain;     params.release = s.release;
    params.cutoff = s.cutoff;        params.volume = s.volume;          params.velocitySens = s.velocitySens;
    {
        const int slicesBefore = s.slices;
        setMarks (s.marks);
        params.slices = slicesBefore;
    }
    sample = s.sample;
    original = s.original;
    sampleRate = s.sampleRate;
    stretchRatio = s.stretch;
    file = s.file;
    name = s.name;
}
