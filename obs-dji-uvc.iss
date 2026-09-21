; obs-dji-uvc.iss — Inno Setup installer for obs-dji-uvc (Windows x64).
;
; Build:  iscc installer\obs-dji-uvc.iss
; Stage first with installer\stage.ps1 (copies DLLs into installer\staging).

#define AppName "obs-dji-uvc"
#define AppVersion "1.3.1"

[Setup]
AppId={{7B1E7A61-DDA2-4C55-9B3A-obs-dji-uvc}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=OniGiri Production
DefaultDirName={code:GetObsDir}
DisableDirPage=no
DirExistsWarning=no
OutputBaseFilename=obs-dji-uvc-setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern

[Files]
Source: "staging\obs-dji-uvc.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "staging\libusb-1.0.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion skipifsourcedoesntexist
Source: "staging\data\*"; DestDir: "{app}\data\obs-plugins\obs-dji-uvc"; Flags: ignoreversion recursesubdirs

[Code]
function GetObsDir(Param: string): string;
var
  v: string;
begin
  { OBS install path from the registry; fall back to the default. }
  if RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', v) then
    Result := v
  else if RegQueryStringValue(HKLM32, 'SOFTWARE\OBS Studio', '', v) then
    Result := v
  else
    Result := ExpandConstant('{autopf}\obs-studio');
end;

function ObsRunning(): Boolean;
var
  ec: Integer;
begin
  Exec(ExpandConstant('{cmd}'), '/C tasklist /FI "IMAGENAME eq obs64.exe" | find /I "obs64.exe" > nul',
       '', SW_HIDE, ewWaitUntilTerminated, ec);
  Result := (ec = 0);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if ObsRunning() then begin
    MsgBox('OBS Studio is running. Close it before installing.', mbError, MB_OK);
    Result := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    MsgBox('Installed.' + #13#10 + #13#10 +
           'REQUIRED ONE-TIME STEP PER CAMERA:' + #13#10 +
           '1. Put the Pocket in Webcam mode and plug it in.' + #13#10 +
           '2. Run Zadig (zadig.akeo.ie), Options > List All Devices.' + #13#10 +
           '3. Select the DJI camera VIDEO interface (VID 2CA3).' + #13#10 +
           '4. Replace the driver with WinUSB.' + #13#10 + #13#10 +
           'The camera will no longer appear as a normal Windows webcam ' +
           '(reversible in Device Manager > Uninstall device > rescan).',
           mbInformation, MB_OK);
  end;
end;
