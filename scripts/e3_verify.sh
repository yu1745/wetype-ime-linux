#!/bin/bash
# e3_verify.sh — 交付版验证矩阵 (对齐 doubao b3/b7)
set -e
BASE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$BASE"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/wetype-verify.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT
# 优先验证已安装的引擎（AppImage install 生成），否则用源码树 runtime/
if [ -z "${WETYPE_ENGINE_DIR:-}" ]; then
  for d in "$HOME/.local/lib/wetype-ime/arm64" /usr/lib/wetype-ime/arm64; do
    if [ -f "$d/lib/libwxhld_jni.so" ]; then export WETYPE_ENGINE_DIR="$d"; break; fi
  done
fi
echo "引擎目录: ${WETYPE_ENGINE_DIR:-$BASE (源码树)}"
mkdir -p "$WORK_DIR/userdict/v5" "$WORK_DIR/userdict/user_hot_word"
export WETYPE_WORK_DIR="$WORK_DIR"
echo "--- 1. nihao → 候选 ---"
bash src/wetype-ime-demo.sh nihao | tail -1
echo "--- 2. zhongguo → 候选 ---"
bash src/wetype-ime-demo.sh zhongguo | tail -1
echo "--- 3. 用户词库目录 ---"
ls "$WETYPE_WORK_DIR/userdict/" 2>/dev/null || echo "(尚未生成)"
echo "--- 4. 选词学习验证 (拟好 rank2 → 选词后 rank1) ---"
WETYPE_TEST_LEARN=1 LD_LIBRARY_PATH="$PWD/runtime" \
  timeout 150 "${QEMU_AARCH64:-qemu-aarch64-static}" -cpu "${QEMU_CPU:-cortex-a72}" -L "${WETYPE_SYSROOT:-$PWD/runtime/sysroot}" \
  ./harness/jinterop runtime/libwxhld_jni.so 2>/dev/null | grep -E '\[A\] rank|\[B\] rank' | head -6
