#!/usr/bin/env python3
"""验证部署后的 Qt 应用，不读取用户凭据或访问网易云。"""
import json
import plistlib
import re
import pathlib
import subprocess
import sys

app = pathlib.Path(sys.argv[1]).resolve()
mac = sys.platform == "darwin"
resources = app / ("Contents/Resources" if mac else "resources")
executable = app / ("Contents/MacOS/RhineMusicDatabase" if mac else "RHINE MUSIC DATABASE.exe")
assert executable.is_file()
assert not re.search(
    rb"cc\.[a-z]+\.rhine\.(?:native-panel|music)", executable.read_bytes()
), "Legacy runtime namespace"
for name in [
    "runtime.js",
    "qt-logic.js",
    "slots.json",
    "scene-fixture.json",
    "room-pmrem.bin",
    "misans-glyphs.json",
    "LICENSE",
    "NOTICE",
]:
    assert (resources / name).is_file(), name
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
    for binary in (app / "Contents/PlugIns").rglob("*.dylib"):
        dependencies = subprocess.check_output(["otool", "-L", str(binary)], text=True)
        assert not any(
            "/Users/" in line or "/Applications/Postgres.app/" in line or "/opt/homebrew/" in line
            for line in dependencies.splitlines()
            if line.startswith("\t")
        ), str(binary)
else:
    assert (app / "Qt6Quick.dll").is_file() or (app / "Qt6Quickd.dll").is_file()
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
