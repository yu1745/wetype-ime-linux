# WeType Linux

在 x86_64 和 ARM64 Linux 上通过 Fcitx 5 使用微信输入法（WeType）引擎。引擎是 Android ARM64 版本，在两种主机上均由 QEMU user 模式运行。

> **非官方项目**，与腾讯无关，也未获其认可。WeType、微信输入法、微信是腾讯的商标。使用 WeType 引擎须遵守腾讯的相关条款。

本仓库和 AppImage 只包含本项目自己的代码，以及可再分发的运行时（QEMU、ARM64 glibc、zlib），不包含任何 WeType 文件。安装时会从腾讯官方服务器下载 WeType 3.5.4 APK，校验 SHA-256 后在本机打补丁，思路类似 Minecraft 的 Paper。

## 安装

先安装依赖：

```sh
sudo apt install python3 patchelf unzip curl          # Debian / Ubuntu
sudo dnf install python3 patchelf unzip curl          # Fedora（RHEL 上 patchelf 来自 EPEL）
sudo pacman -S --needed python patchelf unzip curl    # Arch
```

然后运行：

```sh
./WeTypeIME-Engine-$(uname -m).AppImage install       # 安装到 ~/.local；用 sudo 则安装到 /usr
```

安装完成后重启 Fcitx5，并在输入法配置中添加“微信拼音”。

其他命令：

```sh
./WeTypeIME-Engine-$(uname -m).AppImage install --apk 文件  # 使用已下载的 APK（仅支持 3.5.4）
./WeTypeIME-Engine-$(uname -m).AppImage uninstall           # 卸载（保留词库和用户数据）
./WeTypeIME-Engine-$(uname -m).AppImage demo nihao          # 命令行测试候选词
```

APK 约 214 MB，只下载一次，缓存在 `~/.cache/wetype-ime`。用户学习数据在 `~/.local/share/wetype-ime`。

## 从源码构建

在 x86_64 Debian / Ubuntu 上构建：

```sh
sudo apt install build-essential cmake libfcitx5core-dev patchelf binutils file \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu qemu-user-static libc6-arm64-cross \
  unzip python3 curl
```

还需要 ARM64 的 zlib（`zlib1g:arm64`），也可以用 `WETYPE_ZLIB_SO` 指向任意 ARM64 的 `libz.so.1`。Ubuntu 的 ARM64 软件包在 `ports.ubuntu.com`，需要先添加该源。

在 ARM64 Debian / Ubuntu 上原生构建：

```sh
sudo apt install build-essential cmake libfcitx5core-dev patchelf binutils file \
  qemu-user-static zlib1g python3 unzip curl
```

两种主机均运行：

```sh
scripts/e2_img.sh        # 构建本机架构的插件、ARM64 harness 和 AppImage（不需要 APK）
```

没有 FUSE 时（容器、虚拟机）请设置 `APPIMAGE_EXTRACT_AND_RUN=1`。打包时如果发现任何来自 APK 的文件，会拒绝打包。

在源码树中直接调试引擎：

```sh
scripts/20_build.sh          # 构建 shim 和 ARM64 harness
scripts/prepare_assets.sh    # 下载并校验 APK 到 .deps/
scripts/10_patch_libs.sh     # 修补 APK 中的库，输出到 runtime/
```

插件日志默认写入 `/tmp/wetype-harness.log`。

## 目录结构

- `fcitx5-wetype/`：Fcitx 5 插件
- `harness/`、`shim/`：ARM64 JNI 兼容层
- `scripts/`：下载、补丁、构建、打包和测试脚本

## 许可证

本项目使用 GPL-3.0-or-later，见 [LICENSE](LICENSE)。AppImage 内附带的 QEMU（GPL-2.0）、glibc（LGPL-2.1-or-later）和 zlib 的许可说明见镜像内的 `usr/share/doc/wetype-ime/THIRD-PARTY.md`。WeType 引擎和词库归腾讯所有，不在本许可范围内，本项目也不分发它们。
