# Runs the Lua scripts of tests\scripts and the example scripts of the program, each in the program (no dialogs), on a small project.
# Files whose name starts with "fail-" must fail (exit code 1); the others must succeed.
#   powershell -ExecutionPolicy Bypass -File scripts\scripttest.ps1 [-Exe path\to\UFODAW.exe]
param([string]$Exe)
$root = Split-Path $PSScriptRoot -Parent
if (-not $Exe) { $Exe = "$root\build\UFODAW_artefacts\Release\UFODAW.exe" }
$project = "$root\tests\projects\legacy-v1-no-sections.mdaw"
$scripts = @(Get-ChildItem "$root\tests\scripts\*.lua" | ForEach-Object FullName)
$exampleFolder = "$env:APPDATA\UFODAW\Scripts"
foreach ($name in 'Hello (tour of the project).lua', 'Chord progression.lua', 'Euclidean drums.lua', 'Random melody.lua') {
    if (Test-Path "$exampleFolder\$name") { $scripts += "$exampleFolder\$name" }
}
# the scripts of the example extension (they use include)
foreach ($s in @(Get-ChildItem "$env:APPDATA\UFODAW\Extensions\Example - Drum patterns\*.lua" -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'patterns.lua' })) { $scripts += $s.FullName }
$failed = 0
foreach ($s in $scripts) {
    $expectFail = (Split-Path $s -Leaf).StartsWith('fail-')
    $out = "$env:TEMP\ufodaw-script-out.txt"
    Remove-Item $out -ErrorAction SilentlyContinue
    $p = Start-Process $Exe -ArgumentList @('--run-script', ('"' + $s + '"'), ('"' + $project + '"'), 'max_seconds=2') -PassThru -RedirectStandardOutput $out -WindowStyle Hidden
    $null = $p.Handle   # (keeps the exit code available)
    if (-not $p.WaitForExit(60000)) { $p.Kill(); $code = 99 } else { $p.WaitForExit(); $code = $p.ExitCode }
    $text = Get-Content $out -Raw -ErrorAction SilentlyContinue
    $text = if ($text) { ($text.Trim() -split "`n")[0].Trim() } else { '' }
    $ok = if ($expectFail) { $code -eq 1 } else { $code -eq 0 }
    if (-not $ok) { $failed++ }
    '{0} {1}: exit {2} {3}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), (Split-Path $s -Leaf), $code, $text
}
if ($failed) { "$failed FAILED"; exit 1 } else { 'ALL PASSED'; exit 0 }
