# Scripting UFODAW with Lua

Scripts are small Lua 5.4 programs that look at the project and change it: write a chord progression, spread a euclidean rhythm over a drum track, humanize a clip, set up ten tracks at once, render the project. They are text files in `%APPDATA%\UFODAW\Scripts`.

**Running a script:** *File > Scripts (Lua)...*, or the command palette (`Ctrl+K`, "run a lua script"). The program asks for the script's settings first (if it has any), runs it, and shows what it printed. The whole run is **one undo step**.

**Safety:** a script runs in a sandbox. It cannot read or write files (except `ufodaw.render`, which writes an audio file you name), start programs or use the network. There is no `io`, `os.execute`, `load`, `require` or `debug`. A script that runs longer than 20 seconds or uses more than 128 MB is stopped. Still, only run scripts you trust: a script can delete tracks' contents and change the project, which is why the run is one undo step.

## A first script

```lua
--@param bars 4 How many bars
local track = ufodaw.add_track("midi", "Bass line")
ufodaw.synth_preset(track, "Moog bass")
local clip = ufodaw.add_clip(track, 1, params.bars)
local notes = {}
for bar = 0, params.bars - 1 do
  for _, step in ipairs({ 0, 2.5 }) do
    notes[#notes + 1] = { pitch = music.note("A1"), beat = bar * 4 + step, length = 1.5, velocity = 0.9 }
  end
end
ufodaw.set_notes(track, clip, notes)
print("Wrote " .. #notes .. " notes")
```

Put it in the scripts folder as `Bass line.lua` and run it from the menu. The lines that start with `--@param name default Label` become the settings dialog; the script reads them from the table `params` (numbers arrive as numbers).

## Conventions

- **Tracks** are numbered from 1, in the order of the window. `add_track` returns the number.
- **Bars** count from 1 (bar 1 is the start of the project). A bar may be fractional: `2.5` is the middle of bar 2.
- **Notes** are tables `{ pitch = 60, beat = 0, length = 1, velocity = 0.8 }`: pitch 0 to 127 (60 is middle C), `beat` counts quarter notes from the **start of the clip** (0 is the first beat), `length` in beats, `velocity` 0 to 1.
- **Times** in seconds are used for positions of the transport.
- Errors are Lua errors: they stop the script and are shown with the line number. Use `pcall` to catch them.

## `ufodaw`: the project

| Function | What it does |
|---|---|
| `ufodaw.bpm()` / `ufodaw.set_bpm(x)` | the tempo (of the first section) |
| `ufodaw.position()` / `ufodaw.set_position(seconds)` | the playhead |
| `ufodaw.play()`, `ufodaw.stop()`, `ufodaw.playing()` | the transport |
| `ufodaw.length()` | the length of the project in seconds |
| `ufodaw.bar_to_seconds(bar)`, `ufodaw.seconds_to_bar(s)` | between bars (from 1) and seconds |
| `ufodaw.selected()` | the selected clip as `{ track = 2, clip = 5 }`, or `nil` |
| `ufodaw.render(path)` | renders the whole project to `.wav`, `.flac`, `.aiff`, `.mp3` or `.ogg`; returns `ok, fullpath` (a relative path is in your Documents folder) |
| `print(...)`, `ufodaw.log(...)` | text that is shown when the script ends |

## Tracks

| Function | What it does |
|---|---|
| `ufodaw.tracks()` | a list of `{ index, name, kind, gain, pan, mute, solo }`; `kind` is `"audio"`, `"midi"`, `"drums"`, `"sampler"` or `"bus"` |
| `ufodaw.track_count()` | the number of tracks |
| `ufodaw.add_track(kind, name)` | adds a track (`"audio"`, `"midi"` with the built-in synth, or `"drums"`); returns its number |
| `ufodaw.set_track(i, { name=, gain=, pan=, mute=, solo= })` | changes a track |

## MIDI clips and notes (tracks of kind `"midi"` and `"drums"`)

| Function | What it does |
|---|---|
| `ufodaw.add_clip(track, bar, bars)` | a new empty MIDI clip; returns its id |
| `ufodaw.clips(track)` | a list of `{ id, bar, start, length, notes }` (audio tracks: `file` instead of `notes`) |
| `ufodaw.get_notes(track, clip)` | the notes of a clip |
| `ufodaw.set_notes(track, clip, notes)` | replaces the notes of a clip |

## Drum kits, effects and the arrangement

| Function | What it does |
|---|---|
| `ufodaw.drum_kits()` | the names of the drum kits of the starter pack |
| `ufodaw.drum_kit(track, name)` | fills the pads of a drum track with a kit (`"Boom Bap"`, `"Trap Hard"`, `"World Percussion"`, ...), with its soft and round-robin versions |
| `ufodaw.set_fx(track, { eqOn = true, eqHpFreq = 80, compOn = true, reverbOn = true, reverbMix = 0.3, ... })` | the effects of a track, or of the master with `"master"` instead of a track number. The names are those of the project file: `eq... comp... sat... chorus... delay... reverb... lim... gate... deEss... mb...` (numbers, and `true` / `false` for the `...On` switches); an unknown name is an error |
| `ufodaw.set_sections({ { name = "Intro", bars = 4 }, { name = "Verse", bars = 16 } })` | the named parts of the arrangement, one after the other from bar 1 |

