#!/bin/bash
# e2_img.sh — AppImage packaging with a pinned appimagetool release.
set -eo pipefail
BASE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$BASE"
ARCH="${ARCH:-$(uname -m)}"
case "$ARCH" in
  x86_64|aarch64) ;;
  *) echo "Unsupported AppImage architecture: $ARCH (x86_64 or aarch64)" >&2; exit 1 ;;
esac
# appimagetool runs on the build host, so its architecture is the host's.
TOOL_ARCH="$(uname -m)"
TOOL="${APPIMAGETOOL:-$BASE/.deps/tools/appimagetool-1.9.1-$TOOL_ARCH.AppImage}"
if [ ! -x "$TOOL" ]; then
  echo "== 下载 appimagetool 1.9.1 ($TOOL_ARCH) =="
  mkdir -p "$(dirname "$TOOL")"
  curl -fL --retry 3 -o "$TOOL" \
    "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-$TOOL_ARCH.AppImage"
  chmod +x "$TOOL"
fi
export WETYPE_TARGET_ARCH="$ARCH"
bash scripts/e1_appdir.sh
cp AppDir/usr/share/applications/wetype-ime.desktop AppDir/
OUT="WeTypeIME-Engine-${ARCH}.AppImage"
rm -f WeTypeIME-Engine-*.AppImage
TOOL_ARGS=(--comp zstd)
if [ -n "${APPIMAGE_RUNTIME:-}" ]; then
  TOOL_ARGS+=(--runtime-file "$APPIMAGE_RUNTIME")
fi
ARCH="$ARCH" "$TOOL" "${TOOL_ARGS[@]}" AppDir "$OUT" 2>&1 | tail -3
ls -la "$OUT" | awk '{print $5, $9}'
if [ -f WeTypeIME-Engine-x86_64.AppImage ]; then
  ls -la WeTypeIME-Engine-x86_64.AppImage | awk '{print $5, $9}'
fi
