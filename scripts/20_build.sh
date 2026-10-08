#!/usr/bin/env bash
# Build the project's own ARM64 pieces (libandroid.so stand-in + harness) with the
# Android NDK and extract the bionic runtime into runtime/sysroot.
# Needs no APK input; scripts/10_patch_libs.sh adds the patched WeType libraries.
set -e
cd "$(dirname "$0")/.."
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-${ANDROID_NDK_LATEST_HOME:-$PWD/.deps/tools/android-ndk-r27c}}}"
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
[ -n "$NDK" ] && [ -x "$CC" ] || {
  echo "Missing Android NDK. Run bash scripts/prepare_ndk.sh, or set ANDROID_NDK_HOME." >&2
  exit 1
}
# API 28 matches the bundled Android 9 bionic.
TARGET=--target=aarch64-linux-android28
mkdir -p runtime harness
bash scripts/prepare_bionic.sh runtime/sysroot   # no-op when runtime/sysroot is complete and current
"$CC" $TARGET -shared -fPIC -O2 -Wl,-soname,libandroid.so -o runtime/libandroid.so shim/libandroid.c
"$CC" $TARGET -O0 -g -o harness/jinterop harness/jinterop.c -ldl
"$CC" $TARGET -O0 -g -o harness/probe harness/probe.c -ldl
echo "build ok"
