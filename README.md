# WeType Linux

在 Linux 上通过 Fcitx 5 使用微信输入法（WeType）引擎。引擎是 Android ARM64 版本，由 QEMU user 模式运行。

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
./WeTypeIME-Engine-x86_64.AppImage install            # 安装到 ~/.local；用 sudo 则安装到 /usr
```

安装完成后重启 Fcitx5，并在输入法配置中添加“微信拼音”。

**候选翻页**：组词时 `-` `=`（以及 `PgUp`/`PgDn`、`↑`/`↓`）按整页翻页，到达边界也不会把这两个字符漏进文档；每页候选数跟随 Fcitx5 全局设置“候选词数量”（1–10，默认 5），候选前显示的数字即选词键（每页 10 个时第 10 个为 `0`）。

其他命令：

```sh
./WeTypeIME-Engine-x86_64.AppImage install --apk 文件   # 使用已下载的 APK（仅支持 3.5.4，其他版本未经测试，会被拒绝）
./WeTypeIME-Engine-x86_64.AppImage uninstall            # 卸载（保留词库和用户数据）
./WeTypeIME-Engine-x86_64.AppImage demo nihao           # 命令行测试候选词
```

APK 约 214 MB，只下载一次，缓存在 `~/.cache/wetype-ime`。用户学习数据在 `~/.local/share/wetype-ime`。

## 从源码构建

构建环境为 x86_64 的 Debian / Ubuntu：

```sh
sudo apt install build-essential cmake libfcitx5core-dev patchelf binutils file \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu qemu-user-static libc6-arm64-cross \
  unzip python3 curl
```

还需要 ARM64 的 zlib（`zlib1g-dev:arm64`），也可以用 `WETYPE_ZLIB_SO` 指向任意 ARM64 的 `libz.so.1`。Ubuntu 的 ARM64 软件包在 `ports.ubuntu.com`，需要先添加该源。

```sh
scripts/e2_img.sh        # 构建插件、harness 并打包 AppImage（不需要 APK）
```

没有 FUSE 时（容器、虚拟机）请设置 `APPIMAGE_EXTRACT_AND_RUN=1`。打包时如果发现任何来自 APK 的文件，会拒绝打包。

在源码树中直接调试引擎：

```sh
scripts/20_build.sh          # 构建 shim 和 ARM64 harness
scripts/prepare_assets.sh    # 下载并校验 APK 到 .deps/
scripts/10_patch_libs.sh     # 修补 APK 中的库，输出到 runtime/
```

插件日志默认写入 `/tmp/wetype-harness.log`。

## 可选候选英文释义

将 UTF-8 词表放到 `~/.local/share/wetype-ime/glossary-en.tsv`，设置了 `XDG_DATA_HOME` 时则放到 `$XDG_DATA_HOME/wetype-ime/glossary-en.tsv`。重启 Fcitx5 后加载，无网络请求。词表格式为 `中文词<TAB>[词性. ]英文释义`，例如：

```text
你好	n. hello
世界	n. world
```

兼容 [qingjian](https://github.com/qingjian-team/qingjian) 的 `glossary-en.tsv` 格式；本仓库不附带词表。匹配到的释义以斜体追加在候选后，仅供展示，不改变候选顺序、选词键或上屏内容。缺少词表或未匹配到词条时，候选显示不变。候选数量仍跟随 Fcitx5 全局配置。

此项仅引入英文释义，不改变现有标点或空格提交行为。

## 目录结构

- `fcitx5-wetype/`：Fcitx 5 插件
- `harness/`、`shim/`：ARM64 JNI 兼容层
- `scripts/`：下载、补丁、构建、打包和测试脚本

## 许可证

本项目使用 GPL-3.0-or-later，见 [LICENSE](LICENSE)。AppImage 内附带的 QEMU（GPL-2.0）、glibc（LGPL-2.1-or-later）和 zlib 的许可说明见镜像内的 `usr/share/doc/wetype-ime/THIRD-PARTY.md`。WeType 引擎和词库归腾讯所有，不在本许可范围内，本项目也不分发它们。
