-- an audio file as a clip on a new audio track, and a sound of the starter pack
local track = ufodaw.add_track("audio", "From a script")
local path = ufodaw.starter_sound("Kicks", "Kick Punch")
local id = ufodaw.add_audio_clip(track, path, 2)
local clips = ufodaw.clips(track)
assert(#clips == 1 and clips[1].id == id, "the clip is on the track")
assert(clips[1].bar > 1.99 and clips[1].bar < 2.01, "it starts at bar 2")
print("audio clip " .. id .. " ok")