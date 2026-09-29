; CelMonitor installer (Inno Setup 6). Built by windows\installer\build-installer.ps1, which stages build\dist first.
; Everything a new user needs in one Setup.exe: app, drivers, certificate trust, ADB download, Android app.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef DistDir
  #define DistDir "..\..\build\dist"
#endif

[Setup]
AppId={{2F6C6B1D-7E8A-4C2B-9E61-0C5D8B3A4F12}
AppName=CelMonitor
AppVersion={#AppVersion}
AppPublisher=CelMonitor
AppPublisherURL=https://github.com/Morosetti/CelMonitor
DefaultDirName={autopf}\CelMonitor
DefaultGroupName=CelMonitor
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
OutputDir=..\..\build\installer
OutputBaseFilename=CelMonitor-Setup-{#AppVersion}
SetupIconFile=..\host\res\CelMonitor.ico
UninstallDisplayIcon={app}\CelMonitor.exe
UninstallDisplayName=CelMonitor
InfoBeforeFile=LEIA-ANTES.txt
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; We close CelMonitor ourselves (CelMonitor.exe --exit) so the virtual monitor is removed cleanly.
CloseApplications=no

[Languages]
Name: "ptbr"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Tasks]
Name: "autostart"; Description: "Iniciar o CelMonitor junto com o Windows (fica na bandeja e conecta sozinho)"
Name: "desktopicon"; Description: "Criar atalho na área de trabalho"; Flags: unchecked

[Files]
Source: "{#DistDir}\CelMonitor.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\celmon-cli.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\CelMonitor.apk"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\drivers\*"; DestDir: "{app}\drivers"; Flags: ignoreversion recursesubdirs
Source: "{#DistDir}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist

[Icons]
Name: "{autoprograms}\CelMonitor"; Filename: "{app}\CelMonitor.exe"
Name: "{autodesktop}\CelMonitor"; Filename: "{app}\CelMonitor.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\CelMonitor.exe"; Description: "Abrir o CelMonitor agora"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallDelete]
Type: filesandordirs; Name: "{app}\adb"

[Code]
var
  DownloadPage: TDownloadWizardPage;
  AdbDownloaded: Boolean;
  RestartNeeded: Boolean;

function OnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  Result := True;
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage('Baixando o ADB', 'Baixando o Android SDK Platform-Tools do site do Google...', @OnDownloadProgress);
end;

// The ADB is downloaded from Google (its license does not allow bundling it here).
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpReady then begin
    DownloadPage.Clear;
    DownloadPage.Add('https://dl.google.com/android/repository/platform-tools-latest-windows.zip', 'platform-tools.zip', '');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
        AdbDownloaded := True;
      except
        AdbDownloaded := False;
        SuppressibleMsgBox('Não foi possível baixar o ADB agora: ' + GetExceptionMessage + #13#10#13#10 +
          'O CelMonitor será instalado mesmo assim. Sem o ADB ele não consegue falar com o celular; ' +
          'execute este instalador de novo quando houver internet.', mbInformation, MB_OK, IDOK);
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;

// Upgrades: close the running CelMonitor gracefully (it removes the virtual monitor first).
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Result := '';
  if FileExists(ExpandConstant('{app}\CelMonitor.exe')) then
    Exec(ExpandConstant('{app}\CelMonitor.exe'), '--exit', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

procedure InstallAdb;
var
  Code: Integer;
  Src, Dst: String;
begin
  Src := ExpandConstant('{tmp}\pt');
  Dst := ExpandConstant('{app}\adb');
  Exec('powershell.exe', '-NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath ''' +
       ExpandConstant('{tmp}\platform-tools.zip') + ''' -DestinationPath ''' + Src + ''' -Force"', '', SW_HIDE, ewWaitUntilTerminated, Code);
  ForceDirectories(Dst);
  if not (FileCopy(Src + '\platform-tools\adb.exe', Dst + '\adb.exe', False) and
          FileCopy(Src + '\platform-tools\AdbWinApi.dll', Dst + '\AdbWinApi.dll', False) and
          FileCopy(Src + '\platform-tools\AdbWinUsbApi.dll', Dst + '\AdbWinUsbApi.dll', False)) then
    SuppressibleMsgBox('O ADB foi baixado, mas não pôde ser extraído. Execute o instalador novamente.', mbError, MB_OK, IDOK);
  FileCopy(Src + '\platform-tools\NOTICE.txt', Dst + '\NOTICE.txt', False);
end;

procedure InstallDrivers;
var
  Code: Integer;
begin
  WizardForm.StatusLabel.Caption := 'Instalando os drivers do monitor virtual e do USB...';
  if not Exec(ExpandConstant('{app}\celmon-cli.exe'), 'setup-drivers "' + ExpandConstant('{app}\drivers') + '"', '',
              SW_HIDE, ewWaitUntilTerminated, Code) then
    Code := -1;
  if Code = 3010 then
    RestartNeeded := True
  else if Code <> 0 then
    SuppressibleMsgBox('A instalação dos drivers falhou (código ' + IntToStr(Code) + ').' + #13#10 +
      'O CelMonitor mostrará o problema ao abrir. Tente executar o instalador novamente.', mbError, MB_OK, IDOK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if CurStep = ssPostInstall then begin
    if AdbDownloaded then InstallAdb;
    InstallDrivers;
    // "Start with Windows" belongs to the user who is installing, not to the elevated admin account.
    if WizardIsTaskSelected('autostart') then
      ExecAsOriginalUser(ExpandConstant('{app}\CelMonitor.exe'), '--autostart-on', '', SW_HIDE, ewWaitUntilTerminated, Code)
    else
      ExecAsOriginalUser(ExpandConstant('{app}\CelMonitor.exe'), '--autostart-off', '', SW_HIDE, ewWaitUntilTerminated, Code);
  end;
end;

function NeedRestart: Boolean;
begin
  Result := RestartNeeded;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Code: Integer;
begin
  if CurUninstallStep = usUninstall then begin
    Exec(ExpandConstant('{app}\CelMonitor.exe'), '--exit', '', SW_HIDE, ewWaitUntilTerminated, Code);
    Exec(ExpandConstant('{app}\CelMonitor.exe'), '--autostart-off', '', SW_HIDE, ewWaitUntilTerminated, Code);
    Exec(ExpandConstant('{app}\celmon-cli.exe'), 'remove-drivers --remove-cert', '', SW_HIDE, ewWaitUntilTerminated, Code);
  end;
end;
