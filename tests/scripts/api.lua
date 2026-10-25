-- The API of the project: tracks, clips, notes, synth, tempo; and the music library.
local before = ufodaw.track_count()
local t = ufodaw.add_track("midi", "Test synth")
assert(ufodaw.track_count() == before + 1, "a track was added")
local clip = ufodaw.add_clip(t, 1, 2)
assert(ufodaw.set_notes(t, clip, { { pitch = 60, beat = 0, length = 1, velocity = 0.9 }, { pitch = 64, beat = 1, length = 1 } }))
local notes = ufodaw.get_notes(t, clip)
assert(#notes == 2 and notes[1].pitch == 60 and math.abs(notes[2].beat - 1) < 1e-6 and math.abs(notes[1].velocity - 0.9) < 1e-3, "the notes come back")
assert(#ufodaw.clips(t) == 1 and ufodaw.clips(t)[1].notes == 2)

ufodaw.synth_preset(t, "Grand piano")
ufodaw.set_synth(t, { cutoff = 5000, waveform = "square", unison = 3 })
assert(not pcall(ufodaw.set_synth, t, { nonsense = 1 }), "an unknown setting is an error")
assert(not pcall(ufodaw.synth_preset, t, "No such sound"))
assert(#ufodaw.synth_presets() >= 60)
-- the wavetable engine: by name or number
ufodaw.set_synth(t, { engine = "wavetable", wavetable = "fold", wtPos = 0.4, wtLfo = 0.5, wtEnv = -0.3, wtVel = 0.6 })
ufodaw.set_synth(t, { wtTable = "Choir" })
ufodaw.set_synth(t, { wavetable = 13 })
assert(not pcall(ufodaw.set_synth, t, { wavetable = "no such table" }), "an unknown wavetable is an error")
ufodaw.synth_preset(t, "WT Choir pad")

local d = ufodaw.add_track("drums", "Test drums")
assert(ufodaw.tracks()[d].kind == "drums")
assert(not pcall(ufodaw.add_track, "banana"), "an unknown kind of track is an error")
assert(not pcall(ufodaw.set_notes, t, clip, { { pitch = 200, beat = 0 } }), "a pitch above 127 is an error")
ufodaw.set_track(t, { name = "Renamed", gain = 0.5, mute = true })
assert(ufodaw.tracks()[t].name == "Renamed" and ufodaw.tracks()[t].mute == true)

ufodaw.set_bpm(100)
assert(math.abs(ufodaw.bpm() - 100) < 1e-6, "the tempo changed")
assert(math.abs(ufodaw.bar_to_seconds(2) - 2.4) < 1e-6, "a bar at 100 BPM lasts 2.4 s")

-- the music library
assert(music.note("C4") == 60 and music.note("Bb3") == 58 and music.note("F#2") == 42)
local chord = music.chord("D3", "min9")
assert(#chord == 5 and chord[1] == 50 and chord[4] == 60)
local scale = music.scale("A3", "minor", 1)
assert(#scale == 8 and scale[1] == 57 and scale[8] == 69)
local count = 0
for _, on in ipairs(music.euclid(16, 5)) do if on then count = count + 1 end end
assert(count == 5, "euclid makes the number of hits asked for")
assert(music.hash(1, 2) == music.hash(1, 2))
print("api ok")
