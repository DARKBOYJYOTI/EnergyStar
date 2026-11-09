#ifndef AppArch
  #error AppArch must be provided by the build workflow.
#endif
#ifndef PayloadDir
  #error PayloadDir must be provided by the build workflow.
#endif
#ifndef OutputDir
  #error OutputDir must be provided by the build workflow.
#endif

#define AppName "EnergyStar"
#define AppVersion "1.0.0"
#define AppPublisher "EnergyStar Community"

[Setup]
AppId={{B7168C59-3670-4C38-9A89-0D71BDB4D1A2}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\EnergyStar
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\EnergyStar.exe
SetupIconFile=..\Assets\EnergyStar.ico
OutputDir={#OutputDir}
OutputBaseFilename=EnergyStar-Setup-{#AppArch}
Compression=lzma2/ultra64
SolidCompression=yes
PrivilegesRequired=admin
#if AppArch == "win-arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif
WizardStyle=modern
DisableProgramGroupPage=yes
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\EnergyStar.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\EnergyStar.Core.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\EnergyStar"; Filename: "{app}\EnergyStar.exe"; IconFilename: "{app}\EnergyStar.exe"
Name: "{autodesktop}\EnergyStar"; Filename: "{app}\EnergyStar.exe"; IconFilename: "{app}\EnergyStar.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\EnergyStar.exe"; Description: "Launch EnergyStar"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\schtasks.exe"; Parameters: "/Delete /TN ""\EnergyStar"" /F"; Flags: runhidden; RunOnceId: "RemoveEnergyStarTask"
