; Nintendall Windows installer (NSIS 3).
; Built by tools/build_windows.sh:
;   makensis -DVERSION=0.1.0 -DSRCDIR=<installed folder> -DOUTFILE=<setup.exe> installer/nintendall.nsi

Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!define APPNAME "Nintendall"
!ifndef VERSION
  !define VERSION "0.12.2"
!endif
!ifndef SRCDIR
  !error "Pass -DSRCDIR=<folder with nintendall.exe, SDL2.dll and cores\>"
!endif
!ifndef OUTFILE
  !define OUTFILE "Nintendall-${VERSION}-setup.exe"
!endif
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "Software\${APPNAME}" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "${APPNAME} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "FileDescription" "${APPNAME} Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "See THIRD_PARTY_LICENSES.md"

!define MUI_ICON "..\assets\nintendall.ico"
!define MUI_UNICON "..\assets\nintendall.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "This will install ${APPNAME} ${VERSION}.$\r$\n$\r$\nNintendall plays games from Nintendo systems. Open or drag in a game file and it works out the system and starts the right built-in emulator.$\r$\n$\r$\nYou need your own game files; none are included."
!define MUI_LICENSEPAGE_TEXT_TOP "Nintendall includes these open-source emulators, each under its own license."
!define MUI_LICENSEPAGE_BUTTON "&Next >"
!define MUI_LICENSEPAGE_TEXT_BOTTOM "Click Next to continue."
!define MUI_FINISHPAGE_RUN "$INSTDIR\nintendall.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APPNAME}"
!define MUI_FINISHPAGE_SHOWREADME ""
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Open the Games folder (put your games in here)"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION OpenGamesFolder

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRCDIR}\THIRD_PARTY_LICENSES.md"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${APPNAME} needs 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64

  ; Nintendall's Update Now runs: setup.exe /S /RESTART /WAITPID=<its process> /D=<folder>
  ; Wait (up to a minute) for that Nintendall to close, so its files can be replaced.
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/WAITPID=" $R1
  ${IfNot} ${Errors}
    System::Call 'kernel32::OpenProcess(i 0x00100000, i 0, i $R1) p.R2'
    ${If} $R2 != 0
      System::Call 'kernel32::WaitForSingleObject(p $R2, i 60000)'
      System::Call 'kernel32::CloseHandle(p $R2)'
    ${EndIf}
  ${EndIf}
FunctionEnd

; After Update Now: start Nintendall again. Through Explorer, so it runs as the user and not with
; the setup's administrator rights.
Function RestartNintendall
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/RESTART" $R1
  ${IfNot} ${Errors}
    Exec '"$WINDIR\explorer.exe" "$INSTDIR\nintendall.exe"'
  ${EndIf}
FunctionEnd

Function .onInstSuccess
  Call RestartNintendall
FunctionEnd

Function .onInstFailed
  Call RestartNintendall
FunctionEnd

Function OpenGamesFolder
  SetShellVarContext current
  ExecShell "open" "$DOCUMENTS\Nintendall\Games"
  SetShellVarContext all
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

Section "${APPNAME} (required)" SecMain
  SectionIn RO
  SetShellVarContext all

  ; Remove emulators from an older version so stale ones don't linger.
  RMDir /r "$INSTDIR\cores"

  SetOutPath "$INSTDIR"
  File "${SRCDIR}\nintendall.exe"
  File "${SRCDIR}\SDL2.dll"
  File "${SRCDIR}\SDL2_ttf.dll"
  File "${SRCDIR}\README.md"
  File "${SRCDIR}\THIRD_PARTY_LICENSES.md"
  File /r "${SRCDIR}\third_party"
  SetOutPath "$INSTDIR\fonts"
  File "${SRCDIR}\fonts\*.*"
  SetOutPath "$INSTDIR\cores"
  File "${SRCDIR}\cores\*.dll"
  ; Data some emulators need (Dolphin's Sys folder)
  RMDir /r "$INSTDIR\system"
  SetOutPath "$INSTDIR\system"
  File /r "${SRCDIR}\system\*.*"
  SetOutPath "$INSTDIR"

  ; Games folder (Documents\Nintendall\Games) with a folder for each console: put games in
  ; them and they show up on Nintendall's home screen. Uninstalling leaves it (it holds your games).
  ; Keep these names in sync with homeConsoles() in src/library.cpp.
  SetShellVarContext current
  CreateDirectory "$DOCUMENTS\Nintendall\Games\NES"
  CreateDirectory "$DOCUMENTS\Nintendall\Games\Game Boy"
  CreateDirectory "$DOCUMENTS\Nintendall\Games\SNES"
  CreateDirectory "$DOCUMENTS\Nintendall\Games\Nintendo 64"
  CreateDirectory "$DOCUMENTS\Nintendall\Games\Game Boy Color"
  CreateDirectory "$DOCUMENTS\Nintendall\Games\Game Boy Advance"
  CreateDirectory "$DOCUMENTS\Nintendall\Games\GameCube"
  CreateDirectory "$DOCUMENTS\Nintendall\Games\Nintendo DS"
  CreateDirectory "$DOCUMENTS\Nintendall\Games\Wii"
  CreateDirectory "$DOCUMENTS\Nintendall\Games\Nintendo 3DS"
  CreateDirectory "$DOCUMENTS\Nintendall\Games\Wii U"
  SetShellVarContext all

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\nintendall.exe"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"

  ; Apps & features entry
  WriteRegStr HKLM "Software\${APPNAME}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\nintendall.exe"
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINSTKEY}" "EstimatedSize" "$0"
SectionEnd

Section "Desktop shortcut" SecDesktop
  SetShellVarContext all
  ; Silent installs are Nintendall updating itself: keep the user's choice (no shortcut if they removed it).
  ${If} ${Silent}
  ${AndIfNot} ${FileExists} "$DESKTOP\${APPNAME}.lnk"
    Return
  ${EndIf}
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\nintendall.exe"
SectionEnd

LangString DESC_Main ${LANG_ENGLISH} "The Nintendall app and all of its built-in emulators."
LangString DESC_Desktop ${LANG_ENGLISH} "Put a Nintendall shortcut on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} $(DESC_Main)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} $(DESC_Desktop)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  SetShellVarContext all
  RMDir /r "$INSTDIR\cores"
  RMDir /r "$INSTDIR\fonts"
  RMDir /r "$INSTDIR\system"
  Delete "$INSTDIR\nintendall.exe"
  Delete "$INSTDIR\SDL2.dll"
  Delete "$INSTDIR\SDL2_ttf.dll"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\THIRD_PARTY_LICENSES.md"
  RMDir /r "$INSTDIR\third_party"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$DESKTOP\${APPNAME}.lnk"
  RMDir /r "$SMPROGRAMS\${APPNAME}"
  DeleteRegKey HKLM "${UNINSTKEY}"
  DeleteRegKey HKLM "Software\${APPNAME}"
  ; Saves, save states and settings in %APPDATA%\Nintendall are kept on purpose.
SectionEnd
