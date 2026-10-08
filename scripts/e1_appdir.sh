#!/bin/bash
# e1_appdir.sh — 组装 WeType 输入法 AppDir（对齐 doubao-ime-linux b1 布局）
# AppDir 只含本项目自己的代码（Paper 式）：不含任何 APK 里的库或词库。
# 结构:
#   AppDir/usr/lib/wetype-ime/arm64/{lib/libandroid.so,wetype-harness,wetype-ime-demo.sh}
#   AppDir/usr/lib/wetype-ime/arm64/{qemu-aarch64-static,sysroot/system/}  QEMU + AOSP bionic 运行时（第三方，可再分发）
#     ARM64 宿主（uname -m = aarch64 或 WETYPE_TARGET_ARCH=aarch64）不打包 QEMU：
#     harness 在安装时用 patchelf 把 ELF 解释器指向 sysroot/system/bin/linker64，直接原生运行。
#   AppDir/usr/lib/wetype-ime/scripts/  安装时下载官方 APK、校验 SHA-256 并在本机打补丁
#   AppDir/usr/bin/{wetype-ime-engine,wetype-demo}
#   AppDir/usr/lib/fcitx5/libfcitx5-wetype.so
#   用户可写数据: $XDG_DATA_HOME/wetype-ime/dict  (运行时由 harness 通过 WETYPE_WORK_DIR 使用)
# 环境变量:
#   WETYPE_SKIP_NDK_BUILD=1  跳过 20_build.sh，直接使用已有的 runtime/ 与 harness/jinterop
#                            （NDK 只有 x86_64 主机版，ARM64 主机用 x86_64 构建产物打包）
set -e
BASE="$(cd "$(dirname "$0")/.." && pwd)"     # wetype-ime-linux 根
APPDIR="$BASE/AppDir"
ENG="$APPDIR/usr/lib/wetype-ime/arm64"
PATCH_SCRIPTS="prepare_assets.sh 10_patch_libs.sh 17_disable_scan_sig.py 21_fake_appender.py"
case "${WETYPE_TARGET_ARCH:-$(uname -m)}" in
  aarch64|arm64) NATIVE=1 ;;
  *) NATIVE=0 ;;
esac

# Build the project's own ARM64 pieces and extract the bionic runtime (no APK input needed).
if [ "${WETYPE_SKIP_NDK_BUILD:-}" = 1 ]; then
  for f in runtime/libandroid.so harness/jinterop runtime/sysroot/system/bin/linker64; do
    [ -e "$BASE/$f" ] || { echo "WETYPE_SKIP_NDK_BUILD=1 但缺少 $f" >&2; exit 1; }
  done
else
  bash "$BASE/scripts/20_build.sh"
fi

# Package the host Fcitx5 addon along with the ARM64 engine.
"$BASE/fcitx5-wetype/build.sh"
ADDON_SO="$BASE/fcitx5-wetype/build/libfcitx5-wetype.so"
[ -f "$ADDON_SO" ] || { echo "Missing Fcitx5 addon: $ADDON_SO" >&2; exit 1; }

rm -rf "$APPDIR"
mkdir -p "$ENG/lib" "$APPDIR/usr/lib/wetype-ime/scripts" "$APPDIR/usr/bin" \
         "$APPDIR/usr/lib/fcitx5" "$APPDIR/usr/share/fcitx5/addon" \
         "$APPDIR/usr/share/fcitx5/inputmethod" "$APPDIR/usr/lib/wetype-ime" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps"

# 1. 自有 ARM64 组件 + demo；WeType 引擎库与词库由 install 在用户机器上生成
cp "$BASE/runtime/libandroid.so" "$ENG/lib/"
cp "$BASE/harness/jinterop" "$ENG/wetype-harness"
cp "$BASE/src/wetype-ime-demo.sh" "$ENG/"
chmod +x "$ENG/wetype-ime-demo.sh"

