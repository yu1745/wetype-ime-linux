#!/usr/bin/env bash
# 拷贝引擎闭包库并打引擎级二进制补丁（幂等：先重拷原始库）
# 用法: 10_patch_libs.sh [输出目录，默认 runtime/]。只处理 APK 里的库；libandroid.so 替身由 20_build.sh 产出。
# 库在 bionic 上原样加载，无需改 ELF；安装时也在用户机器上运行（AppImage install），只依赖 python3。
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BASE="$(dirname "$SCRIPT_DIR")"
APK_ROOT="${WETYPE_APK_ROOT:-$BASE/.deps/wechat-ime}"
SRC="$APK_ROOT/lib/arm64-v8a"
OUT="${1:-$BASE/runtime}"
mkdir -p "$OUT"
cd "$OUT"

command -v python3 >/dev/null || { echo "Missing build tool: python3" >&2; exit 1; }

if [ ! -f "$SRC/libwxhld_jni.so" ]; then
  echo "Missing WeType APK libraries at $SRC" >&2
  echo "Run: $BASE/scripts/prepare_assets.sh" >&2
  exit 1
fi

# 引擎闭包：libwxhld_jni.so 及其传递依赖（系统库由 bionic 与 libandroid.so 替身提供）
ENGINELIBS="libandromeda.so libcryptopp.so libc++_shared.so libime_net.so libowl.so \
  libprotobuf-lite.so libtensorflowlite_c.so libwcwss.so libwxhld.so libwxhld_jni.so libwechatxlog.so"

for f in $ENGINELIBS; do
  cp -f "$SRC/$f" .
done

python3 "$SCRIPT_DIR/17_disable_scan_sig.py" libwxhld.so
python3 "$SCRIPT_DIR/21_fake_appender.py" libwxhld.so
