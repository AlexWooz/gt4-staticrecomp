; GT4Recomp per-machine installer (1.2.1 and later; TASK-091, owner 2026-10-22). Needs administrator rights.
; Build (from the repository root, after the launcher and the game are built; every path ABSOLUTE, the .nsi too):
;   makensis /DVERSION=1.2.1 /DLAUNCHER_EXE=<path>\GT4Recomp.exe /DGAME_DIR=<path>\game ^
;            /DSDL3_DLL=<path>\SDL3.dll /DWEBVIEW2_BOOTSTRAPPER=<path>\MicrosoftEdgeWebview2Setup.exe ^
;            /DOUTFILE=<path>\GT4Recomp-Setup-1.2.1.exe <path>\installer\gt4recomp.nsi
; A test build adds /DLABEL=rc5 (output GT4Recomp-Setup-1.3.0-rc5.exe; VERSION stays numeric).
; The default output name GT4Recomp-Setup-<VERSION>.exe is the asset name the launcher's updater looks for
; (launcher\crates\gt4l-core\src\releases.rs, docs/RELEASES.md).
; WEBVIEW2_BOOTSTRAPPER is Microsoft's Evergreen bootstrapper (https://go.microsoft.com/fwlink/p/?LinkId=2124703),
; Authenticode-signed by Microsoft Corporation. It is not in git; the release build records its SHA-256. The launcher
; window is a WebView2 page: the bootstrapper runs only when the runtime is missing (see WebView2Present below).
; SDL3_DLL is SDL3 3.4.18 (runtime\SDL3.dll of SDL3-3.4.18-win32-x64.zip). It ships beside GT4Recomp.exe: the
; launcher reads pads through SDL3 (launcher\crates\gt4l-platform\src\sdl_pads.rs).
; GAME_DIR must hold gt4.exe and (from 1.3.0, TASK-101; renamed TASK-108) gt4-spec2.exe, the runtime DLLs, shaders\ and the licence texts. It is not in git.
;
; Layout (TASK-091; docs/BUILD.md "Slice 250", docs/release-notes/1.2.1.md):
; - Program files in $INSTDIR (default C:\Program Files\GT4Recomp; the directory page lets the player pick any folder).
; - Large data in $INSTDIR\data (disc copy, prepared files, gt4-gs, caches, sessions, logs, Textures). The launcher
;   finds it relative to GT4Recomp.exe, so the folder can be copied elsewhere (portable). This script creates it and
;   gives the local Users group (SID S-1-5-32-545) Modify on that folder only; it never writes into it.
; - User data per Windows user in %APPDATA%\GT4Recomp (settings, controls, saves, replays), written by the launcher.
; The launcher, not this script, moves the files of 1.2.0 (%LOCALAPPDATA%\GT4Recomp, Documents\Gran Turismo 4) at its
; first start, running as the player. This script only removes the old per-user program install (never its data).
; A reinstall into another folder (TASK-091, slice 252) removes the previous program folder's files and records its
; data folder as PreviousDataDir (HKLM); the launcher moves that data at its first start. A self-update runs silently
; with /S /UPDATE /D=<install folder> and starts the launcher again as the player.
; - A desktop shortcut "Gran Turismo 4" on the all-users desktop (TASK-094, 1.2.3), offered on the finish page and kept
;   as DesktopShortcut in HKLM (1 created, 0 declined); a silent run follows that choice (see the Desktop shortcut section).
; Updates are safe (TASK-091, slice 255): .onInit waits for the files of an existing install to be free (up to 30 s);
; the new program is first extracted into $INSTDIR\.update-staging, then swapped in item by item, and a failed rename
; rolls the previous files back (see SwapStagedIn and RollbackSwap). data\ is never touched by the swap. Each run
; appends one line to data\logs\setup.log.

!ifndef VERSION
  !define VERSION "1.3.0"
!endif
; A test build (TASK-109): /DLABEL=rc5 makes the shown version, version.json and the default output name carry -rc5
; (GT4Recomp-Setup-1.3.0-rc5.exe). VERSION stays numeric; it must match the launcher's Cargo version.
!ifdef LABEL
  !define FULL_VERSION "${VERSION}-${LABEL}"
!else
  !define FULL_VERSION "${VERSION}"
!endif
!ifndef LAUNCHER_EXE
  !error "Define LAUNCHER_EXE=<path to GT4Recomp.exe>"
!endif
!ifndef GAME_DIR
  !error "Define GAME_DIR=<folder with gt4.exe and its runtime files>"
!endif
!ifndef SDL3_DLL
  !error "Define SDL3_DLL=<path to SDL3.dll, SDL 3.4.18>"
!endif
!ifndef WEBVIEW2_BOOTSTRAPPER
  !error "Define WEBVIEW2_BOOTSTRAPPER=<path to MicrosoftEdgeWebview2Setup.exe>"
!endif
!ifndef OUTFILE
  !define OUTFILE "GT4Recomp-Setup-${FULL_VERSION}.exe"
!endif
; The Evergreen WebView2 Runtime's client id (Microsoft, "Distribute your app and the WebView2 Runtime").
!define WEBVIEW2_CLIENT "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"

!define APP_NAME "Gran Turismo 4"
!define INTERNAL_NAME "GT4Recomp"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${INTERNAL_NAME}"
!define APP_KEY "Software\${INTERNAL_NAME}"
; Well-known SID of the local Users group (BUILTIN\Users). Never a localized group name.
!define USERS_SID "S-1-5-32-545"
; The expected size of the data the launcher puts in $INSTDIR\data (disc copy, prepared files, gt4-gs), in KiB
; (about 5.2 GB), added to the program size in the uninstall entry.
!define EXPECTED_DATA_KB 5452595