# 1b. 第三方运行时：AOSP bionic（可再分发，见 THIRD-PARTY）；x86 等非 ARM64 宿主另带静态 QEMU user 模式
if [ "$NATIVE" = 0 ]; then
  QEMU_BIN="${QEMU_AARCH64:-$(command -v qemu-aarch64-static || true)}"
  [ -n "$QEMU_BIN" ] && [ -x "$QEMU_BIN" ] || { echo "Missing qemu-aarch64-static (qemu-user-static)" >&2; exit 1; }
  file -L "$QEMU_BIN" | grep -q 'static' || { echo "$QEMU_BIN is not statically linked" >&2; exit 1; }
  cp -L "$QEMU_BIN" "$ENG/qemu-aarch64-static"
fi
cp -a "$BASE/runtime/sysroot" "$ENG/sysroot"
pkg_version() { dpkg-query -W -f '${Version}' "$1" 2>/dev/null || echo unknown; }
mkdir -p "$APPDIR/usr/share/doc/wetype-ime"
cp "$BASE/licenses/Apache-2.0.txt" "$APPDIR/usr/share/doc/wetype-ime/"
cp "$BASE/licenses/jni/README.md" "$APPDIR/usr/share/doc/wetype-ime/JNI-SOURCE.md"
QEMU_ROW=
QEMU_NOTE=
if [ "$NATIVE" = 0 ]; then
  QEMU_ROW='| QEMU user mode | usr/lib/wetype-ime/arm64/qemu-aarch64-static | GPL-2.0 |'
  QEMU_NOTE="QEMU is an unmodified copy of the build host's qemu-user-static $(pkg_version qemu-user-static)
