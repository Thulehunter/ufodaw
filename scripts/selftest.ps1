# Opens every test project (and the program's own starter-pack projects) in UFODAW without dialogs, renders a few seconds of each and checks the
# result. Used by the automatic tests (.github/workflows/tests.yml); run it by hand after a build:
#   powershell -ExecutionPolicy Bypass -File scripts\selftest.ps1 [-Exe path\to\UFODAW.exe]
# Files whose name starts with "warn-" must open WITH a warning (a missing file, a newer format); all others must open cleanly and not be silent.
param([string]$Exe)
$root = Split-Path $PSScriptRoot -Parent
if (-not $Exe) { $Exe = "$root\build\UFODAW_artefacts\Release\UFODAW.exe" }
$result = Join-Path ([IO.Path]::GetTempPath()) 'ufodaw-selftest-result.txt'
Remove-Item $result -ErrorAction SilentlyContinue
$projects = @(Get-ChildItem "$root\tests\projects\*.mdaw" | ForEach-Object FullName)
$starter = "$env:APPDATA\UFODAW\StarterPack\Projects"
if (Test-Path $starter) { $projects += Get-ChildItem "$starter\*.mdaw" | ForEach-Object FullName }
$quoted = $projects | ForEach-Object { '"' + $_ + '"' }
$p = Start-Process $Exe -ArgumentList (@('--selftest', ('"' + $result + '"')) + $quoted) -PassThru
if (-not $p.WaitForExit(120000)) { $p.Kill(); Write-Host 'TIMEOUT: the program did not finish'; exit 2 }
if (-not (Test-Path $result)) { Write-Host 'No result file was written'; exit 2 }
Get-Content $result
exit $p.ExitCode
