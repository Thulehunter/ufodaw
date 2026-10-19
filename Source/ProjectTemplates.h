#pragma once

#include <juce_core/juce_core.h>
#include <vector>

// Ready-made projects that are made from the starter pack: a beat to start from, a lo-fi sketch, a house and a trap groove, a song sketch with
// named sections, and a vocal recording setup. They are written to the starter pack folder as ordinary .mdaw files and opened as new, unsaved
// projects (File > New from a template), so the originals are never overwritten.
namespace ProjectTemplates
{
    struct Info
    {
        juce::String name;
        juce::String description;
        juce::File file;
    };

    void ensureWritten();               // (needs the starter pack: call StarterPack::ensureInstalled() first)
    std::vector<Info> list();
}
