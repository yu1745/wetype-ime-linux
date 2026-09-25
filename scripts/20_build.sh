#!/usr/bin/env bash
# Build the project's own ARM64 pieces (shim + harness) into runtime/ and harness/.
# Needs no APK input; scripts/10_patch_libs.sh adds the patched WeType libraries.
set -e
cd "$(dirname "$0")/.."
HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in
  x86_64) AARCH64_CC="${WETYPE_AARCH64_CC:-aarch64-linux-gnu-gcc}" ;;
  aarch64) AARCH64_CC="${WETYPE_AARCH64_CC:-cc}" ;;
  *) echo "Unsupported build host: $HOST_ARCH (expected x86_64 or aarch64)" >&2; exit 1 ;;
esac
for tool in "$AARCH64_CC" patchelf; do
  command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done
mkdir -p runtime harness
if [ ! -f runtime/libz.so.1 ]; then
  ZLIB_SO="${WETYPE_ZLIB_SO:-}"
  for candidate in \
    /lib/aarch64-linux-gnu/libz.so.1 \
    /usr/lib/aarch64-linux-gnu/libz.so.1 \
    /usr/aarch64-linux-gnu/lib/libz.so.1; do
    if [ -z "$ZLIB_SO" ] && [ -f "$candidate" ]; then ZLIB_SO="$candidate"; fi
  done
  if [ -z "$ZLIB_SO" ] || [ ! -f "$ZLIB_SO" ]; then
    echo "Missing ARM64 zlib. Install zlib1g-dev:arm64 or set WETYPE_ZLIB_SO." >&2
    exit 1
  fi
  cp -fL "$ZLIB_SO" runtime/libz.so.1
fi
"$AARCH64_CC" -shared -fPIC -O2 -o runtime/libwetype-shim.so \
  shim/wetype-shim.c shim/wetype-signal.c -ldl
# shim 自身去掉全部 DT_NEEDED：glibc _dl_sort_maps 按深度重排搜索列表时，
# 依赖 libc 的 shim 会被排到 libc 之后导致 stdio 包装失效；无依赖叶子节点则稳居第一，
# 其 UND 符号（dlsym/fprintf 等）从全局作用域解析（主程序已加载 libc）。
patchelf --remove-needed libc.so.6 runtime/libwetype-shim.so 2>/dev/null || true
patchelf --remove-needed libdl.so.2 runtime/libwetype-shim.so 2>/dev/null || true
echo "shim NEEDED 残留: $(patchelf --print-needed runtime/libwetype-shim.so | wc -l)"
"$AARCH64_CC" -O0 -g -o harness/jinterop harness/jinterop.c \
  -ldl -lpthread -Wl,--no-as-needed -lm "$PWD/runtime/libz.so.1"
"$AARCH64_CC" -O0 -g -o harness/probe harness/probe.c -ldl -lpthread \
  -Wl,--no-as-needed -lm "$PWD/runtime/libz.so.1"
echo "build ok"
