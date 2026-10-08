#!/usr/bin/env bash
# Extract the ARM64 bionic runtime (dynamic linker, libc and friends) from Google's
# AOSP Android 9 emulator system image. The engine libraries are NDK builds for
# bionic, so running them on bionic needs no ABI translation or ELF surgery.
# Usage: prepare_bionic.sh [output dir, default runtime/sysroot]
# Output: <dir>/system/bin/linker64, <dir>/system/lib64/*.so, <dir>/NOTICE, and
# <dir>/.image-sha256 as the completion stamp (a matching stamp skips the work).
# Android 9 is the newest release that keeps these files directly in /system
# (no runtime APEX), and the engine needs at most LIBC_P symbols.
set -euo pipefail

IMAGE_URL=https://dl.google.com/android/repository/sys-img/android/arm64-v8a-28_r02.zip
IMAGE_SHA256=86844d2edf6ba7c7f11261f6be3fb37788c985cfcf3b8924cfe9f57a1ccbc0a9
# system.img is a GPT disk; its only partition starts at sector 2048 (1 MiB).
PART_OFFSET_MIB=1
LIBS="libc.so libm.so libdl.so ld-android.so liblog.so libz.so libc++.so"

BASE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$BASE/runtime/sysroot}"
ZIP="${WETYPE_AOSP_IMAGE:-$BASE/.deps/aosp/arm64-v8a-28_r02.zip}"

export PATH="$PATH:/usr/sbin:/sbin"   # debugfs (e2fsprogs) lives in sbin
for tool in curl unzip sha256sum dd debugfs; do
  command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done

# The output only appears once complete, so an interrupted run is never mistaken for a
# finished one, and a changed pin forces a fresh extraction.
runtime_complete() {
  [[ "$(cat "$OUT/.image-sha256" 2>/dev/null)" == "$IMAGE_SHA256" ]] || return 1
  [[ -x "$OUT/system/bin/linker64" && -s "$OUT/NOTICE" ]] || return 1
  for lib in $LIBS; do
    [[ -s "$OUT/system/lib64/$lib" ]] || return 1
  done
}
if runtime_complete; then exit 0; fi

zip_ok() { [[ "$(sha256sum "$1" | cut -d' ' -f1)" == "$IMAGE_SHA256" ]]; }
if [[ ! -f "$ZIP" ]] || ! zip_ok "$ZIP"; then
  mkdir -p "$(dirname "$ZIP")"
  echo "Downloading AOSP arm64 system image (407 MB) from $IMAGE_URL"
  curl -fL --retry 3 -o "$ZIP.part" "$IMAGE_URL"
  mv "$ZIP.part" "$ZIP"
  zip_ok "$ZIP" || { echo "SHA-256 mismatch for $ZIP" >&2; exit 1; }
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/wetype-bionic.XXXXXX")"
STAGE="$OUT.tmp"
trap 'rm -rf "$TMP" "$STAGE"' EXIT
# sparse: most of the 2.6 GB filesystem is empty; keep a small /tmp tmpfs from filling up.
unzip -p "$ZIP" arm64-v8a/system.img |
  dd of="$TMP/system.ext4" bs=1M skip="$PART_OFFSET_MIB" iflag=fullblock conv=sparse status=none

rm -rf "$STAGE"
mkdir -p "$STAGE/system/bin" "$STAGE/system/lib64"
dump() {
  # debugfs splits its -R command on spaces; quote the output path for its parser.
  debugfs -R "dump /system/$1 \"$STAGE/system/$1\"" "$TMP/system.ext4" 2>/dev/null
  [[ -s "$STAGE/system/$1" ]] || { echo "Missing /system/$1 in the system image" >&2; exit 1; }
}
dump bin/linker64
chmod 755 "$STAGE/system/bin/linker64"
for lib in $LIBS; do dump "lib64/$lib"; done

# Keep the image's own notices for the files we redistribute. NOTICE.txt is a list of
# sections: a "=" rule, "Notices for file(s):", the file paths, a "-" rule, the text.
# It has no section for linker64, ld-android, libm or libdl; licenses/bionic/ carries
# those, generated from the matching bionic sources (see licenses/bionic/README).
unzip -p "$ZIP" arm64-v8a/NOTICE.txt | awk -v libs="$LIBS" '
  function flush() {
    if (keep) printf "%s\nNotices for file(s):\n%s%s\n%s", eq, files, dash, body
    keep = 0; files = ""; body = ""
  }
  BEGIN {
    n = split(libs, l, " "); for (i = 1; i <= n; i++) want["/system/lib64/" l[i]] = 1
    eq = sprintf("%60s", ""); gsub(/ /, "=", eq); dash = eq; gsub(/=/, "-", dash)
  }
  $0 == eq { flush(); state = 1; next }
  state == 1 && $0 == "Notices for file(s):" { next }
  state == 1 && $0 == dash { state = 2; next }
  state == 1 { files = files $0 "\n"; if ($0 in want) keep = 1; next }
  state == 2 { body = body $0 "\n" }
  END { flush() }' >"$STAGE/NOTICE"
[[ -s "$STAGE/NOTICE" ]] || { echo "No notices extracted from the system image" >&2; exit 1; }
notice() {   # notice <vendored file> <files it covers...>
  local src="$BASE/licenses/bionic/$1"; shift
  [[ -s "$src" ]] || { echo "Missing $src" >&2; exit 1; }
  printf '%060d\n' 0 | tr 0 = >>"$STAGE/NOTICE"
  { echo "Notices for file(s):"; printf '%s\n' "$@"; printf '%060d\n' 0 | tr 0 -; cat "$src"; } >>"$STAGE/NOTICE"
}
notice linker.NOTICE /system/bin/linker64 /system/lib64/ld-android.so
notice libm.NOTICE /system/lib64/libm.so
notice libdl.NOTICE /system/lib64/libdl.so
echo "$IMAGE_SHA256" >"$STAGE/.image-sha256"

rm -rf "$OUT"
mv "$STAGE" "$OUT"
printf 'Prepared bionic runtime at %s\n' "$OUT"
