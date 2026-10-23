; 安装源由打包脚本审计并暂存，不包含用户数据。
#ifndef AppSource
    #error AppSource is required; run npm run share
#endif
#ifndef AppVersion
    #error AppVersion is required
#endif

[Setup]
AppId=rhine.music.database
AppName=RHINE MUSIC DATABASE
AppVersion={#AppVersion}
AppVerName=RHINE MUSIC DATABASE {#AppVersion}
AppPublisher=RhineMusicDatabase
AppPublisherURL=https://github.com/MikeQu223/Rhine-Music-Database
DefaultDirName={localappdata}\Programs\RhineMusicDatabase
DefaultGroupName=RHINE MUSIC DATABASE
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
DisableProgramGroupPage=yes
SetupIconFile=..\Assets\icons\AppIcon.ico
UninstallDisplayIcon={app}\RHINE MUSIC DATABASE.exe
Compression=lzma2
SolidCompression=yes
CloseApplications=yes
RestartApplications=no
Uninstallable=yes

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#AppSource}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\RHINE MUSIC DATABASE"; Filename: "{app}\RHINE MUSIC DATABASE.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\RHINE MUSIC DATABASE"; Filename: "{app}\RHINE MUSIC DATABASE.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\RHINE MUSIC DATABASE.exe"; Description: "Launch RHINE MUSIC DATABASE"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

; data 由应用创建，不列入安装或卸载清单，升级与卸载均保留账号和设置。
