#!/bin/bash
# 候选翻页隔离 e2e: 私有 dbus + 临时 XDG_CONFIG_HOME/XDG_DATA_HOME,
# 不影响桌面会话, 把 Fcitx5 全局"候选词数量"设成指定值后真机验证翻页行为。
#
# 用法:  bash fcitx5-wetype/e2e/run_paging_e2e.sh [每页候选数]     # 默认 5
# 前置:  cmake -S fcitx5-wetype -B build && cmake --build build
# 依赖:  python3-dbus, gir1.2-glib-2.0, dbus, fcitx5, 已安装好的 WeType 引擎
set -u
PS="${1:-5}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(dirname "$HERE")"                                  # fcitx5-wetype/
LIB="${LIB:-$(dirname "$SRC")/build/libfcitx5-wetype}"
[ -e "$LIB" ] || [ -e "$LIB.so" ] || { echo "找不到插件: $LIB(.so) — 先 cmake --build build"; exit 9; }
ENGINE_DIR="${WETYPE_ENGINE_DIR:-$HOME/.local/lib/wetype-ime/arm64}"

T=$(mktemp -d /tmp/wetype-paging-e2e.XXXXXX)
cleanup() {
  kill $(cat "$T/pids" 2>/dev/null) 2>/dev/null || true
  # Fcitx writes its config on exit; wait before removing the isolated tree.
  [ -z "${FCITX_PID:-}" ] || wait "$FCITX_PID" 2>/dev/null || true
  rm -rf "$T"
}
trap cleanup EXIT
mkdir -p "$T/config/fcitx5" "$T/data/fcitx5/addon" "$T/data/fcitx5/inputmethod" "$T/bus"
if [ "${WETYPE_PAGETEST_GLOSSARY:-0}" = 1 ]; then
  mkdir -p "$T/data/wetype-ime"
  cp "$HERE/glossary-fixture.tsv" "$T/data/wetype-ime/glossary-en.tsv"
fi

# 只装本插件: addon conf 里的 Library 换成刚构建的绝对路径
cp "$SRC/wetype-im.conf" "$T/data/fcitx5/inputmethod/"
sed "s|^Library=.*|Library=$LIB|" "$SRC/wetype-addon.conf" > "$T/data/fcitx5/addon/wetype.conf"
cat > "$T/config/fcitx5/config" <<EOF
[Behavior]
DefaultPageSize=$PS
EOF
cat > "$T/config/fcitx5/profile" <<'EOF'
[Groups/0]
Name=Default
Default Layout=us
DefaultIM=wetype-im

[Groups/0/Items/0]
Name=wetype-im
Layout=

[GroupOrder]
0=Default
EOF

dbus-daemon --session --fork --print-address=3 --print-pid=4 3>"$T/bus/addr" 4>"$T/bus/pid"
for _ in $(seq 1 30); do [ -s "$T/bus/addr" ] && break; sleep 0.1; done
export DBUS_SESSION_BUS_ADDRESS="$(cat "$T/bus/addr")"
echo "$(cat "$T/bus/pid")" > "$T/pids"
export XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data"
export XDG_DATA_DIRS="/usr/local/share:/usr/share"
export WETYPE_ENGINE_DIR="$ENGINE_DIR"
export WETYPE_WORK_DIR="$T/data/wetype-ime/dict"
export WETYPE_PAGETEST_LOG="$T/fcitx5.log"
export WETYPE_PAGETEST_CONFIG="$T/config/fcitx5/config"

env -u DISPLAY -u WAYLAND_DISPLAY fcitx5 --keep --replace \
  --disable=classicui,xim,waylandim,xcb,wayland,kimpanel >"$T/fcitx5.log" 2>&1 &
FCITX_PID=$!
echo "$FCITX_PID" >> "$T/pids"; sleep 4
if ! grep -q "async addon init" "$T/fcitx5.log"; then
  echo "插件未加载, 日志尾部:"; tail -20 "$T/fcitx5.log"; exit 8
fi

python3 "$HERE/paging_test.py" "$PS"; RC=$?
echo "E2E_PAGE_SIZE=$PS RC=$RC"
[ "$RC" != 0 ] && { echo "--- 日志尾部 ---"; tail -30 "$T/fcitx5.log"; }
exit $RC
