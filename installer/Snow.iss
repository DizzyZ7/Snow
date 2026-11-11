; Inno Setup 6.3+; tools/Package-Snow.ps1 supplies the build paths and version.
#ifndef SnowVersion
  #define SnowVersion "0.2.0"
#endif
#ifndef SnowNumericVersion
  #define SnowNumericVersion "0.2.0.0"
#endif
#ifndef SnowSourceExe
  #define SnowSourceExe SourcePath + "..\build\Release\Snow.exe"
#endif
#ifndef SnowReadme
  #define SnowReadme SourcePath + "..\README.md"
#endif

[Setup]
AppId={{61D7B285-E397-4D1F-9A7F-923E867F9532}
AppName=Snow
AppVersion={#SnowVersion}
VersionInfoVersion={#SnowNumericVersion}
DefaultDirName={localappdata}\Programs\Snow
DefaultGroupName=Snow
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.15063
UninstallDisplayIcon={app}\Snow.exe
OutputDir=..\dist
OutputBaseFilename=Snow-{#SnowVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
Name: "startmenuicon"; Description: "Create a Start menu shortcut"; Flags: unchecked

[Files]
Source: "{#SnowSourceExe}"; DestDir: "{app}"; DestName: "Snow.exe"; Flags: ignoreversion
Source: "{#SnowReadme}"; DestDir: "{app}"; DestName: "README.md"; Flags: ignoreversion

; Keep README's relative help links usable in the installed application folder.
Source: "{#SourcePath}..\docs\*.md"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{userdesktop}\Snow"; Filename: "{app}\Snow.exe"; Tasks: desktopicon
Name: "{userprograms}\Snow\Snow"; Filename: "{app}\Snow.exe"; Tasks: startmenuicon

[Run]
Filename: "{app}\Snow.exe"; Description: "Launch Snow"; Flags: nowait postinstall skipifsilent unchecked

[Code]
var
  DeletePreferences: Boolean;

function InitializeUninstall(): Boolean;
begin
  DeletePreferences := False;
  if not UninstallSilent then
    DeletePreferences := MsgBox(
      'Also remove Snow preferences from this user account?' + #13#10 +
      'Choose No to keep settings for a later installation.',
      mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  RunValue: String;
begin
  if CurUninstallStep = usUninstall then begin
    { Remove only the autostart value owned by this installation, if the user enabled it. }
    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Snow', RunValue) then
      if CompareText(RunValue, '"' + ExpandConstant('{app}\Snow.exe') + '"') = 0 then
        RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Snow');
  end;
  if (CurUninstallStep = usPostUninstall) and DeletePreferences then begin
    { Do not recursively delete a user directory or unrelated files. }
    DeleteFile(ExpandConstant('{localappdata}\Snow\settings.ini'));
    RemoveDir(ExpandConstant('{localappdata}\Snow'));
  end;
end;
