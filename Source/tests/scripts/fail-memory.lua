-- A script that uses too much memory is stopped.
local t = {}
for i = 1, 1e9 do t[i] = string.rep("x", 100000) end
