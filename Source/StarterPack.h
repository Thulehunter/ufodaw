#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>

// A starter pack of sounds that is made by the program itself (no licensing questions): drum one-shots in the usual categories, bass and synth
// sounds, effects and a few loops. It is written to %APPDATA%\UFODAW\StarterPack the first time it is needed, and is the source of the drum kits,
// the project templates and the sample browser. Everything in it is synthesised, so it is free to use in any music.
namespace StarterPack
{
    juce::File folder();                    // %APPDATA%\UFODAW\StarterPack
    bool isReady();                         // written completely (for this version of the pack)
    void ensureInstalled();                 // writes it when it is missing; takes about a second, so call it from a background thread at start-up
    juce::File sample (const juce::String& category, const juce::String& name);   // e.g. ("Kicks", "Kick Punch")

    struct Kit
    {
        juce::String name;
        juce::String description;
        std::vector<std::pair<juce::String, juce::String>> pads;   // (category, name) for pad 1..16; empty = leave the pad as it is
    };
    const std::vector<Kit>& kits();

    // The variations of a drum (see the "Variations" folder): a soft version for light hits, and round-robin versions that take turns with it.
    struct DrumVariants
    {
        juce::File soft;                          // empty when there is none
        std::vector<juce::File> roundRobin;       // "<name> b", "<name> c"
    };
    DrumVariants variantsOf (const juce::String& category, const juce::String& name);

    // The tempo (BPM) a loop was made at; 0 for a sound that is not a loop
    int loopTempo (const juce::String& name);
}
