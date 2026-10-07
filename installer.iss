; AirGlass installer (Inno Setup 6). Built by package.ps1:
;   ISCC /DAppVersion=1.0.0 installer.iss  ->  dist\AirGlass-Setup-<version>.exe
; Per-user install (no admin); one elevation prompt adds the inbound firewall rule.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
; File version info must be numeric: "1.3.0-beta.1" -> "1.3.0".
#define Dash Pos("-", AppVersion)
#if Dash > 0
  #define NumVersion Copy(AppVersion, 1, Dash - 1)
#else
  #define NumVersion AppVersion
#endif
#define AppName "AirGlass"
#define AppExe "AirGlass.exe"
#define RuleName "AirGlass (AirPlay receiver)"

[Setup]
AppId={{6C0F8E4B-2B7A-4E8C-9C1D-A1A55A1A55A1}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=AirGlass
VersionInfoVersion={#NumVersion}
DefaultDirName={localappdata}\Programs\{#AppName}
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=dist
OutputBaseFilename=AirGlass-Setup-{#AppVersion}
SetupIconFile=assets\AirGlass.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
LicenseFile=LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=no

[Tasks]
Name: "autostart"; Description: "Start AirGlass with Windows (recommended, so devices can always find it)"
Name: "firewall"; Description: "Allow AirGlass through Windows Firewall (asks for permission once)"

[Files]
Source: "build\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\avcodec-*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\avutil-*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\swresample-*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#FfmpegDir}\LICENSE.txt"; DestDir: "{app}"; DestName: "FFmpeg-LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; Comment: "AirPlay screen mirroring and lossless music receiver"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppExe}"" --background"; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{cmd}"; Parameters: "/c netsh advfirewall firewall delete rule name=""{#RuleName}"" >nul & netsh advfirewall firewall add rule name=""{#RuleName}"" dir=in action=allow program=""{app}\{#AppExe}"" profile=any enable=yes"; Verb: "runas"; Flags: shellexec runhidden waituntilterminated; Tasks: firewall; StatusMsg: "Adding the firewall rule..."
Filename: "{app}\{#AppExe}"; Description: "Start AirGlass now"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\{#AppExe}"; Parameters: "--quit"; Flags: runhidden waituntilterminated; RunOnceId: "QuitApp"
Filename: "{cmd}"; Parameters: "/c netsh advfirewall firewall delete rule name=""{#RuleName}"""; Verb: "runas"; Flags: shellexec runhidden waituntilterminated; RunOnceId: "RemoveFirewallRule"

[Code]
// Quit a running copy gracefully (it sends its mDNS goodbye) before files are replaced.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Exe: String;
  Code: Integer;
begin
  Exe := ExpandConstant('{app}\{#AppExe}');
  if FileExists(Exe) then begin
    Exec(Exe, '--quit', '', SW_HIDE, ewWaitUntilTerminated, Code);
    // Force-stop only a copy running from this folder (not a dev build elsewhere).
    Exec('powershell.exe', '-NoProfile -Command "Get-Process AirGlass -ErrorAction SilentlyContinue | ' +
      'Where-Object { $_.Path -eq ''' + Exe + ''' } | Stop-Process -Force"', '', SW_HIDE, ewWaitUntilTerminated, Code);
  end;
  Result := '';
end;
