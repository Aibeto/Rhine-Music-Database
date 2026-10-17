#!/usr/bin/env python3
"""验证部署后的 Qt 应用，不读取用户凭据或访问网易云。"""
import json
import plistlib
import re
import pathlib
import subprocess
import sys

app = pathlib.Path(sys.argv[1]).resolve()
# 根据产物类型选择静态审计规则，允许在构建主机检查另一平台的部署目录。
mac = app.suffix.lower() == ".app"
# AppDir 是 AppImage 的前缀目录布局，程序与运行时都按 usr/ 前缀存放。
linux = (app / "usr/bin/RhineMusicDatabase").is_file()
if mac:
    resources = app / "Contents/Resources"
    executable = app / "Contents/MacOS/RhineMusicDatabase"
elif linux:
    resources = app / "usr/bin/resources"
    executable = app / "usr/bin/RhineMusicDatabase"
else:
    resources = app / "resources"
    executable = app / "RHINE MUSIC DATABASE.exe"
assert executable.is_file()
for pattern in (
    "diagnose-windows-audio*",
    "windows-audio-snapshot.ps1",
    "AudioRouteProbe*",
    "RhinePlatformRegression*",
):
    assert not list(app.rglob(pattern)), "Local diagnostic or regression tool entered release package"
assert not re.search(
    rb"cc\.[a-z]+\.rhine\.(?:native-panel|music)", executable.read_bytes()
), "Legacy runtime namespace"
for name in [
    "slots.json",
    "scene-fixture.json",
    "room-pmrem.bin",
    "misans-glyphs.json",
    "LICENSE",
    "NOTICE",
]:
    assert (resources / name).is_file(), name
for name in (
    "runtime.js",
    "timeline.js",
    "qt-logic.js",
    "archive-cassette-geometry.json",
    "archive-assembly-geometry.json",
):
    assert not (resources / name).exists(), "Retired script or geometry copy entered package: " + name
fixture = json.loads((resources / "scene-fixture.json").read_text(encoding="utf-8"))
assert (
    len(fixture["frame"]["view"]) == 16 and len(fixture["frame"]["projection"]) == 16 and fixture["rays"]
), "Invalid offline scene fixture"
if mac:
    with (app / "Contents/Info.plist").open("rb") as file:
        identity = plistlib.load(file)
    assert identity["CFBundleIdentifier"] == "rhine.music.database", "Wrong system application identity"
    assert identity["CFBundleExecutable"] == "RhineMusicDatabase"
    libraries = subprocess.check_output(["otool", "-L", str(executable)], text=True)
    assert "QtQuick" in libraries and "QtQml" in libraries
    assert "libswift" not in libraries.lower(), "Swift runtime dependency found"
    assert not any(p.suffix == ".swift" for p in app.rglob("*") if p.is_file())
    assert not any(p.name in {"node_modules", ".git", ".DS_Store"} for p in app.iterdir())
    assert not any(
        "/Users/" in line or "/opt/homebrew/" in line for line in libraries.splitlines()[1:]
    ), "Undeployed Qt dependency"
    load_commands = subprocess.check_output(["otool", "-l", str(executable)], text=True)
    search_paths = [line.strip() for line in load_commands.splitlines() if line.strip().startswith("path ")]
    assert not any(
        "/Users/" in line or "/opt/homebrew/" in line for line in search_paths
    ), "Local binary search path"
    conf = (resources / "qt.conf").read_text(encoding="utf-8")
    assert "/Users/" not in conf and "/opt/homebrew/" not in conf, "Local QML search path"
    assert not (app / "Contents/PlugIns/sqldrivers").exists(), "Unneeded database plugins"
    assert (
        app / "Contents/PlugIns/multimedia/libffmpegmediaplugin.dylib"
    ).is_file(), "Missing FFmpeg backend"
    for library in ("avcodec", "avformat", "avutil", "swresample", "swscale"):
        assert list((app / "Contents/Frameworks").glob(f"lib{library}.*.dylib")), f"Missing {library} runtime"
    audio_libraries = list((app / "Contents/Frameworks").glob("lib*.dylib"))
    for binary in list((app / "Contents/PlugIns").rglob("*.dylib")) + audio_libraries:
        dependencies = subprocess.check_output(["otool", "-L", str(binary)], text=True)
        assert not any(
            "/Users/" in line or "/Applications/Postgres.app/" in line or "/opt/homebrew/" in line
            for line in dependencies.splitlines()
            if line.startswith("\t")
        ), str(binary)
