# MiniDAW

A small digital audio workstation for Windows, written in C++17 with [JUCE](https://juce.com).

## Features

- **Audio tracks** - import wav / aiff / flac / mp3 / ogg (drag files into the window), record from your audio input.
- **Arrangement** - clips can be moved (also between tracks), split, duplicated and deleted; zoom, snap to the grid, loop region.
- **MIDI tracks** with a built-in polyphonic synth (sine / saw / square / triangle, ADSR, low-pass) and a **piano roll**.
- **Drum tracks** - 8-pad drum kit with generated sounds, a **step sequencer**, and your own samples per pad (right-click a pad).
- **MIDI input** - any connected keyboard plays the selected MIDI track and can be recorded; the computer keyboard works as a piano too ("Keys").
- **Effects per track** - 3-band EQ, compressor, delay, reverb.
- **VST3 hosting** - instruments and effects, with their own editor windows; plugin states are saved in the project.
- **Automation** - breakpoint envelopes for volume, pan, every built-in effect parameter, the synth, and any parameter of a hosted VST3 plugin.
- **Velocity editing** - velocity bars under the piano roll; Ctrl+drag a step in the drum sequencer.
- **Mixer** - volume, pan, mute, solo, level meters, soft limiter on the master.
- **Undo / redo**, project **save / load** (`.mdaw`) and **WAV export** (24-bit).

## Running

Download `MiniDAW-<version>-win64.zip` (see *Packaging*), unzip it and start `MiniDAW.exe`. No installation is needed.

## Keyboard and mouse

| Action | How |
|---|---|
| Play / pause | `Space` |
| Rewind | `Enter` |
| Record (audio, or MIDI when a MIDI track is armed) | `R` |
| Loop on / off | `L` (`Shift+L` loops the selected clip) |
| Set the loop region | right-drag on the ruler |
| Move the playhead | click or drag on the ruler / empty lane |
| Split clip(s) at the playhead | `S` |
| Delete clip / note | `Delete` |
| Duplicate clip | `Ctrl+D` |
| Undo / redo | `Ctrl+Z` / `Ctrl+Y` |
| Save / open | `Ctrl+S` / `Ctrl+O` |
| Zoom | `Ctrl` + mouse wheel, or the `-` / `+` buttons |
| Ignore snapping while dragging | hold `Alt` |
| Create a MIDI clip | double-click an empty spot on a MIDI track |
| Add / remove a note or drum step | click / right-click in the piano roll or step sequencer |
| Change note velocity | drag the bars under the piano roll (sideways = paint several); `Ctrl`+drag a drum step |
| Automate a parameter | **A** button on the track, pick the parameter, then click in the lane to add points (right-click a point removes it) |
| Play with the computer keyboard | turn on **Keys**, then `A W S E D F T G Y H U J K O L P` (`Z` / `X` change octave) |

Buttons on every track: **FX** (built-in effects), **VST** (VST3 instrument and inserts), **A** (volume / pan automation).
Use **Plugins...** at the bottom right to scan for VST3 plugins first.

## Building from source

Requirements: Windows 10/11, Visual Studio with the *Desktop development with C++* workload, CMake 3.22+ (the copy in Visual Studio works), git.

```
cmake -S . -B build
cmake --build build --config Release
build\MiniDAW_artefacts\Release\MiniDAW.exe
```

The first configure downloads JUCE 8.0.4 from GitHub. Use `Release` for real use: the Debug build is far too slow for audio and will crackle.

## Packaging

```
powershell -ExecutionPolicy Bypass -File scripts\package.ps1
```

builds Release and writes `dist\MiniDAW-<version>-win64.zip`. For a regular installer, install [Inno Setup](https://jrsoftware.org/isinfo.php) and compile `installer\MiniDAW.iss` (after running the script above).

## Project layout

| File | What it does |
|---|---|
| `Source/Engine.*` | Real-time mixer: tracks, clips, transport, loop, automation, recording, offline render |
| `Source/Synth.*`, `Source/DrumKit.*` | Built-in instruments |
| `Source/Fx.*` | EQ, compressor, delay, reverb |
| `Source/PluginHost.*`, `Source/Plugin.h` | VST3 scanning, loading, editor windows |
| `Source/MainComponent.*` | Main window: arrangement, transport, project files, undo/redo |
| `Source/PianoRoll.*`, `Source/DrumSequencer.*` | Note editors below the arrangement |
| `Source/FxPanel.*`, `Source/PluginPanel.*` | Per-track effect and plugin panels |

The audio thread never blocks: the UI edits the project under a lock that the audio callback only *tries* to take. Undo works with cheap snapshots of the whole project (sample data is shared, not copied).

## Known limitations

- No latency compensation for plugins (lookahead limiters etc. run slightly late).
- Changes made inside a plugin's own window are not part of undo.
- Automation is applied once per audio block (steps, not a smooth ramp) except for volume and pan; while a parameter is automated its slider can still be moved but the envelope wins during playback.
- VST3 only (no VST2, CLAP or AU).
- Windows only so far.

## License

MiniDAW uses JUCE, which is available under the AGPLv3 or a commercial license. If you distribute MiniDAW binaries, you must comply with one of them (for AGPLv3 that means offering the source under the same terms).
