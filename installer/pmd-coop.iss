; Installer for the PMD co-op build of melonDS (host) + pmdcoop_client.
; Build:  "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" installer\pmd-coop.iss
; Output: dist\PMD-Coop-Setup.exe
;
; Per-user install without admin rights: melonDS is a portable build that stores its
; configuration next to the exe, so the install directory must be writable.
; The ROM is deliberately not part of the installer.

[Setup]
AppId={{6B6F3C0E-7C1D-4E0B-9B5E-PMDCOOP00001}
AppName=PMD Coop (melonDS)
AppVersion=1.1-coop4
AppPublisher=PMD Coop
DefaultDirName={localappdata}\Programs\PMD-Coop
DefaultGroupName=PMD Coop
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=PMD-Coop-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\res\melon.ico
UninstallDisplayIcon={app}\melonDS.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "german"; MessagesFile: "compiler:Languages\German.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Desktop-Verknüpfungen erstellen"; GroupDescription: "Verknüpfungen:"

[Files]
Source: "..\dist\pmd-coop\*"; DestDir: "{app}"; Excludes: "melonDS.toml,rtc.bin,*.sav,*.mln,client_id.txt"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\PMD Coop (melonDS)"; Filename: "{app}\melonDS.exe"; WorkingDir: "{app}"
Name: "{group}\PMD Coop - Host"; Filename: "{app}\start_host.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\melonDS.exe"; Flags: runminimized
Name: "{group}\PMD Coop - 2 Spieler lokal"; Filename: "{app}\start_lokal_2_spieler.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\melonDS.exe"; Flags: runminimized
Name: "{group}\PMD Coop - Beitreten (Client)"; Filename: "{app}\start_client.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\melonDS.exe"
Name: "{group}\Anleitung"; Filename: "{app}\LIESMICH.txt"
Name: "{group}\PMD Coop deinstallieren"; Filename: "{uninstallexe}"
Name: "{autodesktop}\PMD Coop (melonDS)"; Filename: "{app}\melonDS.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{autodesktop}\PMD Coop - Host"; Filename: "{app}\start_host.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\melonDS.exe"; Flags: runminimized; Tasks: desktopicon
Name: "{autodesktop}\PMD Coop - 2 Spieler lokal"; Filename: "{app}\start_lokal_2_spieler.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\melonDS.exe"; Flags: runminimized; Tasks: desktopicon
Name: "{autodesktop}\PMD Coop - Beitreten (Client)"; Filename: "{app}\start_client.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\melonDS.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\LIESMICH.txt"; Description: "Anleitung anzeigen"; Flags: postinstall shellexec skipifsilent unchecked
