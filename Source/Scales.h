#pragma once

#include <vector>

// Scales for the piano roll: which pitches belong to a key (a root note and a scale), the nearest pitch that does, and chords built from it.
namespace Scales
{
    struct Def
    {
        const char* name;
        std::vector<int> steps;   // semitones above the root
    };

    // index 0 = no scale
    inline const std::vector<Def>& defs()
    {
        static const std::vector<Def> list = {
            { "No scale", {} },
            { "Major", { 0, 2, 4, 5, 7, 9, 11 } },
            { "Natural minor", { 0, 2, 3, 5, 7, 8, 10 } },
            { "Dorian", { 0, 2, 3, 5, 7, 9, 10 } },
            { "Phrygian", { 0, 1, 3, 5, 7, 8, 10 } },
            { "Lydian", { 0, 2, 4, 6, 7, 9, 11 } },
            { "Mixolydian", { 0, 2, 4, 5, 7, 9, 10 } },
            { "Locrian", { 0, 1, 3, 5, 6, 8, 10 } },
            { "Harmonic minor", { 0, 2, 3, 5, 7, 8, 11 } },
            { "Melodic minor", { 0, 2, 3, 5, 7, 9, 11 } },
            { "Major pentatonic", { 0, 2, 4, 7, 9 } },
            { "Minor pentatonic", { 0, 3, 5, 7, 10 } },
            { "Blues", { 0, 3, 5, 6, 7, 10 } },
            { "Whole tone", { 0, 2, 4, 6, 8, 10 } },
        };
        return list;
    }

    inline bool contains (int pitch, int root, int type)
    {
        if (type <= 0 || type >= (int) defs().size())
            return true;
        const int rel = ((pitch - root) % 12 + 12) % 12;
        for (int s : defs()[(size_t) type].steps)
            if (s == rel)
                return true;
        return false;
    }

    inline bool isRoot (int pitch, int root) { return ((pitch - root) % 12 + 12) % 12 == 0; }

    // The pitch itself when it is in the scale, else the nearest one that is (the lower one when two are equally near)
    inline int nearest (int pitch, int root, int type)
    {
        if (contains (pitch, root, type))
            return pitch;
        for (int d = 1; d <= 6; ++d)
        {
            if (pitch - d >= 0 && contains (pitch - d, root, type)) return pitch - d;
            if (pitch + d <= 127 && contains (pitch + d, root, type)) return pitch + d;
        }
        return pitch;
    }

    // `degrees` steps up (or down) the scale from the pitch
    inline int step (int pitch, int root, int type, int degrees)
    {
        int p = nearest (pitch, root, type);
        const int dir = degrees < 0 ? -1 : 1;
        for (int n = degrees < 0 ? -degrees : degrees; n > 0; --n)
        {
            do { p += dir; } while (p > 0 && p < 127 && ! contains (p, root, type));
        }
        return p < 0 ? 0 : p > 127 ? 127 : p;
    }

    // The notes of a chord on `pitch`: chordType 1 = triad, 2 = seventh. Inside a scale the chord is stacked in thirds from the scale's own
    // notes (so it is the right kind of chord for that degree); without a scale it is a major chord.
    inline std::vector<int> chord (int pitch, int root, int type, int chordType)
    {
        std::vector<int> notes { pitch };
        if (chordType <= 0)
            return notes;
        if (type <= 0)
        {
            notes.push_back (pitch + 4);
            notes.push_back (pitch + 7);
            if (chordType >= 2)
                notes.push_back (pitch + 11);
        }
        else
        {
            notes.push_back (step (pitch, root, type, 2));
            notes.push_back (step (pitch, root, type, 4));
            if (chordType >= 2)
                notes.push_back (step (pitch, root, type, 6));
        }
        for (auto& n : notes)
            n = n < 0 ? 0 : n > 127 ? 127 : n;
        return notes;
    }
}
