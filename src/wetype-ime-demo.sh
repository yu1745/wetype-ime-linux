#!/bin/bash
# WeType 引擎 CLI 演示: wetype-ime-demo.sh <拼音串> → 候选词
# 用法: bash wetype-ime-demo.sh nihao    (引擎目录由 WETYPE_ENGINE_DIR 指定, 默认开发目录)
set -e
ulimit -c 0
WORD="${1:-nihao}"
ENG="${WETYPE_ENGINE_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
HARNESS="$ENG/harness/jinterop"
[ -x "$HARNESS" ] || HARNESS="$ENG/wetype-harness"
LIBDIR="$ENG/lib"
[ -d "$LIBDIR" ] || LIBDIR="$ENG/runtime"   # 开发目录布局
DICTS="${WETYPE_DICT_DIR:-$ENG/dicts}"
[ -d "$DICTS" ] || DICTS="$ENG/.deps/wechat-ime/assets/config/beta"   # source checkout
WORK="${WETYPE_WORK_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/wetype-ime/dict}"
SYSROOT="${WETYPE_SYSROOT:-}"
if [ -z "$SYSROOT" ]; then   # bionic 运行时：安装目录 sysroot/，源码树 runtime/sysroot/
  if [ -d "$ENG/sysroot/system" ]; then SYSROOT="$ENG/sysroot"; else SYSROOT="$ENG/runtime/sysroot"; fi
fi
QEMU="${QEMU_AARCH64:-}"
if [ -z "$QEMU" ]; then   # 安装包自带 QEMU；ARM64 宿主无 QEMU 时直接运行（harness 解释器已指向 linker64）
  if [ -x "$ENG/qemu-aarch64-static" ]; then QEMU="$ENG/qemu-aarch64-static"
  elif [ "$(uname -m)" != aarch64 ]; then QEMU=qemu-aarch64-static   # 源码树回落到系统包
  fi
fi
RUN=()
if [ -n "$QEMU" ]; then
  RUN=("$QEMU" -L "$SYSROOT")
else
  LIBDIR_EXTRA=":$SYSROOT/system/lib64"
fi

mkdir -p "$WORK/userdict/v5" "$WORK/userdict/user_hot_word"

# 拼音串 → L 命令序列
CMDS="PING"
for ((i=0; i<${#WORD}; i++)); do CMDS+="|L ${WORD:$i:1}"; done
CMDS+="|Q"

printf '%b\n' "${CMDS//|/\\n}" | \
  env LD_LIBRARY_PATH="$LIBDIR${LIBDIR_EXTRA:-}" WETYPE_DICT_DIR="$DICTS" \
      WETYPE_WORK_DIR="$WORK" \
  "${RUN[@]}" "$HARNESS" "$([ -f "$LIBDIR/libwxhld_jni.so" ] && echo "$LIBDIR/libwxhld_jni.so" || echo runtime/libwxhld_jni.so)" --daemon 2>/dev/null | \
  while IFS= read -r line; do
    case "$line" in
      CAND*) echo "候选: ${line#CAND$'\t'}" | tr '\t' ' ' ;;
    esac
  done