package; corresponding source is available from the distribution's source archive
(for Ubuntu: \`apt-get source qemu\`, or https://launchpad.net/ubuntu/+source/qemu) and from
https://www.qemu.org.

"
fi
cat > "$APPDIR/usr/share/doc/wetype-ime/THIRD-PARTY.md" <<NOTICE
# Third-party components bundled in this AppImage

| Component | Files | License |
|---|---|---|
${QEMU_ROW:+$QEMU_ROW
}| AOSP JNI declarations used by harness | wetype-harness (source: harness/jni.h) | Apache-2.0; full text: Apache-2.0.txt, provenance: JNI-SOURCE.md |
| Android bionic (linker, libc, libm, libdl) | usr/lib/wetype-ime/arm64/sysroot/system/bin/linker64, system/lib64/{libc,libm,libdl,ld-android}.so | BSD-style, Apache-2.0 |
| Android liblog | usr/lib/wetype-ime/arm64/sysroot/system/lib64/liblog.so | Apache-2.0 |
| LLVM libc++ (liblog dependency) | usr/lib/wetype-ime/arm64/sysroot/system/lib64/libc++.so | NCSA / MIT |
| zlib | usr/lib/wetype-ime/arm64/sysroot/system/lib64/libz.so | Zlib |

${QEMU_NOTE}The Android components are unmodified files from Google's AOSP Android 9 emulator system
image (system-images;android-28;default;arm64-v8a, arm64-v8a-28_r02, build PSR1.210301.009.B6).
Their notices are in usr/lib/wetype-ime/arm64/sysroot/NOTICE: the image's own notices for
libc, liblog, libc++ and zlib, plus notices for linker64, ld-android, libm and libdl
generated from platform/bionic tag android-9.0.0_r61 (last Android 9 tag), which the image
does not list.
Source code: https://android.googlesource.com (platform/bionic, platform/system/core,
platform/external/libcxx, platform/external/zlib).

WeType (微信输入法) itself is NOT included: its libraries and dictionaries are downloaded
from Tencent's server and patched on the user's machine at install time.
NOTICE

# 2. 安装时运行的下载/校验/补丁脚本
for f in $PATCH_SCRIPTS; do
  cp "$BASE/scripts/$f" "$APPDIR/usr/lib/wetype-ime/scripts/"
done
chmod +x "$APPDIR"/usr/lib/wetype-ime/scripts/*.sh

# 3. 引擎启动器（行协议 REPL，fcitx5 addon 也 exec 它）
# 安装带 QEMU（x86 等）时经 QEMU 运行；ARM64 安装没有 QEMU，harness 的解释器已在安装时
# 指向 sysroot 里的 linker64，直接运行，并让 bionic 的 libc/liblog/libc++ 从 sysroot 解析。
cat > "$APPDIR/usr/bin/wetype-ime-engine" <<'EOF'
#!/bin/bash
ENG="$(dirname "$(readlink -f "$0")")/../lib/wetype-ime/arm64"
ulimit -c 0
USRDATA="${XDG_DATA_HOME:-$HOME/.local/share}/wetype-ime"
mkdir -p "$USRDATA/dict/userdict/v5" "$USRDATA/dict/userdict/user_hot_word"
SYSROOT="${WETYPE_SYSROOT:-$ENG/sysroot}"
QEMU="${QEMU_AARCH64:-$ENG/qemu-aarch64-static}"
LIBS="$ENG/lib"
if [ -x "$QEMU" ]; then
  # 默认的 max 会启用指针认证，QEMU 用软件计算，慢约 2 倍
  RUN=("$QEMU" -cpu "${QEMU_CPU:-cortex-a72}" -L "$SYSROOT")
else
  RUN=()
  LIBS="$LIBS:$SYSROOT/system/lib64"
fi
exec env LD_LIBRARY_PATH="$LIBS" \
         WETYPE_DICT_DIR="$ENG/dicts" \
         WETYPE_ASSET_DIR="$ENG/dicts" \
         WETYPE_WORK_DIR="$USRDATA/dict" \
    "${RUN[@]}" "$ENG/wetype-harness" "$ENG/lib/libwxhld_jni.so" --daemon
EOF
chmod +x "$APPDIR/usr/bin/wetype-ime-engine"

# 4. CLI 演示启动器
cat > "$APPDIR/usr/bin/wetype-demo" <<EOF
#!/bin/bash
ENG="\$(dirname "\$(readlink -f "\$0")")/../lib/wetype-ime/arm64"
WETYPE_ENGINE_DIR="\$ENG" bash "\$ENG/wetype-ime-demo.sh" "\$@"
EOF
chmod +x "$APPDIR/usr/bin/wetype-demo"

# 5. Fcitx5 addon, registration files, and AppImage entry point
cp "$ADDON_SO" "$APPDIR/usr/lib/fcitx5/libfcitx5-wetype.so"
cp "$BASE/fcitx5-wetype/wetype-addon.conf" "$APPDIR/usr/share/fcitx5/addon/wetype.conf"
cp "$BASE/fcitx5-wetype/wetype-im.conf" "$APPDIR/usr/share/fcitx5/inputmethod/wetype-im.conf"
cp "$BASE/scripts/appimage_manage.sh" "$APPDIR/usr/lib/wetype-ime/appimage-manage.sh"
cp "$BASE/scripts/appimage_run.sh" "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun" "$APPDIR/usr/lib/wetype-ime/appimage-manage.sh"

# 6. desktop + icon
cat > "$APPDIR/usr/share/applications/wetype-ime.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=WeType IME Engine
Comment=微信输入法引擎（候选词驱动）
Exec=wetype-demo nihao
Icon=wetype-ime
Categories=Utility;
Terminal=true
EOF
cp "$BASE/assets/wetype-ime.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/wetype-ime.png"
cp "$BASE/assets/wetype-ime.png" "$APPDIR/wetype-ime.png"
cp "$APPDIR/usr/share/applications/wetype-ime.desktop" "$APPDIR/"

# 7. 防线：AppDir 内绝不能出现 APK 派生文件
leaked="$(find "$APPDIR" \( -name 'libwxhld*' -o -name 'libandromeda*' -o -name 'libcryptopp*' \
  -o -name 'libc++_shared*' -o -name 'libime_net*' -o -name 'libowl*' -o -name 'libprotobuf-lite*' \
  -o -name 'libtensorflowlite*' -o -name 'libwcwss*' -o -name 'libwechatxlog*' \
  -o -name 'index.json' -o -name '*.bin' -o -name '*.apk' \) -print)"
if [ -n "$leaked" ]; then
  echo "AppDir 含有 APK 派生文件，拒绝打包:" >&2
  echo "$leaked" >&2
  exit 1
fi

echo "== AppDir 就绪 =="
du -sh "$APPDIR"
