; KiCad Forge Inno Setup script
#define MyAppName "Kicad_Forge"
#define MyAppVersion "0.1.1"
#define MyAppPublisher "Wesp_Wheat"
#define MyAppExeName "KiCad_Forge.exe"
; Defaults follow this script, including when ISCC is run from another directory.
; scripts/build_installer.py validates inputs and overrides these with /D options.
#ifndef AppSourceDir
  #define AppSourceDir AddBackslash(SourcePath) + "..\..\build\mingw\x86_64\release"
#endif
#ifndef AppLicenseFile
  #define AppLicenseFile AddBackslash(SourcePath) + "..\..\LICENSE"
#endif
#ifndef AppIconFile
  #define AppIconFile AddBackslash(SourcePath) + "..\..\resources\icons\app.ico"
#endif
#ifndef AppOutputDir
  #define AppOutputDir AddBackslash(SourcePath) + "..\..\dist\installers"
#endif
#ifndef AppOutputName
  #define AppOutputName "Kicad_Forge_Setup"
#endif

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
LicenseFile={#AppLicenseFile}
PrivilegesRequired=admin
OutputDir={#AppOutputDir}
OutputBaseFilename={#AppOutputName}
SetupIconFile={#AppIconFile}
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
Source: "{#AppSourceDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppSourceDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppSourceDir}\webui\*"; DestDir: "{app}\webui"; Flags: ignoreversion recursesubdirs createallsubdirs
; 类型配置的首次运行种子（可改的副本会写到用户数据目录）
Source: "{#AppSourceDir}\config\*"; DestDir: "{app}\config"; Flags: ignoreversion recursesubdirs createallsubdirs
; plugins → %APPDATA%/KiCad_Forge/plugins (user-writable)
Source: "{#AppSourceDir}\plugins\*"; DestDir: "{userappdata}\KiCad_Forge\plugins"; Excludes: "__pycache__,*.pyc,.git,fetched,staging"; Flags: ignoreversion recursesubdirs createallsubdirs
; 静态依赖的许可原文（PCRE2 + 其内置的 sljit）：随包分发，不能只放在仓库里
Source: "{#AppSourceDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs

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
