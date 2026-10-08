#!/bin/bash
# Native ARM64 smoke test that needs no WeType APK.
# Usage: smoke_native_harness.sh <engine-dir>   (an AppDir's usr/lib/wetype-ime/arm64, or an install)
#
# Runs a temporary patchelf'd copy of the harness (interpreter -> bundled bionic linker64)
# and has it dlopen the libandroid.so stand-in. "dlopen ok" proves the bionic linker,
# libc/liblog/libc++ from the sysroot and the harness start natively without QEMU.
# The harness then stops at the missing engine symbol; that is expected here.
set -euo pipefail

case "$(uname -m)" in
  aarch64|arm64) ;;
  *) echo "smoke_native_harness.sh must run on an ARM64 host (this is $(uname -m))" >&2; exit 2 ;;
esac
command -v patchelf >/dev/null 2>&1 || { echo "patchelf is required" >&2; exit 2; }

eng="$(cd "${1:?usage: smoke_native_harness.sh <engine-dir>}" && pwd)"
for f in wetype-harness lib/libandroid.so sysroot/system/bin/linker64; do
  [ -f "$eng/$f" ] || { echo "missing $eng/$f" >&2; exit 1; }
done

tmp="$(mktemp -d "${TMPDIR:-/tmp}/wetype-smoke.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
cp "$eng/wetype-harness" "$tmp/wetype-harness"
patchelf --set-interpreter "$eng/sysroot/system/bin/linker64" "$tmp/wetype-harness"

out="$(cd "$tmp" && ulimit -c 0 &&
       LD_LIBRARY_PATH="$eng/sysroot/system/lib64" timeout 60 "$tmp/wetype-harness" "$eng/lib/libandroid.so" 2>&1 || true)"
echo "$out"
if grep -q '^dlopen ok: ' <<<"$out"; then
  echo "NATIVE_SMOKE = PASS"
else
  echo "NATIVE_SMOKE = FAIL" >&2
  exit 1
fi
