#!/usr/bin/env bash
# Download the pinned x86_64 Linux NDK into the project cache; no system changes.
# Official release/checksum: https://github.com/android/ndk/releases/tag/r27c
set -euo pipefail
BASE="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=r27c
REVISION=27.2.12479018
SHA1=090e8083a715fdb1a3e402d0763c388abb03fb4e
OUT="$BASE/.deps/tools/android-ndk-$VERSION"
ZIP="$BASE/.deps/tools/android-ndk-$VERSION-linux.zip"

[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || {
  echo "This NDK package requires an x86_64 Linux build host." >&2
  exit 1
}
for tool in curl unzip sha1sum; do
  command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done
ndk_ok() {
  [[ -x "$1/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" &&
     -x "$1/toolchains/llvm/prebuilt/linux-x86_64/bin/ld.lld" &&
     -s "$1/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/jni.h" ]] &&
    grep -Eq '^Pkg.Revision[[:space:]]*=[[:space:]]*27\.2\.12479018[[:space:]]*$' "$1/source.properties"
}
if ndk_ok "$OUT"; then
  echo "Android NDK $VERSION ($REVISION) ready: $OUT"
  exit 0
fi
zip_ok() { [[ -f "$ZIP" && "$(sha1sum "$ZIP" | cut -d' ' -f1)" == "$SHA1" ]]; }
mkdir -p "$(dirname "$ZIP")"
if ! zip_ok; then
  echo "Downloading Android NDK $VERSION for x86_64 Linux"
  curl -fL --retry 3 -o "$ZIP.part" "https://dl.google.com/android/repository/android-ndk-$VERSION-linux.zip"
  [[ "$(sha1sum "$ZIP.part" | cut -d' ' -f1)" == "$SHA1" ]] || {
    echo "Official NDK checksum mismatch" >&2; exit 1;
  }
  mv "$ZIP.part" "$ZIP"
fi
STAGE="$(mktemp -d "$BASE/.deps/tools/ndk-extract.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
unzip -q "$ZIP" -d "$STAGE"
ndk_ok "$STAGE/android-ndk-$VERSION" || { echo "Incomplete NDK archive" >&2; exit 1; }
rm -rf "$OUT"
mv "$STAGE/android-ndk-$VERSION" "$OUT"
echo "Android NDK $VERSION ($REVISION) ready: $OUT"
