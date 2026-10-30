; dock - Inno Setup installer script
;
; Produces a single, clean setup.exe (Start-menu shortcut, optional desktop icon
; and start-with-Windows, proper uninstaller). Requires Inno Setup 6.3+ (it uses
; the "x64compatible" architecture identifier).
;
; The CI passes the version and reads the freshly built exe from ..\dist:
;   ISCC /DMyAppVersion=1.2.3 packaging\dock.iss
;
; To build it locally, first copy build\<config>\dock.exe into a "dist" folder
; at the repo root (or pass /DStagingDir=...), then run ISCC on this file.

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif

; Folder (relative to this .iss) that holds the freshly built dock.exe.
#ifndef StagingDir
  #define StagingDir "..\dist"
#endif

#define MyAppName "dock"
#define MyAppPublisher "PaPi"
#define MyAppURL "https://github.com/Mesrine67/dock"
#define MyAppExeName "dock.exe"

[Setup]
; A stable AppId keeps upgrades and uninstall tidy across versions. Do not change it.
AppId={{B3E6C0E2-1D7A-4D2C-9C3E-6A1F0D5A77C4}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=Output
OutputBaseFilename=dock-{#MyAppVersion}-setup
SetupIconFile=..\src\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; No AppMutex: the app's in-app updater launches this installer silently while it
; is still running, and a held AppMutex would make a /SILENT install abort. The
; ssInstall taskkill below stops any running instance before files are copied.

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "Start {#MyAppName} automatically when I sign in"; GroupDescription: "Startup:"

[Files]
Source: "{#StagingDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Optional "run at sign-in" - writes the very same HKCU Run value dock itself uses.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; \
  ValueName: "dock"; ValueData: """{app}\{#MyAppExeName}"""; \
  Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; \
  Flags: nowait postinstall skipifsilent
; After an unattended (in-app auto-update) install, relaunch the app ourselves,
; as the regular user so it never ends up running elevated. "--updated" makes a
; launch that collides with the app's own relauncher exit quietly.
Filename: "{app}\{#MyAppExeName}"; Parameters: "--updated"; Flags: nowait runascurrentuser; Check: WizardSilent

[UninstallRun]
; Stop the tray app (if running) before removing its files.
Filename: "{sys}\taskkill.exe"; Parameters: "/IM {#MyAppExeName} /F"; \
  Flags: runhidden; RunOnceId: "StopDock"

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  // Make sure no running instance locks dock.exe during a (re)install.
  if CurStep = ssInstall then
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM {#MyAppExeName} /F', '',
         SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  // Remove the autostart entry on uninstall, whoever created it (installer or the
  // app's own "start with Windows" setting), so no dead Run key is left behind.
  if CurUninstallStep = usUninstall then
    RegDeleteValue(HKEY_CURRENT_USER,
      'Software\Microsoft\Windows\CurrentVersion\Run', 'dock');
end;
