#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>
#include <vector>

// Finding the tempo and the hits of audio (used by the sampler's tempo calculator and slicing, and by the clip warp editor).

// Estimates the tempo of audio from where the sound jumps (BPM, 60 - 200): an onset curve made of three frequency bands (kicks, snares,
// hats), then a comb of evenly spaced beats is slid over it at every tempo. When the region is a whole number of beats long (a loop cut
// to the bar) that length decides the exact tempo. 0 = nothing found.
inline double detectBpm (const juce::AudioBuffer<float>& audio, double sampleRate, int startSample, int endSample)
{
    constexpr int hop = 256;
    startSample = juce::jlimit (0, audio.getNumSamples(), startSample);
    endSample = juce::jlimit (startSample, juce::jmin (audio.getNumSamples(), startSample + (int) (sampleRate * 90.0)), endSample);
    const int frames = (endSample - startSample) / hop;
    if (frames < 64 || sampleRate <= 0.0)
        return 0.0;
    const double seconds = (double) (endSample - startSample) / sampleRate;

    // three bands by one-pole filters: low (< 150 Hz), mid (to 1.5 kHz), high; the loudness of each per hop, compressed
    const int channels = juce::jmax (1, audio.getNumChannels());
    const double a1 = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 150.0 / sampleRate);
    const double a2 = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 1500.0 / sampleRate);
    std::vector<float> env[3];
    for (auto& e : env)
        e.assign ((size_t) frames, 0.0f);
    double lp1 = 0.0, lp2 = 0.0;
    for (int f = 0; f < frames; ++f)
    {
        double sum[3] = { 0.0, 0.0, 0.0 };
        for (int i = 0; i < hop; ++i)
        {
            double x = 0.0;
            for (int ch = 0; ch < channels; ++ch)
                x += audio.getSample (juce::jmin (ch, audio.getNumChannels() - 1), startSample + f * hop + i);
            x /= channels;
            lp1 += a1 * (x - lp1);
            lp2 += a2 * (x - lp2);
            const double low = lp1, mid = lp2 - lp1, high = x - lp2;
            sum[0] += low * low;
            sum[1] += mid * mid;
            sum[2] += high * high;
        }
        for (int b = 0; b < 3; ++b)
            env[b][(size_t) f] = (float) std::log1p (300.0 * std::sqrt (sum[b] / hop));
    }

    std::vector<float> onset ((size_t) frames, 0.0f);
    for (int b = 0; b < 3; ++b)
        for (int f = 1; f < frames; ++f)
            onset[(size_t) f] += juce::jmax (0.0f, env[b][(size_t) f] - env[b][(size_t) f - 1]);
    double mean = 0.0;
    for (auto o : onset)
        mean += o;
    mean /= frames;
    if (mean < 1.0e-6)
        return 0.0;
    for (auto& o : onset)
        o = juce::jmax (0.0f, o - (float) mean);   // (only what stands out above the average counts)

    const double fps = sampleRate / hop;
    auto at = [&] (double pos)   // the onset curve between frames
    {
        const int i = (int) pos;
        if (i < 0 || i + 1 >= frames)
            return 0.0;
        const double fr = pos - i;
        return (1.0 - fr) * onset[(size_t) i] + fr * onset[(size_t) i + 1];
    };

    // the comb: for each tempo, the average onset on a grid of beats, at the best offset
    auto combScore = [&] (double bpm, bool total = false)
    {
        const double period = 60.0 * fps / bpm;
        double best = 0.0;
        for (double phase = 0.0; phase < period; phase += 1.0)
        {
            double s = 0.0;
            int n = 0;
            for (double p = phase; p < (double) frames - 1.0; p += period, ++n)
                s += at (p);
            if (n >= 3)
                best = juce::jmax (best, total ? s : s / n);
        }
        return best;
    };

    std::vector<std::pair<double, double>> scores;   // (bpm, score)
    double bestScore = 0.0, bestBpm = 0.0;
    for (double bpm = 60.0; bpm <= 200.0; bpm += 0.25)
    {
        const double w = std::exp (-0.5 * std::pow (std::log2 (bpm / 115.0) / 0.8, 2.0));   // a preference for the usual range
        const double score = combScore (bpm) * (0.7 + 0.3 * w);
        scores.push_back ({ bpm, score });
        if (score > bestScore)
        {
            bestScore = score;
            bestBpm = bpm;
        }
    }
    if (bestBpm <= 0.0)
        return 0.0;

    // a tempo twice as fast that is just as well supported (every beat has its own hit) is the one that is meant
    if (bestBpm * 2.0 <= 175.0 && combScore (bestBpm * 2.0) >= 0.75 * combScore (bestBpm))
        bestBpm *= 2.0;
    // slow readings are usually the half of the real tempo (a break with ghost notes between the backbeat): below 85 the double wins unless
    // clearly less supported (the half / double buttons of the tempo calculator fix the rare slow loop)
    while (bestBpm < 85.0 && bestBpm * 2.0 <= 175.0 && combScore (bestBpm * 2.0) >= 0.5 * combScore (bestBpm))
        bestBpm *= 2.0;

    // refine round the best one: the total of the onsets on the grid, in small steps (a wrong tempo drifts off the hits)
    double refined = bestBpm, refinedScore = 0.0;
    for (double bpm = bestBpm * 0.97; bpm <= bestBpm * 1.03; bpm += 0.02)
    {
        const double s = combScore (bpm, true);
        if (s > refinedScore)
        {
            refinedScore = s;
            refined = bpm;
        }
    }
    // A loop cut to whole beats: of the tempos that make the region exactly 1, 2, 3... beats long, the one whose beat grid catches the most
    // onsets wins when it is nearly as well supported as the free tempo (the free search can lock on a pattern like "every 2.5 beats").
    auto prior = [] (double bpm) { return 0.7 + 0.3 * std::exp (-0.5 * std::pow (std::log2 (bpm / 115.0) / 0.8, 2.0)); };
    double loopBpm = 0.0, loopTotal = 0.0;
    for (int n = 2; n <= 256; ++n)
    {
        const double bpm = 60.0 * n / seconds;
        if (bpm < 60.0 || bpm > 200.0)
            continue;
        const double total = combScore (bpm, true) * prior (bpm);
        if (total > loopTotal)
        {
            loopTotal = total;
            loopBpm = bpm;
        }
    }
    if (loopBpm > 0.0 && loopTotal >= 0.85 * refinedScore * prior (refined))
    {
        // a half-speed grid that catches most of the same onsets is the slower beat (a fast tempo also catches every off-beat hat)
        for (int guard = 0; guard < 3 && loopBpm * 0.5 >= 60.0; ++guard)
        {
            const double half = combScore (loopBpm * 0.5, true) * prior (loopBpm * 0.5);
            if (half < 0.72 * loopTotal)
                break;
            loopBpm *= 0.5;
            loopTotal = half;
        }
        return loopBpm;
    }
    // a region that is a whole number of beats long at (nearly) this tempo: the length gives the exact tempo
    const int beatCount = juce::jmax (1, (int) std::lround (refined * seconds / 60.0));
    const double fitted = 60.0 * beatCount / seconds;
    if (std::abs (fitted - refined) / refined < 0.005 && fitted >= 55.0 && fitted <= 210.0)
        return fitted;
    return refined;
}

