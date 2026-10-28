#!/usr/bin/env bash
# 选择带 MetalFX 帧生成头文件的 Xcode，并把 DEVELOPER_DIR 写入 GitHub Actions 环境。

set -euo pipefail
required_header='System/Library/Frameworks/MetalFX.framework/Headers/MTLFXFrameInterpolator.h'
selected=''
while read -r version bundle; do
    if [[ ( -f "${bundle}/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/${required_header}" ) ||
        ( -f "${bundle}/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk/${required_header}" ) ]]; then
        selected="${bundle}"
        echo "使用 ${bundle}（Xcode ${version}）：SDK 含 MetalFX 帧生成"
        break
    fi
done < <(for bundle in /Applications/Xcode*.app; do
    version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
        "${bundle}/Contents/Info.plist" 2>/dev/null || echo '0.0')
    printf '%s %s\n' "${version}" "${bundle}"
done | sort -Vr)
if [[ -z "$selected" ]]; then
    echo "::error::没有找到带 MTLFXFrameInterpolator.h 的 Xcode，MetalFX 帧生成需要 26 版 SDK"
    exit 1
fi

if [[ -n "${GITHUB_ENV:-}" ]]; then
    echo "DEVELOPER_DIR=${selected}/Contents/Developer" >> "$GITHUB_ENV"
else
    echo "DEVELOPER_DIR=${selected}/Contents/Developer"
fi
