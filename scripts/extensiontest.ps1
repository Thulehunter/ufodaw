# Installs three test extensions through "UFODAW.exe --install-extension" (a good one, one with an executable, one without a manifest) and runs a script of the good one.
$exe = Join-Path (Split-Path $PSScriptRoot -Parent) 'build\UFODAW_artefacts\Release\UFODAW.exe'
$tmp="$env:TEMP\ext-test"; Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory "$tmp\good\My Ext" -Force | Out-Null
'{"name":"Test Extension","version":"0.1","scripts":[{"title":"Hello","file":"hello.lua"}]}' | Set-Content "$tmp\good\My Ext\extension.json"
'print("hi from the extension")' | Set-Content "$tmp\good\My Ext\hello.lua"
Compress-Archive -Path "$tmp\good\My Ext" -DestinationPath "$tmp\good.zip"
# a bad one: an executable inside, and a manifest-less one
New-Item -ItemType Directory "$tmp\bad" -Force | Out-Null
'{"name":"Bad","scripts":[]}' | Set-Content "$tmp\bad\extension.json"; 'x' | Set-Content "$tmp\bad\evil.exe"
Compress-Archive -Path "$tmp\bad\*" -DestinationPath "$tmp\bad.zip"
New-Item -ItemType Directory "$tmp\none" -Force | Out-Null; 'x' | Set-Content "$tmp\none\readme.txt"; Compress-Archive -Path "$tmp\none\*" -DestinationPath "$tmp\none.zip"
foreach($z in 'good','bad','none'){ $o="$tmp\$z.out"; $p=Start-Process $exe -ArgumentList @('--install-extension',('"'+"$tmp\$z.zip"+'"')) -PassThru -RedirectStandardOutput $o -WindowStyle Hidden; $null=$p.Handle; $p.WaitForExit(30000)|Out-Null; "{0}: exit {1} {2}" -f $z,$p.ExitCode,((Get-Content $o -Raw).Trim()) }
"installed folder: " + (Test-Path "$env:APPDATA\UFODAW\Extensions\Test Extension\hello.lua")
$p=Start-Process $exe -ArgumentList @('--run-script',('"'+"$env:APPDATA\UFODAW\Extensions\Test Extension\hello.lua"+'"')) -PassThru -RedirectStandardOutput "$tmp\run.out" -WindowStyle Hidden; $null=$p.Handle; $p.WaitForExit(30000)|Out-Null; "run: exit $($p.ExitCode) " + (Get-Content "$tmp\run.out" -Raw).Trim()
Remove-Item "$env:APPDATA\UFODAW\Extensions\Test Extension" -Recurse -Force -ErrorAction SilentlyContinue