Unicode true
Name "${APP_NAME}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${INTERNAL_NAME}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
ShowInstDetails show
BrandingText "${APP_NAME} ${FULL_VERSION}"
; The installer's own version resource (Explorer's Details tab and the antivirus file reputation read it; it had none
; up to 1.3.0-rc10, when a laptop's Defender flagged that installer by machine learning). VIProductVersion needs four
; numbers, so the release candidate label is only in the text fields.
VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=0 "ProductName" "GT4Recomp"
VIAddVersionKey /LANG=0 "CompanyName" "GT4Recomp contributors"
VIAddVersionKey /LANG=0 "FileDescription" "GT4Recomp setup"
VIAddVersionKey /LANG=0 "FileVersion" "${FULL_VERSION}"
VIAddVersionKey /LANG=0 "ProductVersion" "${FULL_VERSION}"
VIAddVersionKey /LANG=0 "LegalCopyright" "Copyright (c) 2026 GT4Recomp contributors. MIT License."
VIAddVersionKey /LANG=0 "OriginalFilename" "GT4Recomp-Setup-${FULL_VERSION}.exe"

!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

!define MUI_ICON "${__FILEDIR__}\..\launcher\assets\brand\launcher.ico"
!define MUI_UNICON "${__FILEDIR__}\..\launcher\assets\brand\launcher.ico"
!define MUI_ABORTWARNING

; Welcome: the trademark notice is the first thing the user reads.
!define MUI_WELCOMEPAGE_TEXT "GT4Recomp is an unofficial fan project, not affiliated with Sony Interactive Entertainment or Polyphony Digital.$\r$\n$\r$\nGran Turismo and the Gran Turismo 4 logo, emblem and cover are trademarks or copyrighted works of their owners. You need your own copy of Gran Turismo 4 (USA, v2.00) to play. See NOTICE.md."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${__FILEDIR__}\..\LICENSE"
!define MUI_DIRECTORYPAGE_TEXT_TOP "GT4Recomp keeps its program files and its large data (a copy of your disc image, about 5 GB) in this folder, in its data subfolder. Your settings, saves and replays are kept for each Windows user in AppData.$\r$\n$\r$\nThe whole folder can later be copied to another drive or PC and started from there."
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
; The setup runs elevated. The launcher must start as the player (its user data and the move of the 1.2.0 files
; belong to that account), so the finish page asks Explorer to start it with the player's own rights.
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP_NAME}"
!define MUI_FINISHPAGE_RUN_FUNCTION StartLauncherAsPlayer
; TASK-094 (1.2.3): the desktop shortcut checkbox, ticked by default (no MUI_FINISHPAGE_SHOWREADME_NOTCHECKED). The
; checkbox is set from the stored choice when the page is shown and saved when the page is left (Desktop shortcut section).
!define MUI_FINISHPAGE_SHOWREADME
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Create a desktop shortcut"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION CreateDesktopShortcut
!define MUI_PAGE_CUSTOMFUNCTION_SHOW FinishPageShow
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE FinishPageLeave
!insertmacro MUI_PAGE_FINISH

Var DeleteDataCheckbox
Var DeleteDataState
Var WebView2Version
; 1 when /UPDATE was given: the launcher's self-update (TASK-091). Silent with /S, it starts the launcher when done.
Var UpdateRun
; The result of this run, written to data\logs\setup.log: ok, locked: <file>, extract failed, swap failed ... (TASK-091).
Var SetupResult
; TASK-091 (1.2.1): phase timings in ms (GetTickCount deltas), appended to the setup.log line.
Var TickNow
Var TickPrev
Var TickDelta
Var SetupTimings
Var DataExisted

; Adds "NAME=<ms since the previous mark>" to $SetupTimings.
!macro TimingMark NAME
  System::Call 'kernel32::GetTickCount()i.s'
  Pop $TickNow
  IntOp $TickDelta $TickNow - $TickPrev
  StrCpy $SetupTimings "$SetupTimings ${NAME}=$TickDelta"
  StrCpy $TickPrev $TickNow
!macroend

; Sets $WebView2Version to the installed Evergreen WebView2 Runtime version, or "" when it is missing. Microsoft's rule:
; the runtime is present when the "pv" value of its client key exists and is neither empty nor 0.0.0.0. Per-machine
; installs write it under HKLM (the WOW6432Node view on 64-bit Windows), per-user installs under HKCU.
Function WebView2Present
  StrCpy $WebView2Version ""
  SetRegView 64
  ReadRegStr $0 HKLM "SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\${WEBVIEW2_CLIENT}" "pv"
  ${If} $0 == ""
  ${OrIf} $0 == "0.0.0.0"
    ReadRegStr $0 HKLM "SOFTWARE\Microsoft\EdgeUpdate\Clients\${WEBVIEW2_CLIENT}" "pv"
  ${EndIf}
  ${If} $0 == ""
  ${OrIf} $0 == "0.0.0.0"
    ReadRegStr $0 HKCU "Software\Microsoft\EdgeUpdate\Clients\${WEBVIEW2_CLIENT}" "pv"
  ${EndIf}
  SetRegView 32
  ${If} $0 == ""
  ${OrIf} $0 == "0.0.0.0"
    ReadRegStr $0 HKLM "SOFTWARE\Microsoft\EdgeUpdate\Clients\${WEBVIEW2_CLIENT}" "pv"
  ${EndIf}
  SetRegView lastused
  ${If} $0 != ""
  ${AndIf} $0 != "0.0.0.0"
    StrCpy $WebView2Version $0
  ${EndIf}
FunctionEnd

