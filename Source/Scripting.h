#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <vector>

class MainComponent;

// Scripts for UFODAW, written in Lua 5.4. A script is a text file in %APPDATA%\UFODAW\Scripts; it can look at the project and change it (tracks, MIDI
// clips and notes, the synth, the tempo, the transport, rendering) through the `ufodaw` table, and it has a small music library (`music`: note names,
// scales, chords, euclidean rhythms). Scripts run in a sandbox: no files, no network, no programs; only the project. A script that runs for too long or
// uses too much memory is stopped. The whole run is one undo step.
//
// Parameters: lines at the top of a script like
//     --@param bars 8 How many bars
// make the program ask for the values first; the script reads them from the table `params` (numbers when they look like numbers).
namespace Scripting
{
    struct Param { juce::String key, defaultValue, label; };
    std::vector<Param> readParams (const juce::String& source);

    struct Result
    {
        bool ok = true;
        juce::String error;      // what went wrong, with the line number
        juce::String output;     // what the script printed
    };

    // Runs a script on the message thread. `chunkName` is shown in error messages (the file name).
    Result run (MainComponent&, const juce::String& source, const juce::String& chunkName, const std::map<juce::String, juce::String>& params,
                double maxSeconds = 20.0);

    juce::File scriptsFolder();                        // %APPDATA%\UFODAW\Scripts
    juce::Array<juce::File> listScripts();             // the .lua files, sorted by name
    void ensureExamples();                             // writes the example scripts the first time
}
