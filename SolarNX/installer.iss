#define MyAppName "SolarNX"
#ifndef MyAppVersion
  ; Single source of truth for the version: ../VERSION (also read by SolarNX.csproj and Client/Makefile).
  #define VersionFile FileOpen(AddBackslash(SourcePath) + "..\VERSION")
  #define MyAppVersion Trim(FileRead(VersionFile))
  #expr FileClose(VersionFile)
#endif
#define MyAppPublisher "SolarNX"
#define MyAppExeName "SolarNX.exe"
#define MyPublishDir "publish\win-x64"

[Setup]
; New AppId for the SolarNX name - the old OrbitNX one ({6E7B6E1B-...}) made Inno Setup reuse that
; install's folder and Start-menu group. The previous OrbitNX install is migrated in [Code] below.
AppId={{9433503E-E468-4701-9A29-DBEC1EB345A1}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=Setup
OutputBaseFilename=SolarNX-Setup
SetupIconFile=Assets\icon.ico
LicenseFile=..\LICENSE
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
CloseApplications=yes
RestartApplications=yes
CloseApplicationsFilter=*.exe

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
; Texts of the driver page and the messages in [Code] - one line per language, same names.
russian.DriverPageTitle=Драйвер геймпада
english.DriverPageTitle=Gamepad driver
russian.DriverPageDescription=ViGEmBus нужен для эмуляции геймпада
english.DriverPageDescription=ViGEmBus is needed to emulate the gamepad
russian.DriverInstalled=Драйвер геймпада (ViGEmBus) — уже установлен
english.DriverInstalled=Gamepad driver (ViGEmBus) — already installed
russian.DriverMissing=Драйвер геймпада (ViGEmBus) — не найден
english.DriverMissing=Gamepad driver (ViGEmBus) — not found
russian.DriverLeave=Не трогать
english.DriverLeave=Leave as is
russian.DriverRepair=Восстановить/переустановить
english.DriverRepair=Repair/reinstall
russian.DriverInstall=Установить
english.DriverInstall=Install
russian.DriverRemove=Удалить
english.DriverRemove=Remove
russian.DriverName=драйвер геймпада (ViGEmBus)
english.DriverName=the gamepad driver (ViGEmBus)
russian.DriverInstallFailed=Не удалось запустить установку драйвера геймпада.
english.DriverInstallFailed=Could not start the gamepad driver installation.
russian.FileNotFound=Не найден: %1
english.FileNotFound=Not found: %1
russian.UninstallerNotFound=Не удалось найти установщик для удаления "%1". Удалите вручную через Параметры Windows -> Приложения.
english.UninstallerNotFound=Could not find the uninstaller for "%1". Remove it by hand in Windows Settings -> Apps.
russian.UninstallFailed=Не удалось запустить удаление: %1
english.UninstallFailed=Could not start the removal: %1

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[InstallDelete]
; focushook.dll (Steam focus-spoofing hook) was removed from the app - drop the copy older versions
; installed. Silently skipped if a still-running Steam from an old session has it loaded.
Type: files; Name: "{app}\focushook.dll"

[Files]
Source: "{#MyPublishDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Code]
// Audio capture no longer needs a driver at all (see AudioCaptureSender - native WASAPI loopback),
// so this page only covers the gamepad driver now. Was a two-driver page (audio + input) before the
// audio pipeline rewrite dropped the virtual-audio-capturer dependency entirely.
var
  DriverPage: TWizardPage;
  InputGroup: TPanel;
  InputHeader: TNewStaticText;
  InputInstalled: Boolean;
  InputNone, InputAction, InputRemove: TNewRadioButton;

// ViGEmBus registers a kernel driver service under this key regardless of installer version -
// far more reliable than matching against its uninstall registry entry's display name/GUID.
function DetectInputDriver(): Boolean;
begin
  Result := RegKeyExists(HKLM, 'SYSTEM\CurrentControlSet\Services\ViGEmBus');
end;

function FindUninstallStringIn(const RootPath, NameSubstring: String): String;
var
  Names: TArrayOfString;
  I: Integer;
  DisplayName, UninstallStr, SubKey: String;
begin
  Result := '';
  if not RegGetSubkeyNames(HKLM, RootPath, Names) then Exit;
  for I := 0 to GetArrayLength(Names) - 1 do
  begin
    SubKey := RootPath + '\' + Names[I];
    if RegQueryStringValue(HKLM, SubKey, 'DisplayName', DisplayName) then
    begin
      if Pos(Lowercase(NameSubstring), Lowercase(DisplayName)) > 0 then
      begin
        if RegQueryStringValue(HKLM, SubKey, 'UninstallString', UninstallStr) then
        begin
          Result := UninstallStr;
          Exit;
        end;
      end;
    end;
  end;
end;

function FindUninstallString(const NameSubstring: String): String;
begin
  Result := FindUninstallStringIn('SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', NameSubstring);
  if Result = '' then
    Result := FindUninstallStringIn('SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall', NameSubstring);
end;

procedure ExecCommandLine(const CmdLine, FailureMessage: String);
var
  exe, params: String;
  p, resultCode: Integer;
begin
  if (Length(CmdLine) > 0) and (CmdLine[1] = '"') then
  begin
    p := Pos('"', Copy(CmdLine, 2, Length(CmdLine) - 1));
    if p > 0 then
    begin
      exe := Copy(CmdLine, 2, p - 1);
      params := Trim(Copy(CmdLine, p + 2, Length(CmdLine)));
    end
    else
    begin
      exe := CmdLine;
      params := '';
    end;
  end
  else
  begin
    p := Pos(' ', CmdLine);
    if p > 0 then
    begin
      exe := Copy(CmdLine, 1, p - 1);
      params := Trim(Copy(CmdLine, p + 1, Length(CmdLine)));
    end
    else
    begin
      exe := CmdLine;
      params := '';
    end;
  end;
  if not Exec(exe, params, '', SW_SHOWNORMAL, ewWaitUntilTerminated, resultCode) then
    MsgBox(FailureMessage, mbError, MB_OK);
end;

procedure RunGuiInstaller(const ExePath, FailureMessage: String);
var
  resultCode: Integer;
begin
  if not FileExists(ExePath) then
  begin
    MsgBox(FmtMessage(CustomMessage('FileNotFound'), [ExePath]), mbError, MB_OK);
    Exit;
  end;
  if not Exec(ExePath, '', '', SW_SHOWNORMAL, ewWaitUntilTerminated, resultCode) then
    MsgBox(FailureMessage, mbError, MB_OK);
end;

procedure RunUninstall(const NameSubstring, Description: String);
var
  uninstallStr: String;
begin
  uninstallStr := FindUninstallString(NameSubstring);
  if uninstallStr = '' then
  begin
    MsgBox(FmtMessage(CustomMessage('UninstallerNotFound'), [Description]), mbInformation, MB_OK);
    Exit;
  end;
  ExecCommandLine(uninstallStr, FmtMessage(CustomMessage('UninstallFailed'), [Description]));
end;

function MakeRadio(Parent: TWinControl; ATop: Integer; ACaption: String): TNewRadioButton;
begin
  Result := TNewRadioButton.Create(Parent);
  Result.Parent := Parent;
  Result.Left := 12;
  Result.Top := ATop;
  Result.Width := 380;
  Result.Caption := ACaption;
end;

procedure CreateDriverPage;
var
  inputActionCaption: String;
begin
  DriverPage := CreateCustomPage(wpSelectTasks, CustomMessage('DriverPageTitle'), CustomMessage('DriverPageDescription'));

  InputInstalled := DetectInputDriver();

  InputHeader := TNewStaticText.Create(DriverPage);
  InputHeader.Parent := DriverPage.Surface;
  InputHeader.Left := 0;
  InputHeader.Top := 0;
  InputHeader.AutoSize := True;
  InputHeader.Font.Style := [fsBold];
  if InputInstalled then
    InputHeader.Caption := CustomMessage('DriverInstalled')
  else
    InputHeader.Caption := CustomMessage('DriverMissing');

  InputGroup := TPanel.Create(DriverPage);
  InputGroup.Parent := DriverPage.Surface;
  InputGroup.Left := 0;
  InputGroup.Top := InputHeader.Top + InputHeader.Height + 6;
  InputGroup.Width := DriverPage.SurfaceWidth;
  InputGroup.Height := 78;
  InputGroup.BevelOuter := bvNone;

  InputNone := MakeRadio(InputGroup, 0, CustomMessage('DriverLeave'));
  InputNone.Checked := True;

  if InputInstalled then inputActionCaption := CustomMessage('DriverRepair')
  else inputActionCaption := CustomMessage('DriverInstall');
  InputAction := MakeRadio(InputGroup, 22, inputActionCaption);

  InputRemove := MakeRadio(InputGroup, 44, CustomMessage('DriverRemove'));
  InputRemove.Visible := InputInstalled;
  InputRemove.Enabled := InputInstalled;
end;

// --- Migration from the app's previous name, OrbitNX (different AppId, possibly another folder). ---
const
  OrbitNxUninstallKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{6E7B6E1B-6E0B-4C9E-9D3B-5F6C6E7B6E1B}_is1';

function FindOrbitNxInstall(var AppDir, UninstallExe: String): Boolean;
begin
  Result := RegQueryStringValue(HKLM, OrbitNxUninstallKey, 'Inno Setup: App Path', AppDir) and
            RegQueryStringValue(HKLM, OrbitNxUninstallKey, 'UninstallString', UninstallExe);
  if Result then
  begin
    AppDir := RemoveBackslashUnlessRoot(AppDir);
    UninstallExe := RemoveQuotes(UninstallExe);
  end;
end;

// The user's settings, library and covers live next to the exe - carry them over unless the new
// folder already has its own.
procedure CopyUserData(const FromDir, ToDir: String);
var
  resultCode: Integer;
begin
  if FileExists(FromDir + '\config.json') and not FileExists(ToDir + '\config.json') then
    CopyFile(FromDir + '\config.json', ToDir + '\config.json', True);
  if FileExists(FromDir + '\library.json') and not FileExists(ToDir + '\library.json') then
    CopyFile(FromDir + '\library.json', ToDir + '\library.json', True);
  if DirExists(FromDir + '\covers') and not DirExists(ToDir + '\covers') then
    Exec(ExpandConstant('{sys}\robocopy.exe'), '"' + FromDir + '\covers" "' + ToDir + '\covers" /E /NFL /NDL /NJH /NJS',
         '', SW_HIDE, ewWaitUntilTerminated, resultCode);
end;

procedure MigrateFromOrbitNx;
var
  oldDir, uninstallExe, newDir: String;
  resultCode: Integer;
begin
  if not FindOrbitNxInstall(oldDir, uninstallExe) then Exit;
  newDir := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  // Installed over the old folder: its uninstaller would delete the files just installed.
  if CompareText(oldDir, newDir) = 0 then Exit;

  CopyUserData(oldDir, newDir);
  Exec('taskkill.exe', '/IM OrbitNX.exe /F', '', SW_HIDE, ewWaitUntilTerminated, resultCode);
  if FileExists(uninstallExe) then
    Exec(uninstallExe, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, resultCode);
  // The uninstaller leaves the files it didn't install (config, library, covers, logs) - they are
  // copied already, so remove exactly those, and the folder only if nothing else is left in it.
  DeleteFile(oldDir + '\config.json');
  DeleteFile(oldDir + '\library.json');
  DeleteFile(oldDir + '\OrbitNX.log');
  DeleteFile(oldDir + '\touchhelper.log');
  if DirExists(oldDir + '\covers') then
    DelTree(oldDir + '\covers', True, True, True);
  RemoveDir(oldDir);
end;

procedure InitializeWizard;
begin
  CreateDriverPage;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  appDir: String;
begin
  if CurStep = ssPostInstall then
  begin
    appDir := ExpandConstant('{app}');
    MigrateFromOrbitNx;

    if InputAction.Checked then
      RunGuiInstaller(appDir + '\vigembus-setup.exe', CustomMessage('DriverInstallFailed'));
    if InputRemove.Checked then
      RunUninstall('ViGEmBus', CustomMessage('DriverName'));
  end;
end;