; The launcher starts this setup from an update and exits at once. The files of an existing install are waited for
; below (no fixed pause). An earlier per-machine install is upgraded in its own folder (unless /D= was given).
; The launcher's self-update passes "/S /UPDATE /D=<install folder>" (TASK-091, updates.rs setup_arguments): silent,
; into the folder of the running launcher, which is then started again as the player (see the Install section).
Function .onInit
  System::Call 'kernel32::GetTickCount()i.s'
  Pop $TickPrev
  StrCpy $SetupTimings ""
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "GT4Recomp needs 64-bit Windows." /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  ReadRegStr $0 HKLM "${APP_KEY}" "InstallDir"
  ${If} $0 != ""
  ${AndIf} $INSTDIR == "$PROGRAMFILES64\${INTERNAL_NAME}"
    StrCpy $INSTDIR $0
  ${EndIf}
  StrCpy $UpdateRun 0
  ClearErrors
  ${GetParameters} $R0
  ${GetOptions} $R0 "/UPDATE" $R1
  ${IfNot} ${Errors}
    StrCpy $UpdateRun 1
  ${EndIf}

  ; Waits for the files this install replaces to be free (TASK-091), instead of a fixed pause. Nothing is changed yet.
  ; Silent: logged, then aborted with error level 2. Interactive: Retry waits again, Cancel aborts.
  ${If} ${FileExists} "$INSTDIR\GT4Recomp.exe"
    Call WaitForInstallFree
    ${While} $R8 != ""
      StrCpy $SetupResult "locked: $R8"
      ${If} ${Silent}
        Call LogSetupResult
        SetErrorLevel 2
        Abort
      ${EndIf}
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Gran Turismo 4 is still running ($R8). Close it and press Retry." /SD IDCANCEL IDRETRY lockedRetry
      Call LogSetupResult
      SetErrorLevel 2
      Abort
    lockedRetry:
      Call WaitForInstallFree
    ${EndWhile}
  ${EndIf}
  !insertmacro TimingMark "onInitWait"
FunctionEnd

; ---- Safe update helpers (TASK-091, slice 255) ----
; All helpers work on the install in $INSTDIR. Registers: $R1 first item that could not be rolled back, $R2 error
; level, $R3..$R6 scratch, $R7 folder, $R8 first locked or failed item, $R9 item or file path.

!macro SwapStep ITEM
  StrCpy $R9 "${ITEM}"
  Call SwapItem
  ${If} $R8 != ""
    Return
  ${EndIf}
!macroend

!macro RollbackStep ITEM
  StrCpy $R9 "${ITEM}"
  Call RollbackItem
!macroend

; $R7 = a folder, $R9 = an item name. $R6 = "1" when the item (file or folder) exists in that folder.
Function ItemIn
  StrCpy $R6 "0"
  ${If} ${FileExists} "$R7\$R9"
  ${OrIf} ${FileExists} "$R7\$R9\*.*"
    StrCpy $R6 "1"
  ${EndIf}
FunctionEnd

; $R9 = a file. $R8 takes its path when the file exists but cannot be opened for writing (it runs, or it is locked).
Function TestFileFree
  ${If} ${FileExists} "$R9"
    ClearErrors
    FileOpen $R5 "$R9" a
    ${If} ${Errors}
    ${OrIf} $R5 == ""
      ${If} $R8 == ""
        StrCpy $R8 $R9
      ${EndIf}
    ${Else}
      FileClose $R5
    ${EndIf}
  ${EndIf}
FunctionEnd

; Sets $R8 to the first file the install replaces that is locked, or "" when all are free: the launcher, the SDL3 DLL,
; the game executable and every game\*.dll. A file that does not exist counts as free.
Function FindLockedInstallFile
  StrCpy $R8 ""
  StrCpy $R9 "$INSTDIR\GT4Recomp.exe"
  Call TestFileFree
  StrCpy $R9 "$INSTDIR\SDL3.dll"
  Call TestFileFree
  StrCpy $R9 "$INSTDIR\game\gt4.exe"
  Call TestFileFree
  ; TASK-101 (1.3.0): the second game's program, shipped beside the first. A missing file counts as free.
  ; TASK-108 (1.3.0): the program names are gt4.exe and gt4-spec2.exe; a 1.2.x install left gt4vulkan*.exe, checked too.
  StrCpy $R9 "$INSTDIR\game\gt4-spec2.exe"
  Call TestFileFree
  StrCpy $R9 "$INSTDIR\game\gt4vulkan.exe"
  Call TestFileFree
  StrCpy $R9 "$INSTDIR\game\gt4vulkan-spec2.exe"
  Call TestFileFree
  FindFirst $R0 $R1 "$INSTDIR\game\*.dll"
  ${DoWhile} $R1 != ""
    StrCpy $R9 "$INSTDIR\game\$R1"
    Call TestFileFree
    FindNext $R0 $R1
  ${Loop}
  FindClose $R0
FunctionEnd

; Waits up to 30 s (60 checks, 500 ms apart) for the files to be free. $R8 = the first locked file, or "".
Function WaitForInstallFree
  StrCpy $R7 0
  ${Do}
    Call FindLockedInstallFile
    ${If} $R8 == ""
      ${Break}
    ${EndIf}
    IntOp $R7 $R7 + 1
    ${If} $R7 >= 60
      ${Break}
    ${EndIf}
    Sleep 500
  ${Loop}
FunctionEnd

