# MiniDAW

A small digital audio workstation for Windows, written in C++17 with [JUCE](https://juce.com).

## Features

- **Audio tracks** - import wav / aiff / flac / mp3 / ogg (drag files into the window), record from your audio input.
  Every audio track has its own **input** (the button under the track name: one mono input or a stereo pair of your interface),
  several armed tracks record at once, each from its own input, and **Mon** lets you hear the input live through the track's
  effects, volume and pan.
- **Tuner** - **Tuner** (bottom right) opens a chromatic tuner for any input of your interface: note, cents sharp/flat, a needle and
  the frequency (A = 430-450 Hz). It starts on the input of the armed track and works down to a low bass B.
- **Loop recording with takes** - record with **Loop** on: every pass becomes a take. The newest plays; right-click the clip to
  *Choose take*. Split (`S`) cuts all takes at the playhead, so each part can use a different take (comping); *Keep this take*
  flattens the result.
- **Punch in/out** - **Punch** records only inside a region: it starts and stops by itself, the rest of the take stays untouched.
- **Arrangement** - clips can be moved (also between tracks), split, duplicated and deleted; zoom, snap to the grid, loop region.
- **MIDI tracks** with a built-in polyphonic synth (sine / saw / square / triangle, ADSR, low-pass) and a **piano roll**
  that names every key and note, lights up the keys that sound (live or during playback) and scrolls to the clip's notes
  ("Show notes" brings them back).
- **Drum tracks** - 8-pad drum kit with generated sounds, a **step sequencer**, and your own samples per pad (right-click a pad).
- **MIDI input** - any connected keyboard plays the selected MIDI track and can be recorded; the computer keyboard works as a piano too ("Keys").
- **Effects per track** - 3-band EQ, compressor, delay, reverb.
- **VST3 hosting** - instruments and effects, with their own editor windows; plugin states are saved in the project.
- **MIDI routing** - a sequencer plugin on one track can play the synth (built-in, drum kit or VST) of one or more other tracks; plugins receive tempo, bar position and play state from the DAW.
- **External hardware** - the MIDI notes of a track or sequencer plugin can play analog/hardware synths through any MIDI output, with channel selection.
- **MIDI effects** - arpeggiators, chord tools and other MIDI plugins can sit in front of a track's instrument.
- **Plugin delay compensation** - tracks are delayed to line up with plugins that report latency (up to half a second).
- **Automation** - breakpoint envelopes for volume, pan, every built-in effect parameter, the synth, and any parameter of a hosted VST3 plugin.
- **Velocity editing** - velocity bars under the piano roll; Ctrl+drag a step in the drum sequencer.
- **Mixer** - volume, pan, mute, solo and level meters per track, plus a **master channel** (the yellow strip under the tracks):
  volume with automation, the built-in effects (EQ, compressor, delay, reverb), VST3 effect plugins on the whole mix, a
  level meter, and the soft limiter after it. Stems skip the master channel.
- **Undo / redo**, project **save / load** (`.mdaw`) and **WAV export** (24-bit).
- **Stem export** - every track as its own WAV file in a folder you choose (see below).

## Running

Download `MiniDAW-<version>-win64.zip` (see *Packaging*), unzip it and start `MiniDAW.exe`. No installation is needed.
`MiniDAW.exe project.mdaw` (or "Open with" on a `.mdaw` file) opens that project right away.

The interface uses a warm dark theme with red, orange and yellow accents; the colours live in `Source/Theme.h`. Track colours tell the types apart: orange = audio, red = MIDI synth, yellow = drums.

## Exporting stems

**Export stems** (top bar) writes one WAV file per track into a folder you pick, named `01 - Bass loop.wav`,
`02 - Synth 1.wav`, ... in track order. A progress window with a Cancel button shows while it renders, and when it is done
you can open the folder.

- Each stem is the track as you hear it: after its effects and plugins, with its volume, pan and automation.
- Muted tracks, empty tracks and sequencer tracks (which are silent themselves; the synth that plays their notes
  gets them) are skipped; the summary says which ones.
- Format: 24-bit, 16-bit or **32-bit float** (cannot clip, even when a stem is above 0 dBFS).
- **Without master volume and limiter** (the default) the stems add up exactly to the mix; with it, every stem gets the master
  volume and the soft limiter on its own.
- Every stem is as long as the whole project (plus the tail of reverbs/delays), so they all line up when you drop them in another DAW.

## Audio interfaces and ASIO

Click **Audio...** (bottom right) to choose the driver type, the device, the sample rate, the buffer size and which
channels are on. The choice is remembered. The status line at the bottom shows what is in use. MiniDAW plays through the
first two enabled output channels. Which input an audio track records from is chosen per track (see below); only inputs
that are switched on in this window can be picked.

Without extra steps you get *Windows Audio* (WASAPI), *Windows Audio (Exclusive Mode)* / *(Low Latency Mode)* and
*DirectSound*. For **ASIO** drivers (PreSonus Quantum / Studio 192, Roland Rubix, AudioBox, ...) MiniDAW has to be built with
Steinberg's ASIO SDK, which cannot be shipped with the sources:

1. Download the ASIO SDK from <https://www.steinberg.net/asiosdk> (you accept Steinberg's license there).
2. Unzip it into `third_party\` so that `third_party\asiosdk_<version>\common\iasiodrv.h` exists
   (or point at it: `cmake -S . -B build -DASIO_SDK_DIR=C:\path\to\asiosdk`, or set the `ASIO_SDK_DIR` environment variable).
3. Build again (`cmake -S . -B build`, then `cmake --build build --config Release`). The configure step prints
   `ASIO support: ON`, and *ASIO* appears as a driver type in the Audio window.

Once the SDK is in `third_party\`, every build you make there has ASIO on (it is detected on each configure, Debug and
Release alike), and `scripts\package.ps1` refuses to make a package without it (use `-NoAsio` to build one on purpose).

Notes: Windows lets a program use one ASIO driver at a time. To use two interfaces together, use *Windows Audio*
(input and output can be different devices there) or an aggregate driver from the interface maker. Distributing a build
that includes ASIO means following Steinberg's ASIO license (the SDK is also offered under the GPLv3).

## Playing an external (analog) synth

Any MIDI track, and any sequencer track (Stochas...), can play **hardware**: open the track's **VST** panel and tick the MIDI output
under *External MIDI instruments (hardware)*, for example your interface's "... MIDI" output, or a USB MIDI device.

1. Connect the interface's (or USB) MIDI OUT to the synth's MIDI IN, and set the synth to the channel you send on.
2. In the panel pick the output. **From channel** chooses which of the sequencer's channels goes there (All, or one channel -
   e.g. one Stochas track per synth); **Send as channel** is the channel the hardware gets (1 by default, or "Keep channel").
3. Press play. The notes leave at the right moment, shifted by the audio device's output latency so the synth stays in time with
   the rest. When playback stops, "all notes off" is sent so nothing hangs.
4. A track that sends to hardware makes no sound itself (and gives no stem). To hear the synth in the DAW, record its audio:
   connect it to an interface input and record it on an audio track. Until you do, you can listen to it through the interface's
   direct monitoring.

You can combine outputs: a Stochas sequencer track can drive internal synths and several hardware synths at once, each with its own
channel. Exports never play the hardware.

## Sequencer plugins (Stochas etc.) driving a synthesizer

1. Add two MIDI tracks (**+ MIDI**): one for the sequencer, one for the synth.
2. On the synth track, open **VST** and pick your synthesizer as its instrument.
3. On the sequencer track, open **VST**, set **Use as** to *Sequencer*, and pick the sequencer plugin (e.g. Stochas) in the list below it.
4. Still in that panel, tick the track(s) that should play its notes. Each ticked track has a **channel** setting:
   - *All channels* passes the notes on unchanged;
   - *Channel N* sends only the notes the plugin plays on MIDI channel N, delivered to the synth as channel 1. This is how one multi-track sequencer (such as Stochas, whose tracks each have their own channel) drives several different synths: tick one synth per channel.
5. Press play. The plugin gets the DAW's tempo and play position, and the notes it makes are played by the target tracks. The sequencer track itself is silent.

An arpeggiator or similar plugin that should change the notes of one track goes under **MIDI effects** in that track's VST panel instead.

## Keyboard and mouse

| Action | How |
|---|---|
| Play / pause | `Space` |
| Rewind | `Enter` |
| Record (audio, or MIDI when a MIDI track is armed) | `R` |
| Loop on / off | `L` (`Shift+L` loops the selected clip) |
| Set the loop region | right-drag on the ruler |
| Add an empty audio track | **+ Track** > *Empty audio track* (it starts armed for recording); drop audio files on it later, several files go one after the other |
| Record into a specific audio track | click the red dot left of the track name to arm it, then **Record** (`R`). Arm several tracks to record them all at once. Without an armed track the recording becomes a new track from inputs 1+2 |
| Punch in/out | turn on **Punch** (`P`); the region is the selected clip, or two bars at the playhead. Change it with **Shift + right-drag** on the ruler (red). Press Record: playback starts a bar before the region, records inside it, and the recording closes itself at the punch-out |
| Record several takes | turn on **Loop** (set the loop with right-drag on the ruler) and press Record; stop when you have enough. Pick takes with right-click > *Choose take*; `S` splits all takes of a clip at the playhead for comping |
| Tune an instrument | **Tuner** at the bottom right; pick the input (defaults to the armed track's input) |
| Choose a track's input | click the input button on the audio track (e.g. *In: 1+2*): mono inputs, stereo pairs, or *Audio settings...* |
| Hear the input while playing | arm the track and turn on **Mon**; it goes through the track's effects, volume and pan (not in exports). Use headphones, or direct monitoring on your interface, to avoid feedback and double latency |
| Move the playhead | click or drag on the ruler / empty lane |
| Split clip(s) at the playhead | `S` |
| Delete clip / note | `Delete` |
| Duplicate clip | `Ctrl+D` |
| Cut / copy / paste clips | right-click a clip (or an empty spot in a lane) for *Cut, Copy, Paste here, Split here, Duplicate, Delete*; or `Ctrl+X` / `Ctrl+C` / `Ctrl+V` (paste goes to the playhead, on the selected track of the same kind). Works for audio and MIDI clips, also between tracks |
| Undo / redo | `Ctrl+Z` / `Ctrl+Y` |
| Save / open | `Ctrl+S` / `Ctrl+O` |
| Zoom | `Ctrl` + mouse wheel, or the `-` / `+` buttons |
| Ignore snapping while dragging | hold `Alt` |
| Create a MIDI clip | double-click an empty spot on a MIDI track |
| Add / remove a note or drum step | click / right-click in the piano roll or step sequencer |
| Open a VST plugin's own window | picking a plugin opens it; later: double-click the track name, double-click a MIDI clip, or **Plugin window** in the piano roll |
| Change note velocity | drag the bars under the piano roll (sideways = paint several); `Ctrl`+drag a drum step |
| Pan a track | pan slider on the track; with **Pan snap 5%** on it jumps in 5% steps (L 5%, L 10%, ... R 100%), off = free. Hold `Alt` while dragging for the other mode. Double-click = center. Pan envelopes snap the same way |
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
| `Source/Tuner.*` | Pitch detection (YIN) and the tuner window |
| `Source/FxPanel.*`, `Source/PluginPanel.*` | Per-track effect and plugin panels |

The audio thread never blocks: the UI edits the project under a lock that the audio callback only *tries* to take. Undo works with cheap snapshots of the whole project (sample data is shared, not copied).

## Known limitations

- Delay compensation covers audio plugins (instrument and effect inserts) up to 0.5 s; plugins that change their latency while playing may cause a short glitch.
- Changes made inside a plugin's own window are not part of undo.
- Automation of effect and plugin parameters is applied every 64 samples (volume and pan are ramped); synth parameters are applied once per audio block. While a parameter is automated its slider can still be moved but the envelope wins during playback.
- VST3 only (no VST2, CLAP or AU).
- Windows only so far.

## License

MiniDAW uses JUCE, which is available under the AGPLv3 or a commercial license. If you distribute MiniDAW binaries, you must comply with one of them (for AGPLv3 that means offering the source under the same terms).
