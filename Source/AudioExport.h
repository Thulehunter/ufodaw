#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

// The audio file formats a mix or a stem can be exported to: WAV, FLAC, AIFF, Ogg Vorbis (JUCE's own writers) and MP3 (the Shine
// encoder, fixed-point, LGPL). One list of choices is shared by "Export WAV..." and "Export stems...".
namespace AudioExport
{
    enum class Format { Wav, Flac, Aiff, Ogg, Mp3 };

    struct Choice
    {
        const char* label;
        Format format;
        int bits;       // WAV / FLAC / AIFF: 16, 24 or 32 (32 = floating point, WAV only)
        int quality;    // MP3: kbps; Ogg: kbps (the nearest of the encoder's steps)
    };

    const std::vector<Choice>& choices();                 // in the order the dialogs show them
    juce::String extensionOf (Format);                     // "wav", "flac", "aiff", "ogg", "mp3"
    bool isLossy (Format f);

    // A writer for this choice; it takes over `stream` (released on success). Null if the format cannot do this rate / channel count.
    std::unique_ptr<juce::AudioFormatWriter> createWriter (std::unique_ptr<juce::FileOutputStream>& stream, const Choice&,
                                                           double sampleRate, int numChannels);
}
