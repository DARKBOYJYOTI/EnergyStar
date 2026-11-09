#ifndef AppArch
  #error AppArch must be provided by the build workflow.
#endif
#ifndef PayloadDir
  #error PayloadDir must be provided by the build workflow.
#endif
#ifndef OutputDir
  #error OutputDir must be provided by the build workflow.
#endif

[Setup]
AppId={{D45C1B91-22A8-4F2C-8B74-63F3EDC09D0E}
AppName=EnergyStar
AppVersion=2.0.0
AppPublisher=EnergyStar Community
DefaultDirName={autopf}\EnergyStar
UninstallDisplayIcon={app}\EnergyStar.exe
SetupIconFile=..\Assets\EnergyStar.ico
OutputDir={#OutputDir}
OutputBaseFilename=EnergyStar-Setup-win-{#AppArch}
Compression=lzma2/ultra64
SolidCompression=yes
PrivilegesRequired=admin
#if AppArch == "ARM64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif
WizardStyle=modern
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\EnergyStar.exe"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
Type: files; Name: "{app}\EnergyStar.installed"

[Icons]
Name: "{autoprograms}\EnergyStar"; Filename: "{app}\EnergyStar.exe"
Name: "{autodesktop}\EnergyStar"; Filename: "{app}\EnergyStar.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\EnergyStar.exe"; Parameters: "--install"; Flags: runhidden waituntilterminated
Filename: "{app}\EnergyStar.exe"; Description: "Launch EnergyStar"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{cmd}"; Parameters: "/c net stop EnergyStarService >nul 2>&1"; Flags: runhidden waituntilterminated
Filename: "{app}\EnergyStar.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveEnergyStarService"

[UninstallDelete]
Type: files; Name: "{app}\EnergyStar.Service.exe"
Type: files; Name: "{app}\EnergyStar.Agent.exe"
Type: files; Name: "{app}\EnergyStar.installed"

[Code]
procedure StopEnergyStarServiceAndProcesses();
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{cmd}'), '/c net stop EnergyStarService >nul 2>&1', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{cmd}'), '/c sc stop EnergyStarService >nul 2>&1', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{cmd}'), '/c taskkill /F /IM EnergyStar.exe >nul 2>&1', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(500);
end;

function InitializeSetup(): Boolean;
begin
  StopEnergyStarServiceAndProcesses();
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    StopEnergyStarServiceAndProcesses();
end;

function InitializeUninstall(): Boolean;
begin
  StopEnergyStarServiceAndProcesses();
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    StopEnergyStarServiceAndProcesses();
end;
