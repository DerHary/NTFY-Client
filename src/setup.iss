#ifndef ClientExe
  #define ClientExe "..\..\.build\ntfy-client\NTFY-Client.exe"
#endif
#define AppName "NTFY Client"
#define AppVersion "0.2.4"
#define ExeName "NTFY-Client.exe"
#define Aumid "NtfyWindows.Client"
#define SettingsMutex "Local\NtfyWindows.Client.Settings"

[Setup]
AppId={{F46D174C-7293-46AD-A9CC-FBA4E202990B}
AppName={#AppName}
AppVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\NTFY-Client
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
MinVersion=10.0.19041
OutputDir=..\bin
OutputBaseFilename=NTFY-Client-Setup-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern dynamic
UninstallDisplayIcon={app}\{#ExeName}
CloseApplications=no
AppMutex={#SettingsMutex}
VersionInfoVersion={#AppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Dirs]
Name: "{app}\sounds"

[Files]
Source: "{#ClientExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "sounds\*.mp3"; DestDir: "{app}\sounds"; Flags: onlyifdoesntexist
Source: "sounds\*.wav"; DestDir: "{app}\sounds"; Flags: onlyifdoesntexist skipifsourcedoesntexist

[Icons]
Name: "{userprograms}\NTFY Client – Settings"; Filename: "{app}\{#ExeName}"; Parameters: "--settings"; WorkingDir: "{app}"; AppUserModelID: "{#Aumid}"; AppUserModelToastActivatorCLSID: "60E61915-7EFC-45D7-8F05-7236F9043113"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "NTFY Client"; ValueData: """{app}\{#ExeName}"" --receive"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\AppUserModelId\{#Aumid}"; ValueType: string; ValueName: "DisplayName"; ValueData: "{#AppName}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\ntfy-client"; ValueType: string; ValueData: "URL:{#AppName}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\ntfy-client"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\ntfy-client\shell\open\command"; ValueType: string; ValueData: """{app}\{#ExeName}"" --settings"

[Run]
Filename: "{app}\{#ExeName}"; Parameters: "--receive"; Flags: nowait
Filename: "{app}\{#ExeName}"; Parameters: "--settings"; Description: "Open NTFY Client Settings"; Flags: postinstall nowait skipifsilent

[Code]
const
  LegacyUninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{1E8207A7-AEE1-4DDD-AFC8-888D12A5CC42}_is1';

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

function RemoveLegacyInstallation(): Boolean;
var
  OldFolder, Uninstaller: String;
  Code: Integer;
begin
  Result := True;
  if not RegKeyExists(HKCU, LegacyUninstallKey) then
    exit;

  Result := False;
  if not RegQueryStringValue(HKCU, LegacyUninstallKey, 'InstallLocation', OldFolder) then
    exit;
  if not RegQueryStringValue(HKCU, LegacyUninstallKey, 'UninstallString', Uninstaller) then
    exit;
  Uninstaller := RemoveQuotes(Uninstaller);
  if not FileExists(Uninstaller) or
    (CompareText(AddBackslash(ExtractFileDir(Uninstaller)), AddBackslash(OldFolder)) <> 0) then
    exit;

  // The old uninstaller stops its receiver and preserves subscriptions in the user profile.
  if Exec(Uninstaller, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', OldFolder, SW_HIDE,
    ewWaitUntilTerminated, Code) and (Code = 0) then
    Result := not RegKeyExists(HKCU, LegacyUninstallKey);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not StopInstalledReceiver() then begin
    Result := 'Could not stop the NTFY Client receiver. Try again.';
    exit;
  end;
  if not RemoveLegacyInstallation() then
    Result := 'Could not remove the previous installation. Uninstall it under Installed apps and try again. Your subscriptions are kept.';
end;

function InitializeUninstall(): Boolean;
begin
  Result := not CheckForMutexes('{#SettingsMutex}');
  if not Result then begin
    SuppressibleMsgBox('Close NTFY Client Settings first.', mbError, MB_OK, IDOK);
    exit;
  end;
  Result := StopInstalledReceiver();
  if not Result then
    SuppressibleMsgBox('Could not stop the NTFY Client receiver.', mbError, MB_OK, IDOK);
end;
