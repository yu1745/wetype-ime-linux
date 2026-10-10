#!/bin/bash
# Requires Xvfb, xdotool, xwininfo, python3-gi, GTK3 + Fcitx GTK IM module,
# Chinese fonts, and Fcitx5 built with XCB/classicui support.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(dirname "$HERE")
if [ "${1:-}" != --session ]; then
  export WETYPE_VISUAL_OUT=${WETYPE_VISUAL_OUT:-$(mktemp -d /tmp/wetype-visual.XXXXXX)}
  mkdir -p "$WETYPE_VISUAL_OUT"
  export WETYPE_VISUAL_OUT=$(realpath "$WETYPE_VISUAL_OUT")
  exec dbus-run-session -- bash "$0" --session
fi
: "${LIB:?Set LIB to the compiled WeType plugin}"
: "${WETYPE_ENGINE_DIR:?Set WETYPE_ENGINE_DIR to the installed engine}"
for tool in Xvfb xdotool xwininfo python3 fcitx5-remote; do command -v "$tool" >/dev/null; done
LIB=${LIB%.so}
[ -f "$LIB.so" ] || { echo "Plugin missing: $LIB.so"; exit 1; }
T=$(mktemp -d /tmp/wetype-visual-session.XXXXXX)
pids=()
cleanup() {
  # Close clients and Fcitx before their display server.
  for ((i=${#pids[@]}-1; i>=0; i--)); do
    pid=${pids[i]}
    if [ "$pid" = "${WETYPE_VISUAL_FCITX_PID:-}" ]; then
      fcitx5-remote --check -e >/dev/null 2>&1 || true
      for _ in $(seq 1 30); do
        kill -0 "$pid" 2>/dev/null || break
        sleep .1
      done
    fi
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
  rm -rf "$T"
  echo "Artifacts: $WETYPE_VISUAL_OUT"
}
trap cleanup EXIT
export XDG_CONFIG_HOME="$T/config" XDG_DATA_HOME="$T/data" XDG_CACHE_HOME="$T/cache"
export XDG_RUNTIME_DIR="$T/runtime"
mkdir -p "$XDG_CONFIG_HOME/fcitx5/conf" "$XDG_DATA_HOME/fcitx5/addon" \
  "$XDG_DATA_HOME/fcitx5/inputmethod" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
export WETYPE_WORK_DIR="$T/dict" WETYPE_HARNESS_LOG="$WETYPE_VISUAL_OUT/harness.log"
export GTK_IM_MODULE=fcitx GDK_BACKEND=x11 XMODIFIERS=@im=fcitx
unset WAYLAND_DISPLAY
if [ -n "${WETYPE_VISUAL_ADDON_DATA:-}" ]; then
  export XDG_DATA_DIRS="$WETYPE_VISUAL_ADDON_DATA:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
fi
cp "$SRC/wetype-im.conf" "$XDG_DATA_HOME/fcitx5/inputmethod/"
sed "s|^Library=.*|Library=$LIB|" "$SRC/wetype-addon.conf" > "$XDG_DATA_HOME/fcitx5/addon/wetype.conf"
cat > "$XDG_CONFIG_HOME/fcitx5/profile" <<'EOF'
[Groups/0]
Name=Default
Default Layout=us
DefaultIM=wetype-im
[Groups/0/Items/0]
Name=keyboard-us
[Groups/0/Items/1]
Name=wetype-im
[GroupOrder]
0=Default
EOF
cat > "$XDG_CONFIG_HOME/fcitx5/conf/classicui.conf" <<'EOF'
Vertical Candidate List=True
Font=WenQuanYi Zen Hei 16
Theme=default
EOF
Xvfb -displayfd 3 -screen 0 1024x640x24 -nolisten tcp 3>"$T/display" >"$WETYPE_VISUAL_OUT/xvfb.log" 2>&1 &
pids+=("$!")
for _ in $(seq 1 100); do [ ! -s "$T/display" ] || break; sleep .1; done
export DISPLAY=:$(cat "$T/display")
"${FCITX5_BIN:-fcitx5}" --keep --disable=wayland,waylandim,kimpanel,notificationitem,notifications \
  >"$WETYPE_VISUAL_OUT/fcitx5.log" 2>&1 &
pids+=("$!")
export WETYPE_VISUAL_FCITX_PID=${pids[1]}
python3 - <<'PY'
import os, time, dbus
bus = dbus.SessionBus()
expected = int(os.environ["WETYPE_VISUAL_FCITX_PID"])
for _ in range(150):
    os.kill(expected, 0)
    # Query the bus daemon without auto-activating a second Fcitx instance.
    if bus.name_has_owner("org.fcitx.Fcitx5"):
        registry = dbus.Interface(bus.get_object("org.freedesktop.DBus", "/org/freedesktop/DBus"),
                                  "org.freedesktop.DBus")
        assert int(registry.GetConnectionUnixProcessID("org.fcitx.Fcitx5")) == expected
        break
    time.sleep(.1)
else:
    raise RuntimeError("Private Fcitx5 did not become ready; inspect fcitx5.log")
PY
python3 "$HERE/visual_gtk_app.py" >"$WETYPE_VISUAL_OUT/gtk.log" 2>&1 &
pids+=("$!")
python3 "$HERE/visual_gtk_test.py" 2>&1 | tee "$WETYPE_VISUAL_OUT/test.log"
