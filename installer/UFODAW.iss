; Inno Setup script (https://jrsoftware.org/isinfo.php) - builds a normal Windows installer.
; Easiest: powershell -File scripts\package.ps1   (builds Release, the zip and this installer)
; By hand:  ISCC.exe /DAppVersion=0.8.8.2 installer\UFODAW.iss   (after a Release build)
; The installer is written to dist\UFODAW-<version>-setup.exe.

#define AppName "UFODAW"
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{8D2F6B4A-3C1E-4A57-9B0D-5E7A1C2F4D60}
SetupIconFile=..\Resources\icon.ico
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=UFODAW
AppPublisherURL=https://ufodaw.eu
AppSupportURL=https://github.com/Thulehunter/ufodaw/issues
AppUpdatesURL=https://github.com/Thulehunter/ufodaw/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\UFODAW.exe
UninstallDisplayName={#AppName}
OutputDir=..\dist
OutputBaseFilename=UFODAW-{#AppVersion}-setup
LicenseFile=..\LICENSE
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern
ChangesAssociations=yes
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "associate"; Description: "Open &.mdaw project files with UFODAW (and show the UFODAW icon on them)"; GroupDescription: "File type:"

[Files]
Source: "..\build\UFODAW_artefacts\Release\UFODAW.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\UFODAW"; Filename: "{app}\UFODAW.exe"
Name: "{group}\Uninstall UFODAW"; Filename: "{uninstallexe}"
Name: "{autodesktop}\UFODAW"; Filename: "{app}\UFODAW.exe"; Tasks: desktopicon

[Registry]
; .mdaw project files: the UFODAW icon, and double-click opens them in UFODAW
Root: HKA; Subkey: "Software\Classes\.mdaw"; ValueType: string; ValueName: ""; ValueData: "UFODAW.Project"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\UFODAW.Project"; ValueType: string; ValueName: ""; ValueData: "UFODAW project"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\UFODAW.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\UFODAW.exe"",0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\UFODAW.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\UFODAW.exe"" ""%1"""; Tasks: associate
; (the "Open with > Always use this app" choice of Windows keeps the path of one copy of the program: point it at the installed one)
Root: HKA; Subkey: "Software\Classes\Applications\UFODAW.exe\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\UFODAW.exe"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\UFODAW.exe\SupportedTypes"; ValueType: string; ValueName: ".mdaw"; ValueData: ""; Tasks: associate

[Run]
Filename: "{app}\UFODAW.exe"; Description: "Start UFODAW"; Flags: nowait postinstall skipifsilent
Filename: "{app}\UFODAW.exe"; Flags: nowait runasoriginaluser; Check: WizardSilent