With `UFODAW.exe --run-script script.lua save_as=song.mdaw` the project that a script built is saved to that file. A whole song can be written by one script.

## Audio files (tracks of kind `"audio"`)

| Function | What it does |
|---|---|
| `ufodaw.add_audio_clip(track, path, bar)` | puts an audio file (`.wav`, `.flac`, `.aiff`, `.mp3`, `.ogg`) on an audio track from this bar (1 is the first); returns the clip id. A relative path is in your Documents folder |
| `ufodaw.starter_sound(category, name)` | the path of a sound of the starter pack, for example `ufodaw.starter_sound("Kicks", "Kick Punch")` (the names are those of the sample browser) |

Drum pads listen to the usual notes: kick 36, snare 38, clap 39, closed hat 42, open hat 46, low tom 41, high tom 45, crash 49, and 37, 40, 43, 44, 47, 48, 50, 51 for the other eight pads.

## The synth (tracks of kind `"midi"`)

| Function | What it does |
|---|---|
| `ufodaw.synth_presets()` | the names of the ready-made sounds |
| `ufodaw.synth_preset(track, name)` | sets a sound by name ("Grand piano", "808 sub", "Warm pad", ...) |
| `ufodaw.set_synth(track, { cutoff = 3000, resonance = 0.4, waveform = "saw", unison = 3, engine = "piano", mono = true, ... })` | changes settings; the names are those of the synth window, a number out of range is limited, an unknown name is an error |

Settings: `attack decay sustain release cutoff resonance drive keyTrack filterEnv fAttack fDecay fSustain fRelease osc2Semi osc2Detune osc2Mix subLevel pulseWidth unisonDetune spread lfoRate lfoPitch lfoCutoff lfoAmp lfoDelay glide pitchEnv pitchEnvTime pianoDecay pianoBright pianoDetune pianoHammer level`, and `waveform`, `osc2Wave` (`"sine" "saw" "square" "triangle"`), `lfoShape` (`"sine" "triangle" "square" "random"`), `engine` (`"synth" "piano"`), `mono`, `unison` (1 to 5).

## `music`: a small library

| Function | What it does |
|---|---|
| `music.note("C4")` | 60; also `"Bb3"`, `"F#2"`; a number stays as it is |
| `music.scale(root, name, octaves)` | the notes of a scale: `major minor dorian phrygian lydian mixolydian harmonic_minor pentatonic_major pentatonic_minor blues chromatic` |
| `music.chord(root, quality)` | the notes of a chord: `maj min dim aug sus2 sus4 7 maj7 min7 dim7 min7b5 maj9 min9 9 13 add9` |
| `music.euclid(steps, pulses, rotation)` | a list of `true`/`false`: `pulses` hits spread evenly over `steps` |
| `music.hash(seed, i)` | a number between 0 and 1 that is the same for the same seed and `i` |
| `music.scales`, `music.chords` | the tables behind them: add your own |

The standard `math`, `string`, `table` and `utf8` libraries are there, and `os.time`, `os.clock`, `os.date`.

## The example scripts

The program writes these to the scripts folder the first time; read them, copy them, change them:

- **Hello (tour of the project)** - prints the tracks and clips.
- **Chord progression** - seventh chords from a scale on a new synth track.
- **Euclidean drums** - kick, snare and hat patterns on a new drum track.
- **Random melody** - a wandering melody in a scale.
- **Humanize selected clip** - timing and velocity variation (select a MIDI clip first).
- **Arpeggiate selected clip** - turns the chords of the selected clip into arpeggios.

## Extensions

An extension is a folder with scripts that belong together, in `%APPDATA%\UFODAW\Extensions`. It has an `extension.json`:

```json
{
  "name": "My extension",
  "version": "1.0",
  "author": "You",
  "description": "What it does",
  "scripts": [
    { "title": "Make a beat", "file": "beat.lua", "description": "A house beat on a new drum track" }
  ]
}
```

The scripts show in **File > Scripts**, in a submenu with the name of the extension, and run like every other script (settings with `--@param` lines, one undo step, the same sandbox). A script of an extension can share code with the other `.lua` files of its folder:

```lua
local patterns = include("patterns")   -- runs patterns.lua once and gives back what it returns
```

`include` only reads files from the extension's own folder. **Install an extension (zip)...** in the same menu installs one from a zip file (the zip has `extension.json` at the top or in one folder; only scripts, text, pictures and sounds are allowed, no more than 80 MB; installing it again updates it). The program writes an example extension, *Example - Drum patterns*, the first time: copy that folder to start your own.

## From the command line

```
UFODAW.exe --run-script script.lua [project.mdaw] [name=value ...]
```

runs a script without any dialog, on the project if one is given; `name=value` sets a setting (a setting that is not given takes its default; `max_seconds=5` changes the time limit). What the script prints goes to standard output; the exit code is 0 when the script succeeded and 1 when it failed. Combine it with `UFODAW.exe --render project.mdaw out.wav` to build and render projects in a pipeline. Another program (Python, a shell script) can drive UFODAW this way by starting it with `--run-script`; the program has no open network port for it, on purpose. `UFODAW.exe --install-extension extension.zip` installs an extension without a dialog.

## Not (yet) possible

Scripts can add audio files as clips, but not edit audio (cut, stretch, effects). They cannot run while the transport plays (no realtime scripts yet), and they cannot add windows, panels or buttons.