; Appends one line to data\logs\setup.log: the version, the mode, the update flag and $SetupResult.
Function LogSetupResult
  StrCpy $R4 "interactive"
  ${If} ${Silent}
    StrCpy $R4 "silent"
  ${EndIf}
  CreateDirectory "$INSTDIR\data\logs"
  ClearErrors
  FileOpen $R3 "$INSTDIR\data\logs\setup.log" a
  ${IfNot} ${Errors}
    FileSeek $R3 0 END
    FileWrite $R3 "setup ${FULL_VERSION}: mode $R4, update flag $UpdateRun, result: $SetupResult, timings ms:$SetupTimings$\r$\n"
    StrCpy $SetupTimings ""
    FileClose $R3
  ${EndIf}
FunctionEnd

; Ends a failed run: logs $SetupResult, tells an interactive user, then aborts with the error level in $R2.
Function FailInstall
  Call LogSetupResult
  ${IfNot} ${Silent}
    MessageBox MB_OK|MB_ICONSTOP "GT4Recomp could not update its program files ($SetupResult). Details: $INSTDIR\data\logs\setup.log" /SD IDOK
  ${EndIf}
  SetErrorLevel $R2
  Abort
FunctionEnd

; Checks the extracted copy in .update-staging: $R8 = "extract failed" when an extraction error was flagged or a key
; file is missing, "" otherwise.
Function CheckStagedFiles
  StrCpy $R8 ""
  ${If} ${Errors}
  ${OrIfNot} ${FileExists} "$INSTDIR\.update-staging\GT4Recomp.exe"
  ${OrIfNot} ${FileExists} "$INSTDIR\.update-staging\SDL3.dll"
  ${OrIfNot} ${FileExists} "$INSTDIR\.update-staging\game\gt4.exe"
    StrCpy $R8 "extract failed"
  ${EndIf}
FunctionEnd

; Moves one live item to .update-old and its staged copy into its place. $R9 = the item. $R8 = the item when a rename
; fails (the caller rolls back), "" otherwise.
Function SwapItem
  StrCpy $R8 ""
  StrCpy $R7 "$INSTDIR"
  Call ItemIn
  ${If} $R6 == "1"
    ClearErrors
    Rename "$INSTDIR\$R9" "$INSTDIR\.update-old\$R9"
    ${If} ${Errors}
      StrCpy $R8 $R9
      Return
    ${EndIf}
  ${EndIf}
  ClearErrors
  Rename "$INSTDIR\.update-staging\$R9" "$INSTDIR\$R9"
  ${If} ${Errors}
    StrCpy $R8 $R9
  ${EndIf}
FunctionEnd

; Swaps the staged program in, in this order: GT4Recomp.exe, SDL3.dll, NOTICE.md, LICENSE.txt, game. $R8 = the item
; whose rename failed, or "".
Function SwapStagedIn
  StrCpy $R8 ""
  !insertmacro SwapStep "GT4Recomp.exe"
  !insertmacro SwapStep "SDL3.dll"
  !insertmacro SwapStep "NOTICE.md"
  !insertmacro SwapStep "LICENSE.txt"
  !insertmacro SwapStep "game"
FunctionEnd

Function RollbackItemFailed
  ${If} $R1 == ""
    StrCpy $R1 $R9
  ${EndIf}
FunctionEnd

; Restores one item after a failed swap, from the state it is in. $R9 = the item.
; - A previous copy in .update-old goes back in place; the new copy now in place (if any) returns to staging first.
; - A new item with no previous copy goes back to staging (it is not in the previous install).
Function RollbackItem
  StrCpy $R7 "$INSTDIR\.update-old"
  Call ItemIn
  StrCpy $R3 $R6
  StrCpy $R7 "$INSTDIR"
  Call ItemIn
  StrCpy $R4 $R6
  StrCpy $R7 "$INSTDIR\.update-staging"
  Call ItemIn
  StrCpy $R2 $R6
  ${If} $R3 == "1"
    ${If} $R4 == "1"
      Delete "$INSTDIR\.update-staging\$R9"
      RMDir /r "$INSTDIR\.update-staging\$R9"
      ClearErrors
      Rename "$INSTDIR\$R9" "$INSTDIR\.update-staging\$R9"
      ${If} ${Errors}
        Call RollbackItemFailed
        Return
      ${EndIf}
    ${EndIf}
    ClearErrors
    Rename "$INSTDIR\.update-old\$R9" "$INSTDIR\$R9"
    ${If} ${Errors}
      Call RollbackItemFailed
    ${EndIf}
  ${ElseIf} $R4 == "1"
    ${If} $R2 != "1"
      ClearErrors
      Rename "$INSTDIR\$R9" "$INSTDIR\.update-staging\$R9"
      ${If} ${Errors}
        Call RollbackItemFailed
      ${EndIf}
    ${EndIf}
  ${EndIf}
FunctionEnd

; Restores every item (see RollbackItem). $R1 = the first item that could not be restored, or "".
Function RollbackSwap
  StrCpy $R1 ""
  !insertmacro RollbackStep "GT4Recomp.exe"
  !insertmacro RollbackStep "SDL3.dll"
  !insertmacro RollbackStep "NOTICE.md"
  !insertmacro RollbackStep "LICENSE.txt"
  !insertmacro RollbackStep "game"
FunctionEnd

; Explorer runs the launcher with the rights of the signed-in player, not this elevated setup's.
Function StartLauncherAsPlayer
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\GT4Recomp.exe"'
FunctionEnd

