; This Source Code Form is subject to the terms of the Mozilla Public
; License, v. 2.0. If a copy of the MPL was not distributed with this
; file, You can obtain one at https://mozilla.org/MPL/2.0/.
;
; TorFox for Windows: the installer (Inno Setup 7). It installs TorFox for the
; current user only, in %LOCALAPPDATA%\Programs\TorFox, and never asks for
; administrator rights, neither to install nor for updates.
;
; Built by scripts/package_windows.py, which stages the release build and
; defines:
;   AppVersion   X.Y.Z (the CMake project version)
;   SourceDir    the staged program folder
;   OutputDir    where the setup is written
;   Compression  optional; "lzma2/max" when not given
;   TestPerUser  optional, installer tests only: the same setup under another
;                AppId and name, so that a test never touches a real
;                installation. Such a setup is never published.
;
; Updates: TorFox runs the setup after it has shut down with
;   /SILENT /SUPPRESSMSGBOXES /NORESTART /TORFOXUPDATE
; The setup waits for TorFox to exit (PrepareToInstall), installs, and starts
; TorFox again.

#ifndef AppVersion
  #error AppVersion is not defined (build with scripts/package_windows.py)
#endif
#ifndef SourceDir
  #error SourceDir is not defined (build with scripts/package_windows.py)
#endif
#ifndef OutputDir
  #error OutputDir is not defined (build with scripts/package_windows.py)
#endif
#ifndef Compression
  #define Compression "lzma2/max"
#endif

#ifdef TestPerUser
  #define MyAppId "{{a843f419-af80-4bad-b40a-b971e3ea64b1}"
  #define MyAppName "TorFox Installer Test"
  #define MySetupName "TorFox-" + AppVersion + "-win-x64-setup-peruser-test"
#else
  #define MyAppId "{{96a198f9-9688-44af-aa68-192fb7cadebc}"
  #define MyAppName "TorFox"
  #define MySetupName "TorFox-" + AppVersion + "-win-x64-setup"
