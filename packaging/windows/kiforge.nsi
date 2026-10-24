; Compiled by the XPack NSIS backend with validated /D arguments.
Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
Name "KiCad Forge"
OutFile "${KFORGE_OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\KiCad_Forge"
InstallDirRegKey HKCU "Software\KiCad_Forge" "InstallDir"
RequestExecutionLevel user
ManifestDPIAware true
SetCompressor /SOLID lzma
!define MUI_ICON "${KFORGE_ICON}"
!define MUI_FINISHPAGE_RUN "$INSTDIR\KiCad_Forge.exe"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${KFORGE_LICENSE}"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"
VIProductVersion "${KFORGE_VERSION}.0"
VIAddVersionKey "ProductName" "KiCad Forge"
VIAddVersionKey "FileDescription" "KiCad Forge Installer"
VIAddVersionKey "FileVersion" "${KFORGE_VERSION}"
VIAddVersionKey "LegalCopyright" "KiCad Forge contributors"

Var UninstallFailed

!macro RemoveInstalledFile File
  ${If} ${FileExists} "${File}"
    ClearErrors
    Delete "${File}"
    ${If} ${Errors}
      StrCpy $UninstallFailed 1
    ${EndIf}
  ${EndIf}
!macroend

!macro RemoveInstalledDirectory Directory
  ${If} ${FileExists} "${Directory}\*.*"
    ClearErrors
    RMDir /r "${Directory}"
    ${If} ${Errors}
      StrCpy $UninstallFailed 1
    ${EndIf}
  ${EndIf}
!macroend

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "KiCad Forge requires 64-bit Windows."
    Abort
  ${EndIf}
  SetShellVarContext current
FunctionEnd

Function un.onInit
  SetShellVarContext current
FunctionEnd

Section "KiCad Forge" Main
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "${KFORGE_SOURCE}\KiCad_Forge.exe"
  File "${KFORGE_SOURCE}\*.dll"
  File "${KFORGE_SOURCE}\kforge-distribution.json"
  SetOutPath "$INSTDIR\runtime"
  File /r "${KFORGE_SOURCE}\runtime\*"
  SetOutPath "$INSTDIR\webui"
  File /r "${KFORGE_SOURCE}\webui\*"
  SetOutPath "$INSTDIR\config"
  File /r "${KFORGE_SOURCE}\config\*"
  SetOutPath "$INSTDIR\plugins"
  File /r /x __pycache__ /x *.pyc /x .git /x fetched /x staging /x node_modules "${KFORGE_SOURCE}\plugins\*"
  SetOutPath "$INSTDIR\licenses"
  File /r "${KFORGE_SOURCE}\licenses\*"
  SetOutPath "$INSTDIR"
  File "${KFORGE_LICENSE}"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\KiCad Forge"
  CreateShortcut "$SMPROGRAMS\KiCad Forge\KiCad Forge.lnk" "$INSTDIR\KiCad_Forge.exe"
  CreateShortcut "$SMPROGRAMS\KiCad Forge\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\KiCad_Forge" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KiCad_Forge" "DisplayName" "KiCad Forge"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KiCad_Forge" "DisplayVersion" "${KFORGE_VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KiCad_Forge" "DisplayIcon" "$INSTDIR\KiCad_Forge.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KiCad_Forge" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KiCad_Forge" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KiCad_Forge" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KiCad_Forge" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KiCad_Forge" "NoRepair" 1
SectionEnd

Section /o "Desktop shortcut"
  CreateShortcut "$DESKTOP\KiCad Forge.lnk" "$INSTDIR\KiCad_Forge.exe"
SectionEnd

Section "Uninstall"
  ; All mutable state is under the user's application-data directory.
  ; Preserve it and any app-local data/ from a previous portable installation.
  ; Windows cannot remove the current working directory. NSIS initializes
  ; $OUTDIR to $INSTDIR, so leave it before removing any installed files.
  SetOutPath "$TEMP"
  StrCpy $UninstallFailed 0
  !insertmacro RemoveInstalledFile "$INSTDIR\KiCad_Forge.exe"
  ; Do not dismantle a running application if its executable is still in use.
  ${If} $UninstallFailed == 0
    !insertmacro RemoveInstalledFile "$INSTDIR\*.dll"
    !insertmacro RemoveInstalledFile "$INSTDIR\LICENSE"
    !insertmacro RemoveInstalledFile "$INSTDIR\kforge-distribution.json"
    !insertmacro RemoveInstalledDirectory "$INSTDIR\webui"
    !insertmacro RemoveInstalledDirectory "$INSTDIR\config"
    !insertmacro RemoveInstalledDirectory "$INSTDIR\plugins"
    !insertmacro RemoveInstalledDirectory "$INSTDIR\licenses"
    !insertmacro RemoveInstalledDirectory "$INSTDIR\runtime"
  ${EndIf}
  ${If} $UninstallFailed != 0
    ; Keep both registration and the uninstaller available for another attempt.
    SetErrorLevel 1
    IfSilent +2
      MessageBox MB_ICONEXCLAMATION "Some files could not be removed. Close KiCad Forge and retry uninstalling."
    Quit
  ${EndIf}
  Delete "$DESKTOP\KiCad Forge.lnk"
  Delete "$SMPROGRAMS\KiCad Forge\KiCad Forge.lnk"
  Delete "$SMPROGRAMS\KiCad Forge\Uninstall.lnk"
  RMDir "$SMPROGRAMS\KiCad Forge"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KiCad_Forge"
  DeleteRegKey HKCU "Software\KiCad_Forge"
  !insertmacro RemoveInstalledFile "$INSTDIR\Uninstall.exe"
  ; Remove only an empty root; portable data/ and unrelated files are retained.
  RMDir "$INSTDIR"
SectionEnd