; ---- Desktop shortcut (TASK-094, 1.2.3) ----
; The shortcut "Gran Turismo 4" on the all-users desktop starts $INSTDIR\GT4Recomp.exe. Its choice is kept in HKLM
; DesktopShortcut: 1 = created, 0 = declined on the finish page. An interactive run asks on the finish page; a silent run
; (no finish page, also the launcher's /S /UPDATE) follows the stored choice in DesktopShortcutSilent.

; Creates the shortcut (the working folder is $INSTDIR, set first) and records 1 when it was made. Called by the finish
; page checkbox (MUI_FINISHPAGE_SHOWREADME_FUNCTION) and by DesktopShortcutSilent. The current SetOutPath is $INSTDIR at
; every call site, so nothing else depends on it.
Function CreateDesktopShortcut
  SetRegView 64
  SetShellVarContext all
  SetOutPath "$INSTDIR"
  ClearErrors
  CreateShortCut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\GT4Recomp.exe" "" "$INSTDIR\GT4Recomp.exe" 0
  ${If} ${Errors}
    DetailPrint "The desktop shortcut could not be created"
  ${Else}
    WriteRegDWORD HKLM "${APP_KEY}" "DesktopShortcut" 1
  ${EndIf}
FunctionEnd

; Silent runs (install, or the launcher's /S /UPDATE). DesktopShortcut missing (an install from before 1.2.3, or a fresh
; silent install) or 1: the shortcut is created or made again. 0: nothing is done.
Function DesktopShortcutSilent
  SetRegView 64
  ClearErrors
  ReadRegDWORD $0 HKLM "${APP_KEY}" "DesktopShortcut"
  ${If} ${Errors}
  ${OrIf} $0 == 1
    Call CreateDesktopShortcut
  ${EndIf}
FunctionEnd

; The finish page shows the checkbox ticked, unless the stored choice is 0 (declined before).
Function FinishPageShow
  SetRegView 64
  ClearErrors
  ReadRegDWORD $0 HKLM "${APP_KEY}" "DesktopShortcut"
  ${If} ${Errors}
    Return
  ${EndIf}
  ${If} $0 == 0
    ${NSD_Uncheck} $mui.FinishPage.ShowReadme
  ${EndIf}
FunctionEnd

; Leaving the finish page: an unticked box is kept as 0 (a ticked one runs CreateDesktopShortcut, after this function).
Function FinishPageLeave
  SendMessage $mui.FinishPage.ShowReadme ${BM_GETCHECK} 0 0 $0
  ${If} $0 != ${BST_CHECKED}
    SetRegView 64
    WriteRegDWORD HKLM "${APP_KEY}" "DesktopShortcut" 0
  ${EndIf}
FunctionEnd

; Removes one folder of the old per-user install (1.0.0 to 1.2.0) given on the stack: only the files that installer
; wrote, and only when the folder looks like it (GT4Recomp.exe or uninstall.exe there), is not the folder being
; installed now and holds no data folder. The launcher of 1.2.0 may still be exiting, so its file is retried briefly.
Function RemoveOldFolder
  Exch $R1
  Push $R2
  Push $R3
  ; C:\X\ and C:\X are the same folder: one trailing backslash is dropped before the comparison with the install
  ; folder, so the install being made is never deleted (a root such as C:\ keeps its backslash).
  StrLen $R3 $R1
  ${If} $R3 > 3
    StrCpy $R2 $R1 1 -1
    ${If} $R2 == "\"
      StrCpy $R1 $R1 -1
    ${EndIf}
  ${EndIf}
  ${If} $R1 != ""
  ${AndIf} $R1 != $INSTDIR
  ${AndIfNot} ${FileExists} "$R1\data\*.*"
    ${If} ${FileExists} "$R1\GT4Recomp.exe"
    ${OrIf} ${FileExists} "$R1\uninstall.exe"
      DetailPrint "Removing the old per-user program files in $R1"
      StrCpy $R2 0
      ${Do}
        ClearErrors
        Delete "$R1\GT4Recomp.exe"
        ${IfNot} ${Errors}
          ${Break}
        ${EndIf}
        IntOp $R2 $R2 + 1
        ${If} $R2 >= 10
          DetailPrint "$R1\GT4Recomp.exe is in use; the launcher removes it at its next start"
          ${Break}
        ${EndIf}
        Sleep 500
      ${Loop}
      Delete "$R1\SDL3.dll"
      Delete "$R1\NOTICE.md"
      Delete "$R1\LICENSE.txt"
      Delete "$R1\version.json"
      Delete "$R1\uninstall.exe"
      RMDir /r "$R1\game"
      RMDir "$R1"
    ${EndIf}
  ${EndIf}
  Pop $R3
  Pop $R2
  Pop $R1
FunctionEnd

; The per-user install of 1.0.0 to 1.2.0, as the account running this setup sees it: its program folder (registry
; InstallDir, the uninstall entry's InstallLocation, and the default %LOCALAPPDATA%\Programs\GT4Recomp), its Start-menu
; shortcuts and its HKCU entries. %LOCALAPPDATA%\GT4Recomp (the 1.2.0 data) is never touched here. When an administrator
; other than the player runs this setup, the launcher cleans the player's own leftovers at its first start.
Function RemoveOldPerUserInstall
  SetShellVarContext current
  ReadRegStr $0 HKCU "${APP_KEY}" "InstallDir"
  Push $0
  Call RemoveOldFolder
  ReadRegStr $0 HKCU "${UNINSTALL_KEY}" "InstallLocation"
  Push $0
  Call RemoveOldFolder
  Push "$LOCALAPPDATA\Programs\${INTERNAL_NAME}"
  Call RemoveOldFolder
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  DeleteRegKey HKCU "${APP_KEY}"
FunctionEnd

; Normalises the folder in $0 for comparisons: one trailing backslash is dropped (a root such as C:\ keeps it). The
; comparisons of NSIS strings (StrCmp, LogicLib ==) ignore case.
Function NormalizeFolder
  StrLen $1 $0
  ${If} $1 > 3
    StrCpy $2 $0 1 -1
    ${If} $2 == "\"
      StrCpy $0 $0 -1
    ${EndIf}
  ${EndIf}
FunctionEnd

; Sets $R5 to the program folder of the previous per-machine install (HKLM InstallDir, 64-bit view) when it is another
; folder that holds GT4Recomp.exe. $R5 = "" otherwise (nothing installed, or the same folder: an update).
Function FindPreviousProgramFolder
  StrCpy $R5 ""
  SetRegView 64
  ReadRegStr $R5 HKLM "${APP_KEY}" "InstallDir"
  ${If} $R5 == ""
    Return
  ${EndIf}
  StrCpy $0 $R5
  Call NormalizeFolder
  StrCpy $R5 $0
  StrCpy $0 $INSTDIR
  Call NormalizeFolder
  ${If} $R5 == $0
    StrCpy $R5 ""
    Return
  ${EndIf}
  ${IfNot} ${FileExists} "$R5\GT4Recomp.exe"
    StrCpy $R5 ""
  ${EndIf}
FunctionEnd

; Before the launcher starts (TASK-091, 1.2.1): a reinstall into another folder records the previous data folder, which
; the launcher moves at its first start. That data folder is not touched here.
Function RecordPreviousDataDir
  Call FindPreviousProgramFolder
  ${If} $R5 == ""
    Return
  ${EndIf}
  ${If} ${FileExists} "$R5\data\*.*"
    WriteRegStr HKLM "${APP_KEY}" "PreviousDataDir" "$R5\data"
    DetailPrint "Its data folder $R5\data is kept; the launcher moves it into $INSTDIR\data at its first start"
  ${EndIf}
FunctionEnd

; After the launcher has started (TASK-091, 1.2.1): the program files of the previous per-machine folder go, then the
; folder itself when it is empty. Its data folder is never touched (see RecordPreviousDataDir).
Function RemovePreviousProgramFolder
  Call FindPreviousProgramFolder
  ${If} $R5 == ""
    Return
  ${EndIf}
  DetailPrint "The previous install in $R5 is replaced by $INSTDIR"
  ; The launcher of the previous install may still be closing: its file is retried briefly.
  StrCpy $R6 0
  ${Do}
    ClearErrors
    Delete "$R5\GT4Recomp.exe"
    ${IfNot} ${Errors}
      ${Break}
    ${EndIf}
    IntOp $R6 $R6 + 1
    ${If} $R6 >= 10
      DetailPrint "$R5\GT4Recomp.exe is in use; it stays there"
      ${Break}
    ${EndIf}
    Sleep 500
  ${Loop}
  Delete "$R5\SDL3.dll"
  Delete "$R5\NOTICE.md"
  Delete "$R5\LICENSE.txt"
  Delete "$R5\version.json"
  Delete "$R5\uninstall.exe"
  RMDir /r "$R5\game"
  ; Not recursive: the folder goes only when it is empty (a data folder that stays keeps it).
  RMDir "$R5"
FunctionEnd

; $R0 = "1" when the data folder accepts a new file (one is created and deleted again), "0" otherwise (TASK-091, 1.2.1).
Function ProbeDataWritable
  StrCpy $R0 "0"
  ClearErrors
  FileOpen $R9 "$INSTDIR\data\.write-probe" w
  ${IfNot} ${Errors}
    FileClose $R9
    Delete "$INSTDIR\data\.write-probe"
    StrCpy $R0 "1"
  ${EndIf}
FunctionEnd

; Gives the local Users group Modify on the data folder (inherited by its files), so the launcher can write it as any
; player. A fresh install runs it; an update runs it only when the probe fails (TASK-091, 1.2.1).
Function GrantDataModify
  nsExec::ExecToLog '"$SYSDIR\icacls.exe" "$INSTDIR\data" /grant *${USERS_SID}:(OI)(CI)M'
  Pop $0
  ${If} $0 != 0
    DetailPrint "icacls exit code $0: the data folder may not be writable for players"
    MessageBox MB_OK|MB_ICONEXCLAMATION "The permissions of $INSTDIR\data could not be set (icacls exit code $0). GT4Recomp will keep its data in your AppData folder instead." /SD IDOK
  ${EndIf}
FunctionEnd

; Appends the timings of the phases after the launcher start to setup.log (TASK-091, 1.2.1).
Function LogTimings
  CreateDirectory "$INSTDIR\data\logs"
  ClearErrors
  FileOpen $R3 "$INSTDIR\data\logs\setup.log" a
  ${IfNot} ${Errors}
    FileSeek $R3 0 END
    FileWrite $R3 "setup ${FULL_VERSION}: after the launcher start, timings ms:$SetupTimings$\r$\n"
    FileClose $R3
  ${EndIf}
FunctionEnd

!insertmacro MUI_UNPAGE_WELCOME
UninstPage custom un.DataPageCreate un.DataPageLeave
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Install" SecInstall
  SetRegView 64

  ; An update interrupted in the middle of its swap left the marker .update-swapping (TASK-091): .update-old then holds
  ; the previous install, which is put back before anything else. Without the marker the swap had completed and only
  ; the clean-up was blocked (an antivirus scan, say), so a leftover .update-old is simply removed.
  ${IfNot} ${FileExists} "$INSTDIR\.update-swapping"
  ${AndIf} ${FileExists} "$INSTDIR\.update-old\*.*"
    RMDir /r "$INSTDIR\.update-old"
  ${EndIf}
  ${If} ${FileExists} "$INSTDIR\.update-swapping"
  ${AndIf} ${FileExists} "$INSTDIR\.update-old\*.*"
    DetailPrint "An interrupted update was found: restoring the previous install"
    CreateDirectory "$INSTDIR\.update-staging"
    Call RollbackSwap
    RMDir /r "$INSTDIR\.update-staging"
    ${If} $R1 != ""
      StrCpy $SetupResult "rollback of an interrupted update failed at $R1"
      StrCpy $R2 3
      Call FailInstall
    ${EndIf}
    RMDir /r "$INSTDIR\.update-old"
  ${EndIf}
  Delete "$INSTDIR\.update-swapping"

  ; The launcher needs the Microsoft Edge WebView2 Runtime (built into Windows 11, usually present on Windows 10).
  ; When it is missing, Microsoft's Evergreen bootstrapper (embedded) downloads and installs it.
  Call WebView2Present
  ${If} $WebView2Version == ""
    DetailPrint "Installing the Microsoft Edge WebView2 Runtime (needed by the GT4Recomp window)..."
    InitPluginsDir
    File "/oname=$PLUGINSDIR\MicrosoftEdgeWebview2Setup.exe" "${WEBVIEW2_BOOTSTRAPPER}"
    ExecWait '"$PLUGINSDIR\MicrosoftEdgeWebview2Setup.exe" /silent /install' $1
    DetailPrint "WebView2 bootstrapper exit code: $1"
    Call WebView2Present
    ${If} $WebView2Version == ""
      MessageBox MB_OK|MB_ICONEXCLAMATION "The Microsoft Edge WebView2 Runtime could not be installed (it needs an internet connection).$\r$\n$\r$\nGT4Recomp will be installed, but its window needs WebView2. Install it from https://go.microsoft.com/fwlink/p/?LinkId=2124703 and then start GT4Recomp." /SD IDOK
    ${Else}
      DetailPrint "WebView2 Runtime $WebView2Version installed."
    ${EndIf}
  ${Else}
    DetailPrint "WebView2 Runtime $WebView2Version found."
  ${EndIf}

  !insertmacro TimingMark "webview2"

  ; Stage (TASK-091): every program file is extracted into .update-staging first. The live files are not touched here.
  SetOutPath "$INSTDIR"
  RMDir /r "$INSTDIR\.update-staging"
  ClearErrors
  ; TASK-108 (1.3.0): the game programs are gt4.exe and gt4-spec2.exe. A 1.2.x install left gt4vulkan*.exe in game\;
  ; the update swaps the whole game folder, and a fresh copy never has them, so they are removed here as well.
  Delete "$INSTDIR\game\gt4vulkan.exe"
  Delete "$INSTDIR\game\gt4vulkan-spec2.exe"
  SetOutPath "$INSTDIR\.update-staging"
  File "/oname=GT4Recomp.exe" "${LAUNCHER_EXE}"
  File "/oname=SDL3.dll" "${SDL3_DLL}"
  File "/oname=NOTICE.md" "${__FILEDIR__}\..\NOTICE.md"
  File "/oname=LICENSE.txt" "${__FILEDIR__}\..\LICENSE"
  SetOutPath "$INSTDIR\.update-staging\game"
  File /r "${GAME_DIR}\*.*"
  Call CheckStagedFiles
  ${If} $R8 != ""
    StrCpy $SetupResult "extract failed"
    SetOutPath "$INSTDIR"
    RMDir /r "$INSTDIR\.update-staging"
    StrCpy $R2 4
    Call FailInstall
  ${EndIf}
  !insertmacro TimingMark "stage"

  ; Swap (TASK-091): each live item goes to .update-old and its staged copy takes its place. A failed rename rolls back.
  SetOutPath "$INSTDIR"
  CreateDirectory "$INSTDIR\.update-old"
  ; The marker says "a swap is in progress" until it completes; the next run uses it to tell a half swap from a done one.
  FileOpen $R6 "$INSTDIR\.update-swapping" w
  FileClose $R6
  Call SwapStagedIn
  ${If} $R8 != ""
    StrCpy $SetupResult "swap failed at $R8, rolled back"
    Call RollbackSwap
    ${If} $R1 != ""
      ; The marker stays: the next run restores .update-old first.
      StrCpy $SetupResult "swap failed at $R8, rollback failed at $R1"
    ${Else}
      RMDir /r "$INSTDIR\.update-old"
      Delete "$INSTDIR\.update-swapping"
    ${EndIf}
    RMDir /r "$INSTDIR\.update-staging"
    StrCpy $R2 3
    Call FailInstall
  ${EndIf}
  ; The swap is complete: the marker goes first. The previous copies and the staging folder go after the launcher
  ; starts (below). The data folder was never touched.
  Delete "$INSTDIR\.update-swapping"
  !insertmacro TimingMark "swap"

  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; version.json (docs/RELEASES.md): the installed GT4Recomp version and the settings schema. The launcher cannot write
  ; this program folder, so it keeps its own copy (with the gt4-gs version) in the data folder.
  FileOpen $0 "$INSTDIR\version.json" w
  FileWrite $0 '{"schema": 1, "gt4recomp": "${FULL_VERSION}", "gt4gs": null, "settings_schema": 1}$\r$\n'
  FileClose $0

  !insertmacro TimingMark "files"

  ; The large data folder (TASK-091, 1.2.1). A fresh install creates it and gives the local Users group Modify (inherited
  ; by its files), so the launcher can write it as any player. An update keeps that grant: it only checks, with a write
  ; probe, that the folder accepts a file, and runs icacls again only when the probe fails. The rest of the program folder
  ; stays protected.
  ${If} ${FileExists} "$INSTDIR\data\*.*"
    StrCpy $DataExisted 1
  ${Else}
    StrCpy $DataExisted 0
  ${EndIf}
  ${If} $DataExisted == 0
    CreateDirectory "$INSTDIR\data"
    Call GrantDataModify
  ${Else}
    Call ProbeDataWritable
    ${If} $R0 != "1"
      DetailPrint "The data folder does not accept files: its permissions are set again"
      Call GrantDataModify
    ${EndIf}
  ${EndIf}
  !insertmacro TimingMark "data"

  SetShellVarContext all

  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\GT4Recomp.exe" "" "$INSTDIR\GT4Recomp.exe" 0
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" "$INSTDIR\uninstall.exe"

  ; TASK-094 (1.2.3): a silent run creates the desktop shortcut, or makes it again, by the stored choice (see
  ; DesktopShortcutSilent). An interactive run decides on the finish page.
  ${If} ${Silent}
    Call DesktopShortcutSilent
  ${EndIf}

  WriteRegStr HKLM "${APP_KEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${FULL_VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "GT4Recomp contributors"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\GT4Recomp.exe"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" "$\"$INSTDIR\uninstall.exe$\" /S"
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
  !insertmacro TimingMark "shortcuts_registry"
  ; EstimatedSize (KiB): the program files plus the expected data (TASK-091, 1.2.1). The data folder is not walked: the
  ; game folder is summed with its subfolders, the top-level files without them (/G=0).
  ${GetSize} "$INSTDIR\game" "/S=0K" $0 $1 $2
  ${GetSize} "$INSTDIR" "/M=*.* /S=0K /G=0" $3 $1 $2
  IntOp $0 $0 + $3
  IntOp $0 $0 + ${EXPECTED_DATA_KB}
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "EstimatedSize" $0
  ; A reinstall into another folder: the previous data folder is recorded before the launcher starts (TASK-091).
  Call RecordPreviousDataDir
  !insertmacro TimingMark "estimated_size"

  ; The result is logged first. A silent self-update (/S /UPDATE, TASK-091) then starts the launcher here, as the player
  ; (Explorer), so the player gets the updated program back. Without /UPDATE, or when interactive, nothing changes.
  StrCpy $SetupResult "ok"
  Call LogSetupResult
  ${If} $UpdateRun == 1
  ${AndIf} ${Silent}
    Call StartLauncherAsPlayer
    !insertmacro TimingMark "relaunch_request"
  ${EndIf}

  ; After the start: the previous copies, the old per-user install and a previous program folder. None of these files is
  ; needed by the launcher that was just started; a failure here is only logged.
  RMDir /r "$INSTDIR\.update-old"
  RMDir /r "$INSTDIR\.update-staging"
  !insertmacro TimingMark "old_copies"
  SetShellVarContext current
  Call RemoveOldPerUserInstall
  !insertmacro TimingMark "old_user_install"
  Call RemovePreviousProgramFolder
  SetShellVarContext all
  !insertmacro TimingMark "previous_folder"
  Call LogTimings
SectionEnd

Section "Uninstall"
  SetRegView 64
  SetShellVarContext all
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"
  ; TASK-094 (1.2.3): the desktop shortcut of all users. Its choice (HKLM DesktopShortcut) goes with the app key below.
  Delete "$DESKTOP\${APP_NAME}.lnk"

  ; The program folder, its data folder included (the disc copy can be made again from the player's own image).
  RMDir /r "$INSTDIR\game"
  RMDir /r "$INSTDIR\data"
  RMDir /r "$INSTDIR\.update-staging"
  RMDir /r "$INSTDIR\.update-old"
  Delete "$INSTDIR\.update-swapping"
  Delete "$INSTDIR\GT4Recomp.exe"
  Delete "$INSTDIR\SDL3.dll"
  Delete "$INSTDIR\NOTICE.md"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\version.json"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  DeleteRegKey HKLM "${APP_KEY}"

  ; Per-user data stays unless the box was ticked, and then only for the Windows account running this uninstall: its
  ; %APPDATA%\GT4Recomp (settings, controls, saves, replays) and the 1.2.0 leftovers or write fallback in
  ; %LOCALAPPDATA%\GT4Recomp. Folders named "Gran Turismo 4..." (old save libraries) are kept even then.
  ${If} $DeleteDataState == ${BST_CHECKED}
    SetShellVarContext current
    RMDir /r "$APPDATA\GT4Recomp"
    StrCpy $R0 "$LOCALAPPDATA\GT4Recomp"
    FindFirst $R1 $R2 "$R0\*"
    ${DoWhile} $R2 != ""
      StrCpy $R3 $R2 14
      ${If} $R2 != "."
      ${AndIf} $R2 != ".."
      ${AndIf} $R3 != "Gran Turismo 4"
        ${If} ${FileExists} "$R0\$R2\*.*"
          RMDir /r "$R0\$R2"
        ${Else}
          Delete "$R0\$R2"
        ${EndIf}
      ${EndIf}
      FindNext $R1 $R2
    ${Loop}
    FindClose $R1
    RMDir "$R0"
  ${EndIf}
SectionEnd

Function un.DataPageCreate
  SetShellVarContext current
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}
  ${NSD_CreateLabel} 0 0 100% 52u "GT4Recomp is removed from $INSTDIR, with its data folder (the copy of your disc image, gt4-gs, logs and custom textures). Setup can copy your disc image again later.$\r$\n$\r$\nYour settings, saves and replays are in $APPDATA\GT4Recomp. They are kept unless you tick the box below."
  Pop $0
  ${NSD_CreateCheckbox} 0 60u 100% 24u "Also delete my settings, saves and replays. This acts only for the Windows user running this uninstall ($APPDATA\GT4Recomp); other users keep theirs."
  Pop $DeleteDataCheckbox
  ${NSD_SetState} $DeleteDataCheckbox ${BST_UNCHECKED}
  nsDialogs::Show
FunctionEnd

Function un.DataPageLeave
  ${NSD_GetState} $DeleteDataCheckbox $DeleteDataState
FunctionEnd
