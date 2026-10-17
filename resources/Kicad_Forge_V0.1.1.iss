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
; 类型配置的首次运行种子（可改的副本会写到用户数据目录）
Source: "{#SourceDir}\config\*"; DestDir: "{app}\config"; Flags: ignoreversion recursesubdirs createallsubdirs
; plugins → %APPDATA%/KiCad_Forge/plugins (user-writable)
Source: "{#SourceDir}\plugins\*"; DestDir: "{userappdata}\KiCad_Forge\plugins"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[UninstallDelete]
; Uninstall keeps USER DATA by default — the database, user plugins and logs
; under {userappdata}\KiCad_Forge are intentionally NOT deleted (uninstalling
; must never destroy a component library index by accident). To remove them,
; delete the folder manually after uninstalling.
;
; Only app-local runtime leftovers are cleaned up here.
Type: filesandordirs; Name: "{app}\data"


[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
