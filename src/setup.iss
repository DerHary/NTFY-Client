#ifndef ClientExe
  #define ClientExe "..\..\.build\ntfy-client\NtfyWindows.exe"
#endif
#define AppName "ntfy Windows Client"
#define AppVersion "0.1.0"
#define ExeName "NtfyWindows.exe"
#define Aumid "NtfyWindows.Client"

[Setup]
AppId={{1E8207A7-AEE1-4DDD-AFC8-888D12A5CC42}
AppName={#AppName}
AppVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\NtfyWindows
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
MinVersion=10.0.19041
OutputDir=..\bin
OutputBaseFilename=NtfyWindows-Setup-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern dynamic
UninstallDisplayIcon={app}\{#ExeName}
CloseApplications=no
AppMutex=Local\NtfyWindows.Client.Settings
VersionInfoVersion={#AppVersion}

[Languages]
Name: "de"; MessagesFile: "compiler:Languages\German.isl"

[Files]
Source: "{#ClientExe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\ntfy – Einstellungen"; Filename: "{app}\{#ExeName}"; Parameters: "--settings"; WorkingDir: "{app}"; AppUserModelID: "{#Aumid}"; AppUserModelToastActivatorCLSID: "60E61915-7EFC-45D7-8F05-7236F9043113"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "NtfyWindows"; ValueData: """{app}\{#ExeName}"" --receive"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\AppUserModelId\{#Aumid}"; ValueType: string; ValueName: "DisplayName"; ValueData: "ntfy"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\ntfywindows-client"; ValueType: string; ValueData: "URL:ntfy Windows Client"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\ntfywindows-client"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\ntfywindows-client\shell\open\command"; ValueType: string; ValueData: """{app}\{#ExeName}"" --settings"

[Run]
Filename: "{app}\{#ExeName}"; Parameters: "--receive"; Flags: nowait
Filename: "{app}\{#ExeName}"; Parameters: "--settings"; Description: "ntfy-Einstellungen öffnen"; Flags: postinstall nowait skipifsilent

[Code]
function StopInstalledReceiver(): Boolean;
var
  ExePath: String;
  Code: Integer;
begin
  ExePath := ExpandConstant('{app}\{#ExeName}');
  if not FileExists(ExePath) then begin
    Result := True;
    exit;
  end;
  Result := Exec(ExePath, '--stop', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not StopInstalledReceiver() then
    Result := 'Der ntfy-Empfänger konnte nicht beendet werden. Bitte erneut versuchen.';
end;

function InitializeUninstall(): Boolean;
begin
  Result := not CheckForMutexes('Local\NtfyWindows.Client.Settings');
  if not Result then begin
    SuppressibleMsgBox('Bitte zuerst die ntfy-Einstellungen schließen.', mbError, MB_OK, IDOK);
    exit;
  end;
  Result := StopInstalledReceiver();
  if not Result then
    SuppressibleMsgBox('Der ntfy-Empfänger konnte nicht beendet werden.', mbError, MB_OK, IDOK);
end;
