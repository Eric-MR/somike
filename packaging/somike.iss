; Somike setup program (Inno Setup 6.3 or newer).
;
; Build it with scripts/build-installer.ps1, which passes:
;   /DAppVersion=0.2.0-beta.2      full version (with pre-release suffix)
;   /DAppVersionNumeric=0.2.0      numeric part, for the file version resource
;   /DPayloadDir=<dir>             output of `cmake --install`
;   /DOutputDir=<dir>              where Somike-Setup-<version>.exe is written
;   /DWebView2Bootstrapper=<file>  optional: MicrosoftEdgeWebview2Setup.exe,
;                                  run when the WebView2 Runtime is missing
;
; What it does beyond copying files:
; - installs to %ProgramFiles%\Somike, the directory somike-apoctl also uses
;   for the APO DLL, so audiodg.exe loads the DLL straight from the app folder;
; - fresh install: optionally connects Somike to the default microphone (the
;   setup program is elevated already, so no second Windows prompt);
; - update: the old APO DLL may be loaded by audiodg.exe; it is moved aside
;   and Windows Audio is restarted so the new version runs;
; - uninstall: removes Somike from every microphone first (restoring the
;   original effect configuration), then deletes the files.

#ifndef AppVersion
  #error Pass /DAppVersion=<version> (see scripts/build-installer.ps1)
#endif
#ifndef AppVersionNumeric
  #define AppVersionNumeric "0.0.0"
#endif
#define RepoRoot AddBackslash(SourcePath) + ".."
#ifndef PayloadDir
  #define PayloadDir RepoRoot + "\build\release\package"
#endif
#ifndef OutputDir
  #define OutputDir RepoRoot + "\build\installer"
#endif

