#!/bin/bash
# Called by AppRun inside the mounted AppImage or an extracted AppDir.
set -euo pipefail

appdir="${1:?missing AppDir}"
action="${2:?missing action}"
shift 2

usage() {
  echo "Usage: AppImage install [--user|--system] [--apk FILE]" >&2
  echo "       AppImage uninstall [--user|--system]" >&2
  exit 2
}

case "$action" in install|uninstall) ;; *) usage ;; esac
scope=
apk=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --user) scope=user ;;
    --system) scope=system ;;
    --apk)
      [ "$action" = install ] && [ -n "${2:-}" ] || usage
      apk="$(readlink -f "$2")"; shift ;;
    *) usage ;;
  esac
  shift
done
if [ -z "$scope" ]; then
  if [ "$(id -u)" -eq 0 ]; then scope=system; else scope=user; fi
fi

if [ "$scope" = system ]; then
  if [ "$(id -u)" -ne 0 ]; then
    echo "系统安装需要 root 权限；请使用 sudo，或选择 --user。" >&2
    exit 1
  fi
  prefix=/usr
  if command -v pkg-config >/dev/null 2>&1 &&
     [ "$(pkg-config --variable=prefix Fcitx5Module 2>/dev/null || true)" = /usr ]; then
    fcitx_libdir="$(pkg-config --variable=libdir Fcitx5Module)"
    case "$fcitx_libdir" in /usr/lib|/usr/lib/*|/usr/lib64) ;; *) fcitx_libdir= ;; esac
  else
    fcitx_libdir=
  fi
  if [ -z "$fcitx_libdir" ]; then
    triplet="$(gcc -dumpmachine 2>/dev/null || true)"
    if [ -n "$triplet" ] && [ -d "/usr/lib/$triplet/fcitx5" ]; then
      fcitx_libdir="/usr/lib/$triplet"
    elif [ -d /usr/lib64/fcitx5 ]; then
      fcitx_libdir=/usr/lib64
    else
      fcitx_libdir=/usr/lib
    fi
  fi
  addon_dir="$fcitx_libdir/fcitx5"
  apk_cache=/var/cache/wetype-ime
else
  prefix="$HOME/.local"
  addon_dir="$prefix/lib/fcitx5"
  apk_cache="${XDG_CACHE_HOME:-$HOME/.cache}/wetype-ime"
fi

src="$appdir/usr"
eng="$prefix/lib/wetype-ime/arm64"
src_eng="$src/lib/wetype-ime/arm64"
src_scripts="$src/lib/wetype-ime/scripts"
addon="$addon_dir/libfcitx5-wetype.so"
addon_conf="$prefix/share/fcitx5/addon/wetype.conf"
im_conf="$prefix/share/fcitx5/inputmethod/wetype-im.conf"
desktop="$prefix/share/applications/wetype-ime.desktop"
icon="$prefix/share/icons/hicolor/256x256/apps/wetype-ime.png"

print_existing_paths() {
  local label="$1" path="$2"
  if [ -e "$path" ] || [ -L "$path" ]; then
    if [ -d "$path" ] && [ ! -L "$path" ]; then
      find "$path" -print0 | LC_ALL=C sort -z | while IFS= read -r -d '' entry; do
        printf '  [%s] %s\n' "$label" "$entry"
      done
    else
      printf '  [%s] %s\n' "$label" "$path"
    fi
  fi
}

# Check everything up front and print a copy-paste command; never install packages ourselves.
check_install_deps() {
  local missing=() cmd tool id=
  for tool in python3 unzip sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
  done
  if [ -z "$apk" ] && ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
    missing+=(curl)
  fi
  [ "${#missing[@]}" -eq 0 ] && return

  echo "缺少安装依赖：${missing[*]}" >&2
  if [ -r /etc/os-release ]; then
    id="$(. /etc/os-release; echo " ${ID:-} ${ID_LIKE:-} ")"
  fi
  case "$id" in
    *" debian "*|*" ubuntu "*) cmd="sudo apt install python3 unzip curl" ;;
    *" arch "*) cmd="sudo pacman -S --needed python unzip curl" ;;
    *" fedora "*|*" rhel "*|*" centos "*) cmd="sudo dnf install python3 unzip curl" ;;
    *" suse "*|*" opensuse "*) cmd="sudo zypper install python3 unzip curl" ;;
  esac
  if [ -n "${cmd:-}" ]; then
    echo "请先运行：$cmd" >&2
  else
    echo "请用发行版的包管理器安装上述命令。" >&2
  fi
  exit 1
}

if [ "$action" = install ]; then
  for required in \
    "$src_eng/wetype-harness" "$src_eng/wetype-ime-demo.sh" \
    "$src_eng/lib/libandroid.so" \
    "$src_eng/qemu-aarch64-static" "$src_eng/sysroot/system/bin/linker64" \
    "$src_scripts/prepare_assets.sh" "$src_scripts/10_patch_libs.sh" \
    "$src/lib/fcitx5/libfcitx5-wetype.so" \
    "$src/share/fcitx5/addon/wetype.conf" \
    "$src/share/fcitx5/inputmethod/wetype-im.conf" \
    "$src/share/applications/wetype-ime.desktop" \
    "$src/share/icons/hicolor/256x256/apps/wetype-ime.png" \
    "$src/bin/wetype-ime-engine" "$src/bin/wetype-demo"; do
    if [ ! -f "$required" ]; then
      echo "AppImage 缺少安装文件：$required" >&2
      exit 1
    fi
  done
  check_install_deps
  # Paper 式：WeType APK 从腾讯官方服务器下载（或用 --apk 指定），校验 SHA-256 后在本机打补丁。
  stage="$(mktemp -d "${TMPDIR:-/tmp}/wetype-install.XXXXXX")"
  trap 'rm -rf "$stage"' EXIT
  WETYPE_APK_ROOT="$stage/apk" WETYPE_APK_CACHE="$apk_cache" \
    bash "$src_scripts/prepare_assets.sh" ${apk:+"$apk"}
  echo "正在本机修补 WeType 引擎库…"
  WETYPE_APK_ROOT="$stage/apk" bash "$src_scripts/10_patch_libs.sh" "$stage/lib" >/dev/null
  cp -a --no-preserve=ownership "$src_eng/lib/." "$stage/lib/"

  mkdir -p "$eng" "$eng/dicts" "$addon_dir" \
    "$(dirname "$addon_conf")" "$(dirname "$im_conf")" \
    "$(dirname "$desktop")" "$(dirname "$icon")" "$prefix/bin"
  rm -rf "$eng/lib"
  cp -a --no-preserve=ownership "$stage/lib" "$eng/lib"
  for dict in "$stage/apk/assets/config/beta"/*; do   # keep existing dictionaries
    [ -e "$eng/dicts/${dict##*/}" ] || cp -a --no-preserve=ownership "$dict" "$eng/dicts/"
  done
  install -m 755 "$src_eng/wetype-harness" "$eng/wetype-harness"
  install -m 755 "$src_eng/wetype-ime-demo.sh" "$eng/wetype-ime-demo.sh"
  install -m 755 "$src_eng/qemu-aarch64-static" "$eng/qemu-aarch64-static"
  rm -rf "$eng/sysroot"
  cp -a --no-preserve=ownership "$src_eng/sysroot" "$eng/sysroot"
  install -m 755 "$src/bin/wetype-ime-engine" "$prefix/bin/wetype-ime-engine"
  install -m 755 "$src/bin/wetype-demo" "$prefix/bin/wetype-demo"
  install -m 755 "$src/lib/fcitx5/libfcitx5-wetype.so" "$addon"
  install -m 644 "$src/share/fcitx5/addon/wetype.conf" "$addon_conf"
  if [ "$scope" = user ]; then
    # Fcitx5 never searches ~/.local/lib/fcitx5; an absolute Library= (without .so) is honored.
    sed -i "s|^Library=.*|Library=${addon%.so}|" "$addon_conf"
  fi
  install -m 644 "$src/share/fcitx5/inputmethod/wetype-im.conf" "$im_conf"
  install -m 644 "$src/share/applications/wetype-ime.desktop" "$desktop"
  install -m 644 "$src/share/icons/hicolor/256x256/apps/wetype-ime.png" "$icon"
  echo "安装完成 ($scope)。Fcitx5 插件：$addon"
  echo "请重启 Fcitx5，并在输入法配置中添加“微信拼音”。"
