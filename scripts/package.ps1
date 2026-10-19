# Builds UFODAW in Release and packs it into dist\UFODAW-<version>-win64.zip, and (when Inno Setup is installed) into the installer
# dist\UFODAW-<version>-setup.exe.
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File scripts\package.ps1
#
# The build must include ASIO (the SDK goes in third_party\, see the README). Without it the script stops, so a
# package is never made without ASIO by accident. Use -NoAsio to build one without it on purpose.
param([switch]$NoAsio)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

# CMake from PATH, or the copy that ships with Visual Studio
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $cmake) { throw 'cmake not found. Install CMake or Visual Studio with the C++ workload.' }

$version = (Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(UFODAW VERSION ([0-9.]+)').Matches[0].Groups[1].Value

# configure, and read back whether CMake found the ASIO SDK
$configureLog = & $cmake -S $root -B "$root\build" 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { $configureLog; throw 'CMake configure failed' }
$asioLine = $configureLog | Where-Object { $_ -match 'ASIO support' } | Select-Object -First 1
Write-Host ($asioLine -replace '^-- ', '')
if (-not $NoAsio -and $asioLine -notmatch 'ASIO support: ON') {
    throw "ASIO is off in this build. Put the Steinberg ASIO SDK in third_party\ (see README, 'Audio interfaces and ASIO'), or run with -NoAsio."
}

# (the file version of the exe comes from a resource file that is only written when it is missing: write it again from the project version)
Remove-Item "$root\build\UFODAW_artefacts\JuceLibraryCode\UFODAW_resources.rc" -ErrorAction SilentlyContinue
& $cmake --build "$root\build" --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

$exe = "$root\build\UFODAW_artefacts\Release\UFODAW.exe"
$stage = "$root\dist\UFODAW-$version"
$zip = "$root\dist\UFODAW-$version-win64.zip"

Remove-Item $stage, $zip -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe $stage
Copy-Item "$root\README.md" $stage
Copy-Item "$root\LICENSE" $stage
Compress-Archive -Path "$stage\*" -DestinationPath $zip

Write-Host "Created $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"

# the installer (needs Inno Setup: winget install JRSoftware.InnoSetup)
$iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
if (-not $iscc) {
    $iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if ($iscc) {
    & $iscc "/DAppVersion=$version" "$root\installer\UFODAW.iss" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'The installer could not be built' }
    $setup = "$root\dist\UFODAW-$version-setup.exe"
    Write-Host "Created $setup ($([math]::Round((Get-Item $setup).Length / 1MB, 1)) MB)"
} else {
    Write-Host 'Inno Setup not found: no installer made (winget install JRSoftware.InnoSetup).'
}