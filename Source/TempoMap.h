#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// The tempo and time signature of the project: a list of sections, each starting at a bar and lasting until the next one.
// A beat is a quarter note and the tempo is in quarter notes per minute, whatever the time signature (6/8 has 3 beats of
// a quarter per bar; its bar is 3 quarters long). The timeline itself is in seconds: this map turns seconds into beats and
// bars (for the grid, the metronome and plugins) and back.
struct TempoSection
{
    int bar = 0;               // first bar of the section (0-based; the screen shows bar + 1)
    double bpm = 120.0;
    int numerator = 4;         // beats per bar, counted in units of 1/denominator
    int denominator = 4;
    std::string name;          // what the section is called (verse, chorus...); may be empty

    double beatsPerBar() const { return numerator * 4.0 / denominator; }   // in quarter notes
    double beatUnit() const { return 4.0 / denominator; }                  // length of one counted beat in quarter notes
};

class TempoMap
{
public:
    TempoMap() { sections.push_back ({}); normalise(); }

    const std::vector<TempoSection>& getSections() const { return sections; }

    // Replaces the sections (sorted, first one at bar 0, values clamped) and recalculates the positions.
    void setSections (std::vector<TempoSection> s)
    {
        sections = std::move (s);
        normalise();
    }

    // ---- positions
    double beatsToSeconds (double beat) const
    {
        const size_t i = sectionAtBeat (beat);
        return startSeconds[i] + (beat - startBeat[i]) * 60.0 / sections[i].bpm;
    }

    double secondsToBeats (double seconds) const
    {
        const size_t i = sectionAtSeconds (seconds);
        return startBeat[i] + (seconds - startSeconds[i]) * sections[i].bpm / 60.0;
    }

    double barStartBeat (int bar) const
    {
        const size_t i = sectionAtBar (bar);
        return startBeat[i] + (double) (bar - sections[i].bar) * sections[i].beatsPerBar();
    }

    double barStartSeconds (int bar) const { return beatsToSeconds (barStartBeat (bar)); }

    // The bar a beat position lies in; `intoBar` (optional) gets how many quarter notes into that bar it is.
    int barAtBeat (double beat, double* intoBar = nullptr) const
    {
        const size_t i = sectionAtBeat (beat);
        const double perBar = sections[i].beatsPerBar();
        const double rel = beat - startBeat[i];
        const int k = (int) std::floor (rel / perBar + 1.0e-9);
        if (intoBar != nullptr)
            *intoBar = rel - (double) k * perBar;
        return sections[i].bar + k;
    }

    int barAtSeconds (double seconds, double* intoBar = nullptr) const { return barAtBeat (secondsToBeats (seconds), intoBar); }

    double barBeatToSeconds (int bar, double quartersIntoBar) const { return beatsToSeconds (barStartBeat (bar) + quartersIntoBar); }

    // ---- what is in force at a position
    const TempoSection& sectionAt (double seconds) const { return sections[sectionAtSeconds (seconds)]; }
    double bpmAt (double seconds) const { return sectionAt (seconds).bpm; }
    double beatSecondsAt (double seconds) const { return 60.0 / bpmAt (seconds); }   // one quarter note
    double barSecondsAt (double seconds) const
    {
        const auto& s = sectionAt (seconds);
        return s.beatsPerBar() * 60.0 / s.bpm;
    }

    // Moves a time from one map to another so that it keeps its place in the music: same bar, same distance into the bar.
    static double retime (const TempoMap& from, const TempoMap& to, double seconds)
    {
        double into = 0.0;
        const int bar = from.barAtSeconds (std::max (0.0, seconds), &into);
        return to.barBeatToSeconds (bar, into);
    }

    // The grid: calls `line (seconds, kind, bar)` for every bar line (kind 2), beat line (1) and, when `subdivisions` > 1,
    // subdivision line (0) between `fromSeconds` and `toSeconds`.
    template <typename F>
    void forEachGridLine (double fromSeconds, double toSeconds, int subdivisions, F&& line) const
    {
        int bar = barAtSeconds (std::max (0.0, fromSeconds));
        for (;; ++bar)
        {
            const double barStart = barStartBeat (bar);
            const double start = beatsToSeconds (barStart);
            if (start > toSeconds)
                break;

            const auto& s = sections[sectionAtBar (bar)];
            const double unit = s.beatUnit();
            line (start, 2, bar);
            for (int b = 0; b < s.numerator; ++b)
                for (int sub = (b == 0 ? 1 : 0); sub < std::max (1, subdivisions); ++sub)
                {
                    const double beat = barStart + ((double) b + (double) sub / std::max (1, subdivisions)) * unit;
                    const double sec = beatsToSeconds (beat);
                    if (sec <= toSeconds)
                        line (sec, sub == 0 ? 1 : 0, bar);
                }
        }
    }

private:
    // sorted by bar, first at bar 0; the derived positions are kept in step by normalise()
    std::vector<TempoSection> sections;
    std::vector<double> startBeat, startSeconds;

    void normalise()
    {
        if (sections.empty())
            sections.push_back ({});
        for (auto& s : sections)
        {
            s.bpm = std::clamp (s.bpm, 20.0, 400.0);
            s.numerator = std::clamp (s.numerator, 1, 32);
            if (s.denominator != 2 && s.denominator != 4 && s.denominator != 8 && s.denominator != 16)
                s.denominator = 4;
            s.bar = std::max (0, s.bar);
        }
        std::stable_sort (sections.begin(), sections.end(), [] (auto& a, auto& b) { return a.bar < b.bar; });
        sections.front().bar = 0;
        for (size_t i = sections.size() - 1; i > 0; --i)   // two sections cannot start at the same bar: the later one wins
            if (sections[i].bar == sections[i - 1].bar)
                sections.erase (sections.begin() + (long) i - 1);

        startBeat.assign (sections.size(), 0.0);
        startSeconds.assign (sections.size(), 0.0);
        for (size_t i = 1; i < sections.size(); ++i)
        {
            startBeat[i] = startBeat[i - 1] + (double) (sections[i].bar - sections[i - 1].bar) * sections[i - 1].beatsPerBar();
            startSeconds[i] = startSeconds[i - 1] + (startBeat[i] - startBeat[i - 1]) * 60.0 / sections[i - 1].bpm;
        }
    }

    size_t sectionAtBar (int bar) const
    {
        size_t i = 0;
        while (i + 1 < sections.size() && sections[i + 1].bar <= bar)
            ++i;
        return i;
    }

    size_t sectionAtBeat (double beat) const
    {
        size_t i = 0;
        while (i + 1 < sections.size() && startBeat[i + 1] <= beat + 1.0e-9)
            ++i;
        return i;
    }

    size_t sectionAtSeconds (double seconds) const
    {
        size_t i = 0;
        while (i + 1 < sections.size() && startSeconds[i + 1] <= seconds + 1.0e-9)
            ++i;
        return i;
    }
};
