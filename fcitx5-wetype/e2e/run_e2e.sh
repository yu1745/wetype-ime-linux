#!/bin/bash
# fcitx5-wetype 真机 e2e — 私有 dbus(前台) + 两阶段 profile
T=$(mktemp -d /tmp/wetype-e2e.XXXXXX)
trap 'rm -rf "$T"' EXIT
BASE="$(cd "$(dirname "$0")/../.." && pwd)"
ENGD="$BASE/squashfs-root/usr/lib/wetype-ime/arm64"
if [ ! -d "$ENGD" ]; then echo "缺引擎目录: 先 bash scripts/e2_img.sh && ./WeTypeIME-Engine-$(uname -m).AppImage --appimage-extract"; exit 9; fi
MODE="${1:-test}"

rm -rf "$T/config" "$T/data" "$T/bus"
mkdir -p "$T/config/fcitx5" "$T/data" "$T/bus"

dbus-daemon --session --fork --print-address=3 --print-pid=4 3>"$T/bus/addr" 4>"$T/bus/pid"
for i in $(seq 1 30); do [ -s "$T/bus/addr" ] && break; sleep 0.1; done
BUS_ADDR=$(cat "$T/bus/addr")
BUS_PID=$(cat "$T/bus/pid")
echo "BUS_ADDR=$BUS_ADDR"
export DBUS_SESSION_BUS_ADDRESS="$BUS_ADDR"
export XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data"
export XDG_DATA_DIRS="/usr/local/share:/usr/share"
export WETYPE_ENGINE_DIR="$ENGD"
unset FCITX_ADDON_DIRS

# 阶段1: 生成 5.1.7 格式 profile
fcitx5 --disable=classicui --disable=xim --disable=waylandim > "$T/fcitx5_s1.log" 2>&1 &
P1=$!
sleep 3
kill $P1 2>/dev/null
sleep 1

# 阶段2: 注入 wetype
PROF="$T/config/fcitx5/profile"
cat > "$PROF" <<'PROFEOF'
[Groups/0]
# Group Name
Name=Default
# Layout
Default Layout=us
# Default Input Method
DefaultIM=wetype-im

[Groups/0/Items/0]
# Name
Name=wetype-im
# Layout
Layout=

[Groups/0/Items/1]
# Name
Name=keyboard-us
# Layout
Layout=

[GroupOrder]
0=Default
PROFEOF
grep -c 'wetype-im' "$PROF"
grep -c 'wetype-im' "$PROF"

# 阶段3: 正式测试
fcitx5 --disable=classicui --disable=xim --disable=waylandim > "$T/fcitx5.log" 2>&1 &
MY_FCITX=$!
sleep 3

if [ "$MODE" = query ]; then
  python3 "$T/query_im.py"; RC=$?
else
  python3 "$(dirname "$0")/dbus_wetype_test.py"; RC=$?
fi

kill $MY_FCITX 2>/dev/null
pkill -f wetype-harness 2>/dev/null
kill $BUS_PID 2>/dev/null
exit $RC
