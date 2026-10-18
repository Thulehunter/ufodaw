# UFODAW

A small digital audio workstation for Windows, written in C++17 with [JUCE](https://juce.com).

## Features

- **Audio tracks** - import wav / aiff / flac / mp3 / ogg (drag files into the window), record from your audio input.
  Every audio track has its own **input** (the button under the track name: one mono input or a stereo pair of your interface),
  several armed tracks record at once, each from its own input, and **Mon** lets you hear the input live through the track's
  effects, volume and pan.
- **Tuner** - **Tuner** (gauge icon in the left rail) opens a chromatic tuner for any input of your interface: note, cents sharp/flat, a needle and
  the frequency (A = 430-450 Hz). It starts on the input of the armed track and works down to a low bass B.
- **Collect, versions and backups** - **Save As** (button) opens a menu: *Save a new version* (jam, jam v2, jam v3...), and *Save with all
  audio in one folder*, which copies every audio file the project uses into `<folder>Audio` and opens the project from there, so the
  whole folder can be moved to another disk or PC. Every normal save also leaves a time-stamped copy in `<project> backups` (the newest
  twenty are kept; *Show the backups folder* in the same menu).
- **Autosave and recovery** - every two minutes, when something changed, the project is written to a recovery file. If the program
  crashes (a plugin, say) or is closed with unsaved changes, the next start offers to restore it; saving normally removes it.
- **Metronome and count-in** - the **Click** button in the transport bar (or `M`) switches the metronome on and off, also while playing
  or recording. **Count-in** (top bar) sets 0-4 bars of clicks before a recording starts; they follow the tempo and time signature at
  the playhead, play even when the metronome is off, and nothing is recorded during them (the display counts the beats down).
- **Loop recording with takes** - record with **Loop** on: every pass becomes a take. The newest plays; right-click the clip to
  *Choose take*. Split (`S`) cuts all takes at the playhead, so each part can use a different take (comping); *Keep this take*
  flattens the result.
- **Punch in/out** - **Punch** records only inside a region: it starts and stops by itself, the rest of the take stays untouched.
- **Arrangement** - clips can be moved (also between tracks), split, duplicated and deleted; zoom, snap to the grid, loop region.
- **MIDI tracks** with a built-in polyphonic synth (sine / saw / square / triangle, ADSR, low-pass) and a **piano roll**
  that names every key and note, lights up the keys that sound (live or during playback) and scrolls to the clip's notes
  ("Show notes" brings them back).
- **Sampler** (Simpler-style) - **+ (left rail) > Sampler** makes a MIDI track that plays one sample: load a file (or drop one on the track), drag the
  yellow start/end markers in its window, and pick the mode: *Classic* (pitched by the key from the root note, with loop, ADSR and a low-pass
  filter), *One-shot* (plays out whatever the key does) or *Slice* (the region cut into equal slices, one key each). *Detect pitch* sets the root
  note and tuning from the sample itself. Right-click an audio clip > *Sample this clip* turns it into a sampler track with one note.
  Double-click the track name (or *Sampler window* in the piano roll) to open it again.
- **Stretchable patterns** - drag the right edge of a MIDI or drum clip: longer repeats the pattern to fill the clip, shorter cuts it off. Hold **Shift** to stretch the pattern instead (half as long = twice as fast, longer = slower); hold **Alt** to switch snapping off.
- **Undo for the sampler and the drum rack** - sample loads, markers, sliders, pad level / pan / mute / solo / choke and stretches are undo steps. Stretching a sample runs in the background behind a progress window with a Cancel button.
- **Comping loop recordings** - right-click a take > *Comp takes*: every take of the recording gets a row; swipe across a row to play that take there (the takes are split at the swipe, with short fades at the joins). Right-click to leave. Undo works on each swipe.
- **Follow** - the *Follow* button in the top bar makes the arrangement turn a page when the playhead reaches the right edge while playing.
- **Takes as blocks** - in the comp view every take is a block with handles: drag an edge to trim it, Alt+drag the body to move it, right-click to duplicate or delete (all undoable).
- **Comp view** - for an envelope with takes, A > *Comp view* shows the result on top and every take in a row of its own. Swipe across a take's row to play that take there; the result updates at once. Recorded rides can be undone, and hovering the right edge of a MIDI / drum clip shows what drag, Shift and Alt do.
- **Project tabs** - several projects open at once, as tabs along the top (New project / Ctrl+N and + open a new tab, Open puts a file in a tab of its own, Ctrl+W closes, Ctrl+Tab / Ctrl+Shift+Tab switch). Only the shown tab uses the audio device and MIDI inputs; the others stop playing when you switch away. Unsaved tabs have a yellow dot and are asked about when closed; each keeps its own recovery file.
- **Automation rides, takes and comping** - **A** > pick an envelope (volume, pan, effect, synth/sampler or plugin parameter) > *Record rides* > Touch / Latch / Write. Press play and move the fader or knob: every pass is kept as a take, comped into the envelope with a short crossfade. In the lane, Shift+drag picks a time range and right-click gives *Play here* per take (comping), *Delete take* and *Fade between takes*.
- **Icon rail** - the strip on the left has File, Add track (+), Plugins, Tuner and Audio settings; tracks are rounded cards; the transport floats as a pill at the bottom.
- **File menu and STOP ALL** - the *File* button has New project (Ctrl+N, a new tab), Open, Open recent, Save, Save as / new version / collect, and the exports. The red *STOP ALL* button at the top right (Ctrl+Shift+Space) stops playback and recording and cuts off every sound at once, plugins and MIDI hardware included.
  The **Stretch** slider time-stretches the sample (0.25x-4x) without changing its pitch (Rubber Band); it works on drum pads too.
- **Drum Rack** - 16 pads, each a small Simpler (start / end, pitch, envelope, filter, reverse) with its own level, pan, mute, solo and choke group (closed / open hat). Click the track's "Drum kit" button or double-click a pad name in the step sequencer to open the 4x4 pad window; drop an audio file on a pad to load it. The first 8 pads come with generated sounds; a **step sequencer** plays all 16.
- **MIDI input per track** - a MIDI track listens to every MIDI input, or to one device and channel (VST panel > *MIDI input*), so a
  keyboard can play the synth and an e-drum the drum track at the same time. Arm several MIDI tracks with their red dot and they
  record together, each from its own input (also together with armed audio tracks). The computer keyboard ("Keys") plays the selected track.
- **Controllers** - mod wheel, expression, sustain and other pedals, hi-hat pedal, pitch bend and aftertouch are recorded with the
  notes, played back to plugin instruments and hardware, and edited in the lane under the piano roll (pick the controller in the lane box:
  click adds a point, drag moves it, right-click deletes). The built-in synth ignores them.
- **Effects per track** - 3-band EQ, compressor, delay, reverb, and a **Mono** button on every track, bus and the master (lit = mono): the channel
  plays with left and right summed - to check a source or the whole mix in mono, or to centre a stereo recording.
- **VST3 hosting** - instruments and effects, with their own editor windows; plugin states are saved in the project. Picking a plugin
  (the instrument, or *+ Add effect plugin...*) opens a **search box**: type part of a name, a maker or a category ("kon", "reverb",
  "native") and the list narrows; Up / Down move through it, Enter picks.
- **MIDI routing** - a sequencer plugin on one track can play the synth (built-in, drum kit or VST) of one or more other tracks; plugins receive tempo, bar position and play state from the DAW.
- **External hardware** - the MIDI notes of a track or sequencer plugin can play analog/hardware synths through any MIDI output, with channel selection.
- **MIDI effects** - arpeggiators, chord tools and other MIDI plugins can sit in front of a track's instrument.
- **Plugin delay compensation** - tracks are delayed to line up with plugins that report latency (up to half a second).
- **Automation** - breakpoint envelopes for volume, pan, every built-in effect parameter, the synth, and any parameter of a hosted VST3 plugin.
- **Velocity editing** - velocity bars under the piano roll; Ctrl+drag a step in the drum sequencer.
- **CPU readout** - the status line shows the audio device load (*CPU n %*), and a track name turns yellow (25 %+) or orange (50 %+) of the audio
  time when its plugins and effects are heavy; hover the header for the exact figure. Denormal numbers are flushed to zero in the mix, and the
  window only repaints what changes, so UFODAW itself stays light.
- **Clipping indicators** - a red **CLIP** lamp in the top bar, a lamp at the top of every level meter, a red frame round the strip and a red
  track name show when something reached full scale: a track's output, the master mix (over 0 dB before the limiter), or the input of an
  armed audio track (then lower the gain on the interface; a recording that clips cannot be repaired). They stay lit until you click
  them, and are cleared when playback starts. Hover a lamp to see where. Armed audio tracks show their input level on their meter.
- **Mixer** - volume, pan, mute, solo and level meters per track, plus a **master channel** (the yellow strip under the tracks):
  volume with automation, the built-in effects (EQ, compressor, delay, reverb), VST3 effect plugins on the whole mix, a
  level meter, and the soft limiter after it. Stems skip the master channel.
- **Clip gain, volume curve, time-stretch and pitch-shift** - right-click an audio clip: *Gain, stretch and pitch...* sets the clip gain
  (with *Normalize*), the length as a percentage (200% = twice as long; *Fit to project tempo* from the clip's own BPM) and the pitch in
  semitones, made offline with Rubber Band from the original (a progress window, Cancel; *Reset* puts the original back; results are
  cached on disk). *Edit volume curve* draws a multiplier over the clip (click adds a point, drag moves, right-click removes, Esc is
  done); splitting a clip splits the curve too.
- **Clip fades, crossfades and reverse** - drag the little squares in the top corners of an audio clip to set its fade-in and
  fade-out (equal-power curves). Drag a clip so it overlaps its neighbour a little, then right-click > *Crossfade with overlapping
  clip(s)*. *Reverse* plays the clip backwards (also after splitting it).
- **Tempo and time signature changes** - **Tempo** (next to BPM) opens the list of sections: each starts at a bar and has its own BPM
  and time signature (3/4, 6/8, 7/8, ...). The ruler, grid, snapping, metronome (one click per counted beat, the first of the bar
  higher) and the tempo/position given to plugins all follow it. Changing tempo or meter keeps MIDI, the loop and the punch region
  in place *in the music* (same bar, same beat), so MIDI follows the new tempo; audio clips stay where they are in time. The BPM
  slider is the tempo of the first section.
- **Buses and sends** - **+ (left rail) > Bus** makes a group / effects-return channel. Put a reverb or delay on it (FX or VST), then use
  the **Out** button on any track: play the track *into* the bus (a drum bus, say) and/or *send* part of it to buses with a level per
  bus (post-fader, or *Pre*). Muting a track mutes its sends; soloing a track keeps its bus audible. Buses are not exported as stems.
- **MIDI quantize, swing, humanize** - right-click a MIDI clip (or press `Q`): grid from 1/4 to 1/32 and triplets, strength, swing
  (100% = triplet feel), random timing (ms) and velocity variation, optionally the note lengths too. Works on drum clips as well.
- **Undo / redo**, project **save / load** (`.mdaw`) and **WAV export** (24-bit).
- **Stem export** - every track as its own WAV file in a folder you choose (see below).

## Running

Download `UFODAW-<version>-win64.zip` from the [Releases](../../releases) page (or build it yourself, see *Packaging*), unzip it and start `UFODAW.exe`. No installation is needed.
`UFODAW.exe project.mdaw` (or "Open with" on a `.mdaw` file) opens that project right away.

The interface uses a warm dark theme with red, orange and yellow accents; the colours live in `Source/Theme.h`. Track colours tell the types apart: orange = audio, red = MIDI synth, yellow = drums.

## Exporting stems

**Export stems** (top bar) writes one WAV file per track into a folder you pick, named `01 - Bass loop.wav`,
`02 - Synth 1.wav`, ... in track order. A progress window with a Cancel button shows while it renders, and when it is done
you can open the folder.

- Each stem is the track as you hear it: after its effects and plugins, with its volume, pan and automation.
- Stems are dry: they ignore buses and sends, so bus effects (a shared reverb) are not in them. Use **Export WAV** for the full mix.
- Muted tracks, empty tracks and sequencer tracks (which are silent themselves; the synth that plays their notes
  gets them) are skipped; the summary says which ones.
- Format: 24-bit, 16-bit or **32-bit float** (cannot clip, even when a stem is above 0 dBFS).
- **Without master volume and limiter** (the default) the stems add up exactly to the mix; with it, every stem gets the master
  volume and the soft limiter on its own.
- Every stem is as long as the whole project (plus the tail of reverbs/delays), so they all line up when you drop them in another DAW.

## Audio interfaces and ASIO

Click the **Audio** icon (sliders, bottom of the left rail) to choose the driver type, the device, the sample rate, the buffer size and which
channels are on. The choice is remembered. The status line at the bottom shows what is in use. UFODAW plays through the
first two enabled output channels. Which input an audio track records from is chosen per track (see below); only inputs
that are switched on in this window can be picked.

Without extra steps you get *Windows Audio* (WASAPI), *Windows Audio (Exclusive Mode)* / *(Low Latency Mode)* and
*DirectSound*. For **ASIO** drivers (PreSonus Quantum / Studio 192, Roland Rubix, AudioBox, ...) UFODAW has to be built with
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

## Recording latency and MIDI clock

**Audio...** has three tabs. *Audio device* is the usual driver / device / buffer choice.

**Recording latency.** Sound takes time to go out to your headphones and back in through the interface, so a recording arrives
late. UFODAW places a recording on the timeline *earlier* by that round trip, so it lines up with what you played along to.
- *Automatic* uses what the driver reports (input + output latency). *Measured* uses the **Measure** button: it plays a short
  click through the first two outputs and listens for it on the input you pick. Connect an output of your interface to that input with
  a cable (or point a microphone at the speakers), turn the monitors down, and press Measure; the result is kept. *None* switches it off.
- *Extra offset* fine-tunes in milliseconds (positive = earlier). Record a click or a clap and check it lands on the grid; adjust if not.
- This also applies to loop takes and punch recordings; at the very start of a project the part that would lie before 0 is trimmed.

**MIDI clock.** Tick the MIDI output(s) of your drum machine, sequencer or synth under *MIDI clock*: while the transport runs they get
24 clock ticks per quarter note (following the tempo map, so tempo changes carry over), a Start (or Continue with the song position
when you start somewhere else), a new position when you jump (a loop, a click on the ruler), and a Stop when playback ends.
*Clock shift* moves the clock a few milliseconds earlier or later to line the hardware up with the audio. Exports never send clock.
Notes to hardware are set up separately, in a track's VST panel (*External MIDI instruments*).

## Sequencer plugins (Stochas etc.) driving a synthesizer

1. Add two MIDI tracks (**+** in the left rail > MIDI track): one for the sequencer, one for the synth.
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
| Set the loop region | right-drag on the ruler or in any track (a right-click without dragging opens the clip menu) |
| Add an empty audio track | **+** (left rail) > *Audio track* (it starts armed for recording); drop audio files on it later, several files go one after the other |
| Record into a specific audio track | click the red dot left of the track name to arm it, then **Record** (`R`). Arm several tracks to record them all at once. Without an armed track the recording becomes a new track from inputs 1+2 |
| Clip gain / stretch / pitch | right-click an audio clip > *Gain, stretch and pitch...* |
| Volume curve of a clip | right-click the clip > *Edit volume curve* (click = add, drag = move, right-click = remove, `Esc` = done) |
| New version / collect audio / backups | **Save As** button: its menu |
| Sampler | **+ (left rail) > Sampler**, then Load... (or drop an audio file on the track); right-click an audio clip > *Sample this clip* |
| Find a plugin | VST panel > the Instrument button or *+ Add effect plugin...*: type to search by name, maker or category; Up / Down, Enter |
| Fade in / out of an audio clip | drag the squares in the top corners of the clip; right-click > *Remove fades* |
| Crossfade two clips | let the clips overlap a little, right-click one > *Crossfade with overlapping clip(s)* |
| Reverse a clip | right-click > *Reverse* |
| Tempo / time signature changes | **Tempo** button next to BPM: add sections that start at a bar, each with a BPM and a time signature |
| Mono or stereo | the **Mono** button on the track, bus or master strip (lit = mono; also in the FX panel) |
| Send a track to a bus / choose its output | **Out** button (bottom right of the track header, e.g. *→ Master*): pick the output and set a send level per bus |
| Quantize / swing / humanize a MIDI clip | select the clip and press `Q`, or right-click it |
| Metronome on / off | **Click** next to Punch, or `M` (works during playback and recording) |
| Count-in before recording | **Count-in** in the top bar: off, 1, 2, 3 or 4 bars; applies when you press Record from a stop |
| Latency compensation | **Audio** icon > *Recording latency*: automatic, measured with a cable from an output to an input, plus a manual offset |
| MIDI clock to hardware | **Audio** icon > *MIDI clock*: tick the outputs that should follow the tempo |
| Punch in/out | turn on **Punch** (`P`); the region is the selected clip, or two bars at the playhead. Change it with **Shift + right-drag** on the ruler (red). Press Record: playback starts a bar before the region, records inside it, and the recording closes itself at the punch-out |
| Record a keyboard and an e-drum together | arm both MIDI tracks with their red dot, set each track's *MIDI input* in its VST panel, press Record |
| Edit a mod wheel / pedal / pitch bend | piano roll: the box in the lane under the notes (Velocity by default) lists the controllers |
| Record several takes | turn on **Loop** (set the loop with right-drag on the ruler) and press Record; stop when you have enough. Pick takes with right-click > *Choose take*; `S` splits all takes of a clip at the playhead for comping |
| Tune an instrument | **Tuner** icon in the left rail; pick the input (defaults to the armed track's input) |
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
Use the **Plugins** icon in the left rail to scan for VST3 plugins first.

## Building from source

Requirements: Windows 10/11, Visual Studio with the *Desktop development with C++* workload, CMake 3.22+ (the copy in Visual Studio works), git.

```
cmake -S . -B build
cmake --build build --config Release
build\UFODAW_artefacts\Release\UFODAW.exe
```

The first configure downloads JUCE 8.0.4 from GitHub. Use `Release` for real use: the Debug build is far too slow for audio and will crackle.

## Packaging

```
powershell -ExecutionPolicy Bypass -File scripts\package.ps1
```

builds Release and writes `dist\UFODAW-<version>-win64.zip`. For a regular installer, install [Inno Setup](https://jrsoftware.org/isinfo.php) and compile `installer\UFODAW.iss` (after running the script above).

## Project layout

| File | What it does |
|---|---|
| `Source/Engine.*` | Real-time mixer: tracks, clips, transport, loop, automation, recording, offline render |
| `Source/Synth.*`, `Source/DrumKit.*` | Built-in instruments |
| `Source/Fx.*` | EQ, compressor, delay, reverb |
| `Source/PluginHost.*`, `Source/Plugin.h` | VST3 scanning, loading, editor windows |
| `Source/MainComponent.*` | Main window: arrangement, transport, project files, undo/redo |
| `Source/PianoRoll.*`, `Source/DrumSequencer.*` | Note editors below the arrangement |
| `Source/Sampler.*`, `Source/SamplerPanel.h` | The sampler instrument and its window |
| `Source/Stretch.*`, `Source/ClipPanel.h` | Time-stretch and pitch-shift (Rubber Band), and the clip gain / stretch / pitch panel |
| `Source/Tuner.*` | Pitch detection (YIN) and the tuner window |
| `Source/TempoMap.h`, `Source/TempoPanel.h` | Tempo and time signature sections: seconds / beats / bars, and the list to edit them |
| `Source/SyncPanels.h` | The latency and MIDI clock tabs of the audio settings |
| `Source/RoutingPanel.h`, `Source/MidiTools.h` | Output and sends of a track; quantize / swing / humanize |
| `Source/FxPanel.*`, `Source/PluginPanel.*` | Per-track effect and plugin panels |

The audio thread never blocks: the UI edits the project under a lock that the audio callback only *tries* to take. Undo works with cheap snapshots of the whole project (sample data is shared, not copied).

## Known limitations

- Delay compensation covers audio plugins (instrument and effect inserts) up to 0.5 s; plugins that change their latency while playing may cause a short glitch.
- Changes made inside a plugin's own window are not part of undo.
- Automation of effect and plugin parameters is applied every 64 samples (volume and pan are ramped); synth parameters are applied once per audio block. While a parameter is automated its slider can still be moved but the envelope wins during playback.
- VST3 only (no VST2, CLAP or AU).
- Windows only so far.

## License

UFODAW is free software, released under the **GNU Affero General Public License v3.0** (see `LICENSE`). If you distribute a modified
version, or let people use it over a network, you must offer them the source under the same terms.

It is built on:

- [JUCE](https://juce.com) 8 (AGPLv3, or a commercial licence from its makers), which also provides VST3 hosting under the GPLv3 terms of Steinberg's VST3 SDK.
- The [Rubber Band Library](https://breakfastquay.com/rubberband/) (Particular Programs Ltd., GPL v2 or later) for time-stretching and pitch-shifting. A closed-source build would need a commercial licence from them as well.
- Optionally Steinberg's ASIO SDK (not included; see *Audio interfaces and ASIO*). Builds that include ASIO follow Steinberg's licence for it (the SDK is also offered under the GPLv3).

VST is a registered trademark of Steinberg Media Technologies GmbH. UFODAW is not affiliated with or endorsed by Steinberg, the JUCE team or Particular Programs.

## Code signing policy

Free code signing provided by [SignPath.io](https://signpath.io), certificate by [SignPath Foundation](https://signpath.org).
*(Active once the project has been accepted by SignPath Foundation; until then releases are not signed and Windows SmartScreen may warn.)*

- **What is signed:** `UFODAW.exe` of the official releases, built from this repository by the GitHub Actions workflow
  `.github/workflows/release.yml` (so the signed program can be checked against the source). Builds you make yourself are not signed.
- **Roles:** the committer, reviewer and approver of this project is its owner, [@Thulehunter](https://github.com/Thulehunter).
- **Privacy:** UFODAW does not send any information to other networked systems unless you ask for it. (Plugins you load may do
  what they like; they are not part of UFODAW.)
