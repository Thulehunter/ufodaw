# Builds MiniDAW in Release and packs it into dist\MiniDAW-<version>-win64.zip.
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File scripts\package.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

# CMake from PATH, or the copy that ships with Visual Studio
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $cmake) { throw 'cmake not found. Install CMake or Visual Studio with the C++ workload.' }

$version = (Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(MiniDAW VERSION ([0-9.]+)').Matches[0].Groups[1].Value

& $cmake -S $root -B "$root\build"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& $cmake --build "$root\build" --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

$exe = "$root\build\MiniDAW_artefacts\Release\MiniDAW.exe"
$stage = "$root\dist\MiniDAW-$version"
$zip = "$root\dist\MiniDAW-$version-win64.zip"

Remove-Item $stage, $zip -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe $stage
Copy-Item "$root\README.md" $stage
Compress-Archive -Path "$stage\*" -DestinationPath $zip

Write-Host "Created $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
