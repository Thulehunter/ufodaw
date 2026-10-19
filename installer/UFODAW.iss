; Inno Setup script (https://jrsoftware.org/isinfo.php) - builds a normal Windows installer.
; 1. Build Release first:  powershell -File scripts\package.ps1
; 2. Compile this file with Inno Setup (ISCC.exe installer\UFODAW.iss, or open it in the Inno Setup IDE).
; The installer is written to dist\UFODAW-<version>-setup.exe.

#define AppName "UFODAW"
#define AppVersion "0.1.0"

[Setup]
SetupIconFile=..\Resources\icon.ico
AppName={#AppName}
AppVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\UFODAW.exe
OutputDir=..\dist
OutputBaseFilename=UFODAW-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern
ChangesAssociations=yes

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "..\build\UFODAW_artefacts\Release\UFODAW.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\UFODAW"; Filename: "{app}\UFODAW.exe"
Name: "{autodesktop}\UFODAW"; Filename: "{app}\UFODAW.exe"; Tasks: desktopicon

[Registry]
; .mdaw project files: the UFODAW icon, and double-click opens them in UFODAW
Root: HKA; Subkey: "Software\Classes\.mdaw"; ValueType: string; ValueName: ""; ValueData: "UFODAW.Project"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\UFODAW.Project"; ValueType: string; ValueName: ""; ValueData: "UFODAW project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\UFODAW.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\UFODAW.exe"",0"
Root: HKA; Subkey: "Software\Classes\UFODAW.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\UFODAW.exe"" ""%1"""

[Run]
Filename: "{app}\UFODAW.exe"; Description: "Start UFODAW"; Flags: nowait postinstall skipifsilent
