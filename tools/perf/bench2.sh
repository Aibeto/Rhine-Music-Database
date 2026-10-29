#!/bin/bash
# bench2.sh <backend> <fg> <tag> [hold_seconds]
#
# 全屏 2560x1440、主界面列表页（收藏专辑 98 条）、长按 ↓ 取区间平均帧率。
# 轮次产出：$BENCH_OUT/<tag>.log（带时间戳）与 <tag>.png（长按后截图）。
#
# 环境变量：
#   BENCH_OUT    输出目录，默认 /tmp/mb
#   EXTRA_ENV    追加给被测程序的环境变量，例如 EXTRA_ENV="RHINE_DRAW_PROBE=1"
#   WARMUP       热身秒数，默认 5
#
# 注意：被测程序按**相对路径**找播放服务，必须在仓库根目录用 ./output/qt-build/... 启动。
set -u
backend=$1; fg=$2; tag=$3; hold=${4:-20}
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=${BENCH_OUT:-/tmp/mb}
log=$out/$tag.log
sess=~/.local/share/RhineMusicDatabase/rhine.music.database/session.json
mkdir -p "$out"

pkill -f "qt-build/RhineMusicDataba[s]" 2>/dev/null
sleep 2
# 每轮注入固定起始状态：程序退出时会回写 session.json，不重置就会状态漂移。
[ -f "$out/session.baseline.json" ] && cp "$out/session.baseline.json" "$sess"

cd "$repo" || exit 1
env QT_ASSUME_STDERR_HAS_CONSOLE=1 \
    RHINE_GRAPHICS="$backend" \
    RHINE_FG="$fg" \
    RHINE_FG_DIAG=1 \
    RHINE_PACE_DIAG=1 \
    RHINE_KEY_PROBE=1 \
    RHINE_RESOURCES="$repo/output/resources" ${EXTRA_ENV:-} \
    ./output/qt-build/RhineMusicDatabase 2>&1 | python3 -u "$(dirname "$0")/ts.py" >"$log" 2>/dev/null &
pipepid=$!

for _ in $(seq 1 160); do grep -q "节拍诊断" "$log" 2>/dev/null && break; sleep 0.5; done
sleep "${WARMUP:-5}"

apid=$(pgrep -f "qt-build/RhineMusicDataba[s]" | head -1)
[ -z "${apid:-}" ] && { echo "FATAL $tag: 进程未起来"; exit 1; }
wid=$(niri msg windows 2>/dev/null | awk -v p="$apid" '/^Window ID /{id=$3;sub(":","",id)} /PID: /{if($2==p){print id;exit}}')
[ -z "${wid:-}" ] && { echo "FATAL $tag: 无窗口"; pkill -f "qt-build/RhineMusicDataba[s]"; exit 1; }

niri msg action focus-window --id "$wid" >/dev/null 2>&1
sleep 1

# ESC 关掉启动时的设置弹窗回主界面 → 右方向键切到「收藏专辑」→ F11 全屏。
# 收藏歌单(COLLECTION 03) 在账号校验失败时是空的，长按 ↓ 没有可选项，所以用 04。
python3 "$(dirname "$0")/press.py" 1 106 87 >/dev/null 2>&1
sleep 2

start_ms=$(date +%s%3N)
python3 "$(dirname "$0")/holdkey.py" "$hold" 108 >/dev/null 2>&1
end_ms=$(date +%s%3N)
sleep 1
grim -o DP-1 "$out/$tag.png" 2>/dev/null
kp=$(grep -c "按键到达输入层" "$log" 2>/dev/null || echo 0)
pkill -f "qt-build/RhineMusicDataba[s]" 2>/dev/null
sleep 1
kill $pipepid 2>/dev/null

dur=$(python3 -c "print(f'{($end_ms-$start_ms)/1000:.1f}')")
printf '%s\tbackend=%s\tfg=%s\thold=%ss\tkeys=%s\n' "$tag" "$backend" "$fg" "$dur" "$kp"
