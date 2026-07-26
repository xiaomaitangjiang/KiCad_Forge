; KiCad Forge Inno Setup script
#define MyAppName "Kicad_Forge"
#define MyAppVersion "0.1.1"
#define MyAppPublisher "Wesp_Wheat"
#define MyAppExeName "KiCad_Forge.exe"
#define SourceDir "D:\VScode\KiCad_Forge\build\mingw\x86_64\release"

[Setup]
AppId={{5479F8E3-FE61-4F6F-9299-B34DEBA78041}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\Kicad_Forge
UninstallDisplayIcon={app}\{#MyAppExeName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
LicenseFile=D:\VScode\KiCad_Forge\LICENSE
PrivilegesRequired=admin
OutputDir=D:\VScode\KiCad_Forge\install_file
OutputBaseFilename=Kicad_Forge_Setup
SetupIconFile=D:\VScode\KiCad_Forge\resources\app.ico
Compression=lzma/ultra64
SolidCompression=yes
WizardStyle=modern dynamic
UsedUserAreasWarning=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; exe + DLLs + webui → Program Files
Source: "{#SourceDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\webui\*"; DestDir: "{app}\webui"; Flags: ignoreversion recursesubdirs createallsubdirs
; plugins → %APPDATA%/KiCad_Forge/plugins (user-writable)
Source: "{#SourceDir}\plugins\*"; DestDir: "{userappdata}\KiCad_Forge\plugins"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
