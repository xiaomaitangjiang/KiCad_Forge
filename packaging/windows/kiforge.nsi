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

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "KiCad Forge requires 64-bit Windows."
    Abort
  ${EndIf}
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
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KiCad_Forge" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KiCad_Forge" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KiCad_Forge" "NoRepair" 1
SectionEnd

Section /o "Desktop shortcut"
  CreateShortcut "$DESKTOP\KiCad Forge.lnk" "$INSTDIR\KiCad_Forge.exe"
SectionEnd

Section "Uninstall"
  ; All mutable state is under the user's application-data directory.
  ; Preserve it and any app-local data/ from a previous portable installation.
  Delete "$INSTDIR\KiCad_Forge.exe"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\kforge-distribution.json"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir /r "$INSTDIR\webui"
  RMDir /r "$INSTDIR\config"
  RMDir /r "$INSTDIR\plugins"
  RMDir /r "$INSTDIR\licenses"
  RMDir /r "$INSTDIR\runtime"
  RMDir "$INSTDIR"
  Delete "$DESKTOP\KiCad Forge.lnk"
  Delete "$SMPROGRAMS\KiCad Forge\KiCad Forge.lnk"
  Delete "$SMPROGRAMS\KiCad Forge\Uninstall.lnk"
  RMDir "$SMPROGRAMS\KiCad Forge"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KiCad_Forge"
  DeleteRegKey HKCU "Software\KiCad_Forge"
SectionEnd