else
  echo "卸载清单 ($scope)，以下路径均在 AppImage 之外："
  for path in "$addon" "$addon_conf" "$im_conf" "$desktop" "$icon" \
    "$prefix/bin/wetype-ime-engine" "$prefix/bin/wetype-demo" \
    "$eng/wetype-harness" "$eng/wetype-ime-demo.sh" "$eng/qemu-aarch64-static"; do
    print_existing_paths 删除 "$path"
  done
  print_existing_paths 删除 "$eng/lib"
  print_existing_paths 删除 "$eng/sysroot"
  print_existing_paths 保留词库 "$eng/dicts"
  print_existing_paths 保留APK缓存 "$apk_cache"
  if [ "$scope" = user ]; then
    user_data="${XDG_DATA_HOME:-$HOME/.local/share}/wetype-ime"
    print_existing_paths 保留用户数据 "$user_data"
  else
    echo '  [保留用户数据] 各用户的 ${XDG_DATA_HOME:-$HOME/.local/share}/wetype-ime（系统卸载不访问）'
  fi
  rm -f "$addon" "$addon_conf" "$im_conf" "$desktop" "$icon" \
    "$prefix/bin/wetype-ime-engine" "$prefix/bin/wetype-demo" \
    "$eng/wetype-harness" "$eng/wetype-ime-demo.sh" "$eng/qemu-aarch64-static"
  rm -rf "$eng/lib" "$eng/sysroot"
  echo "卸载完成 ($scope)。预置词库保留在：$eng/dicts"
  echo "用户学习数据未清理。请重启 Fcitx5。"
fi

# Reload a running user session when possible; a restart is still needed for new modules.
if command -v fcitx5-remote >/dev/null 2>&1 && [ "$(id -u)" -ne 0 ]; then
  fcitx5-remote -r >/dev/null 2>&1 || true
fi
