; Inno Setup script (https://jrsoftware.org/isinfo.php) - builds a normal Windows installer.
; 1. Build Release first:  powershell -File scripts\package.ps1
; 2. Compile this file with Inno Setup (ISCC.exe installer\MiniDAW.iss, or open it in the Inno Setup IDE).
; The installer is written to dist\MiniDAW-<version>-setup.exe.

#define AppName "MiniDAW"
#define AppVersion "0.1.0"

[Setup]
SetupIconFile=..Resourcesicon.ico
AppName={#AppName}
AppVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\MiniDAW.exe
OutputDir=..\dist
OutputBaseFilename=MiniDAW-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "..\build\MiniDAW_artefacts\Release\MiniDAW.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\MiniDAW"; Filename: "{app}\MiniDAW.exe"
Name: "{autodesktop}\MiniDAW"; Filename: "{app}\MiniDAW.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\MiniDAW.exe"; Description: "Start MiniDAW"; Flags: nowait postinstall skipifsilent