// The hits of a piece of audio (kicks, snares, hats...): the sample positions where the sound jumps, for cutting a loop into its hits.
// `sensitivity` 0..1: higher finds more (quieter) hits. At most `maxCount` are returned, the strongest ones, sorted; the first position is
// always `startSample`.
inline std::vector<int> detectOnsets (const juce::AudioBuffer<float>& audio, double sampleRate, int startSample, int endSample,
                                      float sensitivity, int maxCount = 64)
{
    constexpr int hop = 128;
    startSample = juce::jlimit (0, audio.getNumSamples(), startSample);
    endSample = juce::jlimit (startSample, audio.getNumSamples(), endSample);
    const int frames = (endSample - startSample) / hop;
    std::vector<int> result { startSample };
    if (frames < 16 || sampleRate <= 0.0)
        return result;

    const int channels = juce::jmax (1, audio.getNumChannels());
    const double a1 = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 150.0 / sampleRate);
    const double a2 = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 1500.0 / sampleRate);
    std::vector<float> env[3];
    for (auto& e : env)
        e.assign ((size_t) frames, 0.0f);
    double lp1 = 0.0, lp2 = 0.0;
    for (int f = 0; f < frames; ++f)
    {
        double sum[3] = { 0.0, 0.0, 0.0 };
        for (int i = 0; i < hop; ++i)
        {
            double x = 0.0;
            for (int ch = 0; ch < channels; ++ch)
                x += audio.getSample (juce::jmin (ch, audio.getNumChannels() - 1), startSample + f * hop + i);
            x /= channels;
            lp1 += a1 * (x - lp1);
            lp2 += a2 * (x - lp2);
            sum[0] += lp1 * lp1;
            sum[1] += (lp2 - lp1) * (lp2 - lp1);
            sum[2] += (x - lp2) * (x - lp2);
        }
        for (int b = 0; b < 3; ++b)
            env[b][(size_t) f] = (float) std::log1p (300.0 * std::sqrt (sum[b] / hop));
    }

    std::vector<float> flux ((size_t) frames, 0.0f);
    double mean = 0.0;
    for (int f = 1; f < frames; ++f)
    {
        for (int b = 0; b < 3; ++b)
            flux[(size_t) f] += juce::jmax (0.0f, env[b][(size_t) f] - env[b][(size_t) f - 1]);
        mean += flux[(size_t) f];
    }
    mean /= frames;
    double var = 0.0;
    for (auto v : flux)
        var += (v - mean) * (v - mean);
    const double sd = std::sqrt (var / frames);
    const double k = 3.2 - 2.6 * juce::jlimit (0.0f, 1.0f, sensitivity);   // sensitive: a hit only has to stand out a little
    const double threshold = mean + k * sd;

    const int around = juce::jmax (2, (int) (0.045 * sampleRate / hop));   // a hit is the loudest jump within +-45 ms
    std::vector<std::pair<float, int>> peaks;                              // (strength, frame)
    for (int f = 1; f < frames - 1; ++f)
    {
        if (flux[(size_t) f] < threshold)
            continue;
        bool top = true;
        for (int g = juce::jmax (1, f - around); g <= juce::jmin (frames - 1, f + around) && top; ++g)
            if (flux[(size_t) g] > flux[(size_t) f] || (flux[(size_t) g] == flux[(size_t) f] && g < f))
                top = false;
        if (top)
            peaks.push_back ({ flux[(size_t) f], f });
    }

    if ((int) peaks.size() > maxCount - 1)   // the strongest ones
    {
        std::sort (peaks.begin(), peaks.end(), [] (auto& a, auto& b) { return a.first > b.first; });
        peaks.resize ((size_t) maxCount - 1);
    }
    std::sort (peaks.begin(), peaks.end(), [] (auto& a, auto& b) { return a.second < b.second; });

    const int minGap = (int) (0.03 * sampleRate);
    for (auto& p : peaks)
    {
        // a hair before the jump, so the attack of the hit is inside its slice
        const int pos = startSample + juce::jmax (0, p.second - 2) * hop - (int) (0.002 * sampleRate);
        if (pos - result.back() >= minGap)
            result.push_back (juce::jmax (startSample, pos));
    }
    return result;
}