[Setup]
AppId={{88DD7E1A-5478-441D-8444-9CAE45D20948}
AppName=Somike
AppVersion={#AppVersion}
AppVerName=Somike {#AppVersion}
AppPublisher=Somike contributors
AppPublisherURL=https://github.com/Eric-MR/somike
AppSupportURL=https://github.com/Eric-MR/somike/issues
AppUpdatesURL=https://github.com/Eric-MR/somike/releases
AppComments=Plays any app into your real microphone.
VersionInfoVersion={#AppVersionNumeric}
VersionInfoProductTextVersion={#AppVersion}
VersionInfoDescription=Somike setup
; The APO is a 64-bit x64 DLL loaded by audiodg.exe: ARM64 Windows runs a
; native ARM64 audio engine that cannot load it, so only x64 Windows is allowed.
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
; Per-app capture (process loopback) needs build 20348+: Windows 11 or
; Windows Server 2022 and newer.
MinVersion=10.0.20348
PrivilegesRequired=admin
DefaultDirName={autopf}\Somike
DisableDirPage=yes
DisableProgramGroupPage=yes
; Only our own programs: the APO DLL is handled in [Code], never by closing
; the Windows audio engine.
CloseApplications=force
CloseApplicationsFilter=*.exe
RestartApplications=no
OutputDir={#OutputDir}
OutputBaseFilename=Somike-Setup-{#AppVersion}
SetupIconFile={#RepoRoot}\src\app\somike.ico
UninstallDisplayIcon={app}\somike.exe
UninstallDisplayName=Somike
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
SetupLogging=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "de"; MessagesFile: "compiler:Languages\German.isl"

[CustomMessages]
en.MicGroup=Microphone:
en.SetupMicTask=Connect Somike to my default microphone now (sound pauses for a second)
en.InstallingWebView2=Installing the Microsoft Edge WebView2 Runtime...
en.ConnectingMic=Connecting Somike to your microphone...
en.RestartingAudio=Restarting Windows Audio so the new version runs...
en.AppComment=Play any app into your microphone
de.MicGroup=Mikrofon:
de.SetupMicTask=Somike jetzt mit meinem Standardmikrofon verbinden (der Ton setzt kurz aus)
de.InstallingWebView2=Microsoft Edge WebView2 Runtime wird installiert...
de.ConnectingMic=Somike wird mit deinem Mikrofon verbunden...
de.RestartingAudio=Windows-Audio wird neu gestartet, damit die neue Version läuft...
de.AppComment=Jede App in dein Mikrofon spielen

[Tasks]
Name: "setupmic"; Description: "{cm:SetupMicTask}"; GroupDescription: "{cm:MicGroup}"; Check: not ApoRegistered
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\somike.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\somike-apoctl.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\somike_apo.dll"; DestDir: "{app}"; Flags: ignoreversion uninsrestartdelete
Source: "{#PayloadDir}\somike-selftest.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
#ifdef WebView2Bootstrapper
Source: "{#WebView2Bootstrapper}"; DestDir: "{tmp}"; DestName: "MicrosoftEdgeWebview2Setup.exe"; Flags: deleteafterinstall; Check: not WebView2Installed
#endif

[Icons]
Name: "{autoprograms}\Somike"; Filename: "{app}\somike.exe"; Comment: "{cm:AppComment}"
Name: "{autodesktop}\Somike"; Filename: "{app}\somike.exe"; Comment: "{cm:AppComment}"; Tasks: desktopicon

[Run]
#ifdef WebView2Bootstrapper
Filename: "{tmp}\MicrosoftEdgeWebview2Setup.exe"; Parameters: "/silent /install"; StatusMsg: "{cm:InstallingWebView2}"; Flags: waituntilterminated; Check: not WebView2Installed
#endif
Filename: "{app}\somike-apoctl.exe"; Parameters: "install --restart-audio --report ""{app}\setup-mic.log"""; StatusMsg: "{cm:ConnectingMic}"; Flags: runhidden waituntilterminated; Tasks: setupmic
Filename: "{app}\somike.exe"; Description: "{cm:LaunchProgram,Somike}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Restores every microphone's original effect configuration, unregisters the
; APO and restarts Windows Audio so the DLL is no longer in use.
Filename: "{app}\somike-apoctl.exe"; Parameters: "uninstall --all --restart-audio"; RunOnceId: "RemoveSomikeApo"; Flags: runhidden waituntilterminated

[UninstallDelete]
Type: files; Name: "{app}\somike_apo.dll.old-*"
Type: files; Name: "{app}\setup-mic.log"
Type: filesandordirs; Name: "{commonappdata}\Somike"

[Code]
const
  ApoClsidKey = 'SOFTWARE\Classes\CLSID\{B5F655D4-FC22-4DDB-9240-494757B5BF74}';
  WebView2ClientKey = 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';
  MainWindowClass = 'Somike.MainWindow';
  WM_CLOSE = $0010;

var
  ApoDllReplaced: Boolean;

// Somike's APO is registered (by a previous version, the app or somike-apoctl).
function ApoRegistered: Boolean;
begin
  Result := RegKeyExists(HKLM64, ApoClsidKey);
end;

function WebView2VersionFound(RootKey: Integer): Boolean;
var
  Version: String;
begin
  Result := RegQueryStringValue(RootKey, WebView2ClientKey, 'pv', Version) and
            (Version <> '') and (Version <> '0.0.0.0');
end;

function WebView2Installed: Boolean;
begin
  Result := WebView2VersionFound(HKLM32) or WebView2VersionFound(HKCU);
end;

// Asks a running Somike window to close (it saves its settings) and waits.
procedure CloseRunningSomike;
var
  Tries: Integer;
  Wnd: HWND;
begin
  Wnd := FindWindowByClassName(MainWindowClass);
  if Wnd = 0 then
    Exit;
  Log('Closing the running Somike window');
  PostMessage(Wnd, WM_CLOSE, 0, 0);
  Tries := 0;
  while (FindWindowByClassName(MainWindowClass) <> 0) and (Tries < 50) do
  begin
    Sleep(100);
    Tries := Tries + 1;
  end;
  Sleep(500);  // let the process exit and release its files
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  CloseRunningSomike;
  Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Dll, Parked: String;
  ResultCode: Integer;
begin
  if CurStep = ssInstall then
  begin
    Dll := ExpandConstant('{app}\somike_apo.dll');
    if FileExists(Dll) and not DeleteFile(Dll) then
    begin
      // audiodg.exe has the old APO loaded: it cannot be overwritten, but it
      // can be renamed. Windows Audio is restarted after the copy.
      Parked := Dll + '.old-' + GetDateTimeString('yyyymmddhhnnss', #0, #0);
      if RenameFile(Dll, Parked) then
      begin
        ApoDllReplaced := True;
        Log('Moved the in-use APO DLL aside: ' + Parked);
      end
      else
        Log('Could not move the in-use APO DLL aside: ' + Dll);
    end;
  end
  else if CurStep = ssPostInstall then
  begin
    if ApoDllReplaced then
    begin
      // Loads the new APO and releases the old file.
      WizardForm.StatusLabel.Caption := CustomMessage('RestartingAudio');
      if not Exec(ExpandConstant('{app}\somike-apoctl.exe'), 'restart-audio', '', SW_HIDE,
                  ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then
        Log(Format('Restarting Windows Audio failed (exit code %d); the new APO loads after a reboot', [ResultCode]));
    end;
    // A file that is still locked stays until the next update or the uninstall.
    DelTree(ExpandConstant('{app}\somike_apo.dll.old-*'), False, True, False);
  end;
end;

function InitializeUninstall: Boolean;
begin
  CloseRunningSomike;
  Result := True;
end;