#endif

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#AppVersion}
AppVerName={#MyAppName} {#AppVersion}
AppPublisher=The TorFox developers
AppPublisherURL=https://github.com/hamburger101/TorFox
AppSupportURL=https://github.com/hamburger101/TorFox
AppUpdatesURL=https://github.com/hamburger101/TorFox/releases
VersionInfoVersion={#AppVersion}.0
VersionInfoProductName=TorFox
VersionInfoDescription=TorFox Setup
DefaultDirName={autopf}\{#MyAppName}
DisableProgramGroupPage=yes
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\TorFox.exe
OutputDir={#OutputDir}
OutputBaseFilename={#MySetupName}
Compression={#Compression}
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; For the current user only: {autopf} is %LOCALAPPDATA%\Programs, the
; shortcuts and the uninstall entry are the user's own (HKEY_CURRENT_USER).
PrivilegesRequired=lowest
WizardStyle=modern
; Files still in use after TorFox has closed (a slow child process) are
; found by the Restart Manager; TorFox itself is waited for in [Code].
CloseApplications=yes
RestartApplications=no
SetupMutex={#MyAppName}SetupRunning
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; Optional files an earlier version may have installed but this one may not
; ship (scripts/package_windows.py OPTIONAL_FILES): an update must not leave
; a stale copy next to newer CEF files. [Files] replaces everything else.
Type: files; Name: "{app}\dxil.dll"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\TorFox.exe"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\TorFox.exe"; Tasks: desktopicon

[Registry]
; TorFox writes HKCU\Software\TorFox only to ask an update in progress to
; start it afterwards ([Code]); nothing is created here, the uninstaller
; removes the key.
Root: HKCU; Subkey: "Software\TorFox"; Flags: dontcreatekey uninsdeletekey

[Run]
; An interactive install offers to start TorFox. An update starts it again
; after "Relaunch to update" (or if TorFox was opened while it installed).
; Setup never asks for elevation, so TorFox runs as the user who started
; Setup (runasoriginaluser stays as a second guard).
Filename: "{app}\TorFox.exe"; Description: "{cm:LaunchProgram,TorFox}"; Flags: nowait postinstall skipifsilent runasoriginaluser
Filename: "{app}\TorFox.exe"; Flags: nowait runasoriginaluser; Check: StartAfterUpdate; BeforeInstall: UpdateFinished

[Code]
const
  RunningMutex = 'TorFoxBrowserRunning';
  UpdateWaitSeconds = 60;
  // Held while an update installs: a TorFox started meanwhile does not run
  // (it would hold files this setup replaces); it records a request instead,
  // and this setup starts TorFox when it has finished
  // (src/browser/platform/platform_win.cc).
  InstallingMutex = 'TorFoxUpdateInstalling';
  // TorFox holds the mutex itself from before it shuts down until this
  // setup sets this event, once it holds the mutex too: no TorFox can start
  // in between (LaunchUpdate in platform_win.cc).
  StartedEvent = 'TorFoxUpdateStarted';
  EVENT_MODIFY_STATE = $0002;
  TorFoxKey = 'Software\TorFox';
  StartRequestValue = 'RelaunchAfterUpdate';

var
  InstallingHandle: NativeInt;
  TorFoxStarted: Boolean;

function CreateMutex(Attributes: NativeInt; InitialOwner: BOOL; Name: String): NativeInt;
external 'CreateMutexW@kernel32.dll stdcall';

function CloseHandle(Handle: NativeInt): BOOL;
external 'CloseHandle@kernel32.dll stdcall';

function OpenEvent(DesiredAccess: DWORD; InheritHandle: BOOL; Name: String): NativeInt;
external 'OpenEventW@kernel32.dll stdcall';

function SetEvent(Handle: NativeInt): BOOL;
external 'SetEvent@kernel32.dll stdcall';

function HasParam(const Name: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Name) = 0 then
      Result := True;
end;

function IsUpdate: Boolean;
begin
  Result := HasParam('/TORFOXUPDATE');
end;

// After "Relaunch to update" TorFox starts again; after TorFox was closed
// with an update waiting (/TORFOXNORELAUNCH), only if it was opened while
// the update installed.
function StartAfterUpdate: Boolean;
begin
  Result := IsUpdate and (not HasParam('/TORFOXNORELAUNCH') or
                          RegValueExists(HKCU, TorFoxKey, StartRequestValue));
end;

procedure ReleaseInstalling;
begin
  if InstallingHandle <> 0 then
  begin
    CloseHandle(InstallingHandle);
    InstallingHandle := 0;
  end;
end;

// Just before the [Run] entry starts TorFox.
procedure UpdateFinished;
begin
  ReleaseInstalling;
  RegDeleteValue(HKCU, TorFoxKey, StartRequestValue);
  TorFoxStarted := True;
end;

function InitializeSetup: Boolean;
var
  Started: NativeInt;
begin
  Result := True;
  if IsUpdate then
  begin
    InstallingHandle := CreateMutex(0, False, InstallingMutex);
    Log('Update: holding ' + InstallingMutex);
    // TorFox may stop waiting now. (No event: this setup was started some
    // other way.)
    Started := OpenEvent(EVENT_MODIFY_STATE, False, StartedEvent);
    if Started <> 0 then
    begin
      SetEvent(Started);
      CloseHandle(Started);
      Log('Update: told TorFox');
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  // The files are in place: TorFox may start from here on.
  if CurStep = ssPostInstall then
    ReleaseInstalling;
end;

procedure DeinitializeSetup;
var
  Code: Integer;
begin
  ReleaseInstalling;
  // The setup did not get as far as starting TorFox (it failed, or the
  // request came after its [Run] entry was checked), but should have: after
  // "Relaunch to update", or because TorFox was opened while the update was
  // being installed. Start it now, installed or not; a TorFox that was not
  // updated says so when it starts (update::CheckHandoff).
  if not TorFoxStarted and StartAfterUpdate then
  begin
    RegDeleteValue(HKCU, TorFoxKey, StartRequestValue);
    try
      ExecAsOriginalUser(ExpandConstant('{app}\TorFox.exe'), '', '', SW_SHOWNORMAL,
                         ewNoWait, Code);
    except
      Log('Could not start TorFox: ' + GetExceptionMessage);
    end;
  end;
end;

// A WQL string literal matching paths that start with Dir (LIKE wildcards
// and the literal's own escapes are escaped).
function WqlPrefix(const Dir: String): String;
var
  I: Integer;
  C: String;
begin
  Result := '';
  for I := 1 to Length(Dir) do
  begin
    C := Copy(Dir, I, 1);
    if C = '\' then
      C := '\\'
    else if C = '''' then
      C := '\'''
    else if (C = '[') or (C = '_') or (C = '%') then
      C := '[' + C + ']';
    Result := Result + C;
  end;
  Result := '''' + Result + '%''';
end;

// Any process (TorFox, its helpers, tor) started from the program folder.
// The uninstaller is left out: its first copy (unins000.exe) runs from the
// program folder while its second copy, from the temporary folder, works.
function ProgramFolderInUse(const Dir: String): Boolean;
var
  Locator, Service, Items: Variant;
begin
  Result := False;
  if (Dir = '') or not DirExists(Dir) then
    Exit;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Items := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE ExecutablePath LIKE ' +
                               WqlPrefix(AddBackslash(Dir)) + ' AND NOT Name LIKE ''unins%''');
    Result := Items.Count > 0;
  except
    // WMI unavailable: the mutex and the Restart Manager still apply.
    Log('Could not list processes: ' + GetExceptionMessage);
  end;
end;

function TorFoxRunning(const Dir: String): Boolean;
begin
  Result := CheckForMutexes(RunningMutex) or ProgramFolderInUse(Dir);
end;

function WaitForTorFox(const Dir: String; Seconds: Integer): Boolean;
var
  I: Integer;
begin
  for I := 1 to Seconds * 4 do
  begin
    if not TorFoxRunning(Dir) then
    begin
      Result := True;
      Exit;
    end;
    Sleep(250);
  end;
  Result := not TorFoxRunning(Dir);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Dir: String;
begin
  Result := '';
  Dir := ExpandConstant('{app}');
  if IsUpdate then
  begin
    Log('Update: waiting for TorFox to exit');
    if not WaitForTorFox(Dir, UpdateWaitSeconds) then
      Result := 'TorFox did not close, so the update was not installed. Close TorFox, then install the update again.';
    Exit;
  end;
  while TorFoxRunning(Dir) do
  begin
    if WizardSilent then
    begin
      Result := 'TorFox is running. Close TorFox, then run Setup again.';
      Exit;
    end;
    if MsgBox('TorFox is running. Close every TorFox window, then click OK to continue.',
              mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := 'TorFox is running. Close TorFox, then run Setup again.';
      Exit;
    end;
  end;
end;

function InitializeUninstall: Boolean;
var
  Dir: String;
begin
  Result := True;
  Dir := ExpandConstant('{app}');
  while TorFoxRunning(Dir) do
  begin
    if UninstallSilent then
    begin
      Result := False;
      Exit;
    end;
    if MsgBox('TorFox is running. Close every TorFox window, then click OK to continue.',
              mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
  end;
end;