elif linux:
    libraries = subprocess.check_output(["ldd", str(executable)], text=True)
    assert "libQt6Core" in libraries and "libQt6Quick" in libraries, "Missing Qt runtime"
    assert "not found" not in libraries, "Undeployed library dependency"
    # 只允许引用随包 Qt 与系统图形栈，不得回落到构建机安装前缀。
    bundled = app / "usr/lib"
    assert (bundled / "libQt6Core.so.6").is_file(), "Qt runtime is not bundled"
    for pattern in ("libQt6Gui.so.6", "libQt6Quick.so.6", "libQt6Qml.so.6", "libQt6Multimedia.so.6"):
        assert (bundled / pattern).is_file(), "Missing bundled Qt module: " + pattern
    for line in libraries.splitlines():
        if "=> /" in line:
            # ldd 保留 RUNPATH 里的 $ORIGIN，比较前先归一化到真实路径。
            target = pathlib.Path(line.split("=> ")[1].split(" (")[0]).resolve()
            name = target.name
            if name.startswith("libQt6"):
                assert target.parent == bundled.resolve(), "Qt library resolved outside package: " + name
    assert not list(bundled.rglob("libQt6WebEngine*")), "Unused Qt module entered the package"
    assert (app / "usr/plugins/platforms/libqxcb.so").is_file(), "Missing X11 platform plugin"
    assert (app / "usr/plugins/platforms/libqwayland.so").is_file(), "Missing Wayland platform plugin"
    assert (
        app / "usr/plugins/platforms/libqoffscreen.so"
    ).is_file(), "Missing offscreen test platform plugin"
    assert (app / "usr/plugins/multimedia/libffmpegmediaplugin.so").is_file(), "Missing FFmpeg audio backend"
    assert (app / "usr/plugins/tls/libqopensslbackend.so").is_file(), "Missing TLS backend"
    assert (app / "usr/qml/QtQuick").is_dir(), "Missing QtQuick QML module"
    assert (resources / "MusicBridge/node").is_file(), "Missing bundled Node runtime"
    conf = (app / "usr/bin/qt.conf").read_text(encoding="utf-8")
    assert "/Users/" not in conf and "/opt/" not in conf, "Local QML search path"
    assert (app / "AppRun").is_file() and (app / "rhine-music-database.desktop").is_file()
    assert not list(app.rglob("*.swift"))
    assert not list(app.rglob("sqldrivers"))
else:
    debug = (app / "Qt6Cored.dll").is_file()
    suffix = "d.dll" if debug else ".dll"
    for name in ("msvcp140", "vcruntime140", "vcruntime140_1"):
        assert (app / (name + suffix)).is_file(), "Missing local MSVC runtime: " + name
    assert not list(app.glob("vc_redist*.exe")), "Portable package must not require a runtime installer"
    assert (app / "Qt6Quick.dll").is_file() or (app / "Qt6Quickd.dll").is_file()
    assert (app / "Qt6Multimedia.dll").is_file() or (
        app / "Qt6Multimediad.dll"
    ).is_file(), "Missing Qt audio runtime"
    assert (app / "multimedia/windowsmediaplugin.dll").is_file() or (
        app / "multimedia/windowsmediaplugind.dll"
    ).is_file(), "Missing Windows native compatibility audio backend"
    assert (app / "multimedia/ffmpegmediaplugin.dll").is_file() or (
        app / "multimedia/ffmpegmediaplugind.dll"
    ).is_file(), "Missing default FFmpeg audio backend"
    for library in ("avcodec", "avformat", "avutil", "swresample", "swscale"):
        assert list(app.glob(library + "-*.dll")), "Missing FFmpeg runtime: " + library
    assert (app / "platforms/qwindows.dll").is_file() or (app / "platforms/qwindowsd.dll").is_file()
    assert (app / "platforms/qoffscreen.dll").is_file() or (
        app / "platforms/qoffscreend.dll"
    ).is_file(), "Missing offscreen test platform plugin"
    assert (resources / "MusicBridge/node.exe").is_file()
    assert (app / "qml").is_dir()
    assert not list(app.rglob("*.swift"))
    assert not (app / "sqldrivers").exists()
result = {"qt_deployed": True, "swift_runtime_required": False, "locked_music_bridge": True}
pathlib.Path("output/qt-package-audit.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps(result))
