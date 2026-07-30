#ifndef AppName
#define AppName "OBS Plugin"
#endif

#ifndef AppVersion
#define AppVersion "1.0.0"
#endif

#ifndef SourceDir
#define SourceDir "..\..\release\RelWithDebInfo"
#endif

#ifndef OutputDir
#define OutputDir "..\..\release"
#endif

#ifndef OutputBaseFilename
#define OutputBaseFilename "OBS-Plugin-Installer"
#endif

[Setup]
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=OBS Community
DefaultDirName={code:GetOBSPath}
DefaultGroupName={#AppName}
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
LZMADictionarySize=1048576
LZMANumBlockThreads=6
DirExistsWarning=no
DisableProgramGroupPage=yes
DisableReadyPage=yes
DisableWelcomePage=no
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64

[Files]
Source: "{#SourceDir}\{#AppName}\bin\64bit\*"; Excludes: "*.pdb, *.zip, *.exe"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\{#AppName}\data\*"; DestDir: "{app}\data\obs-plugins\{#AppName}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
var
  OBSPath: String;

function GetOBSPath(Param: String): String;
begin
  if Length(OBSPath) = 0 then
  begin
    // Attempt to read the OBS Studio installation path from the registry (Uninstall key)
    if RegQueryStringValue(HKLM64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio_is1', 'InstallLocation', OBSPath) then
    begin
      // Found the 64-bit OBS installation
    end
    else if RegQueryStringValue(HKLM32, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio_is1', 'InstallLocation', OBSPath) then
    begin
      // Found the 32-bit OBS installation
    end
    else
    begin
      // Fallback to the default installation directory
      OBSPath := ExpandConstant('{pf}\obs-studio');
    end;
  end;
  Result := OBSPath;
end;

procedure InitializeWizard;
begin
  // Initialize the OBSPath
  GetOBSPath('');
end;
