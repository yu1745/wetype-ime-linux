# WeType Linux

在 Linux 上通过 Fcitx 5 使用微信输入法（WeType）引擎。引擎是 Android ARM64 版本，由 QEMU user 模式运行在 Android 自己的 C 运行时（AOSP bionic）上。

> **非官方项目**，与腾讯无关，也未获其认可。WeType、微信输入法、微信是腾讯的商标。使用 WeType 引擎须遵守腾讯的相关条款。

本仓库和 AppImage 只包含本项目自己的代码，以及可再分发的运行时（QEMU、取自 AOSP 系统镜像的 bionic），不包含任何 WeType 文件。安装时会从腾讯官方服务器下载 WeType 3.5.4 APK，校验 SHA-256 后在本机打补丁，思路类似 Minecraft 的 Paper。

## 功能与限制

本项目只把微信输入法的 Android 引擎接入 Fcitx 5，**不是官方客户端的完整移植**。当前能力如下：

| 功能 | 状态 | 说明 |
|---|---|---|
| 键盘拼音输入、中文候选 | 支持 | 候选由 WeType 3.5.4 引擎产生 |
| 数字、空格、鼠标选词及部分选词 | 支持 | 部分选词后可继续选择剩余拼音的候选 |
| 候选翻页、全局候选数量 | 支持 | 每页 1–10 个，跟随 Fcitx5 全局配置；第 10 个用 `0` 选择 |
| 本地选词学习及持久化 | 支持 | 学习数据保存在本机；重启后可复用 |
| 候选英文释义 | 可选 | 用户提供本地 TSV 词表，仅影响显示，不改变上屏内容 |
| 跨设备剪贴板、跨设备粘贴 | 未实现 | 未接入跨设备通信或官方剪贴板功能；不影响系统本地复制粘贴 |
| 官方账号登录、云端词库及跨设备同步 | 未接入 | 本项目没有提供登录或同步接口，不能使用官方客户端的同步流程 |
| 按中英上下文自动切换标点 | 未引入 | 仍采用现有标点处理行为 |

本地词库和学习数据的保存不等于云同步。学习数据默认位于 `~/.local/share/wetype-ime/dict`，设置了 `XDG_DATA_HOME` 时使用 `$XDG_DATA_HOME/wetype-ime/dict`；高级调试可用 `WETYPE_WORK_DIR` 覆盖。

当前安装包及构建目标为 **x86_64 Linux**。引擎是 ARM64 二进制，不代表本项目已提供 ARM64 Linux 的完整输入法安装包。默认在缺少有效 APK 缓存时联网下载；也可通过 `--apk` 使用已下载的 APK。英文释义是本地查表，不进行网络查询。上述功能清单不构成对专有引擎内部网络行为的保证。

## 安装

先安装依赖：

```sh
sudo apt install python3 unzip curl          # Debian / Ubuntu
sudo dnf install python3 unzip curl          # Fedora
sudo pacman -S --needed python unzip curl    # Arch
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
sudo apt install build-essential cmake libfcitx5core-dev file qemu-user-static \
  e2fsprogs unzip python3 curl
```

harness 使用 Android NDK r27c（27.2.12479018）交叉编译为 Android ARM64/API 28；Fcitx5 插件仍由宿主 C++ 编译器原生构建，无需 Android Studio、Java 或完整 Android SDK。

```sh
bash scripts/prepare_ndk.sh  # 从 Google 下载到 .deps/tools/，按官方校验值检查；后续复用缓存
export ANDROID_NDK_HOME="$PWD/.deps/tools/android-ndk-r27c"
```

也可用 `ANDROID_NDK_HOME` 指向已有兼容 NDK；不设置时构建脚本回落到上述项目缓存。CI 固定使用 r27c。构建时还会下载 AOSP Android 9 的 ARM64 模拟器系统镜像（约 407 MB，缓存在 `.deps/aosp/`），校验 SHA-256 后提取 bionic 运行时。本构建方案要求 x86_64 Linux 主机，不包含 ARM64 原生构建或 ARM64 AppImage 支持。

```sh
scripts/e2_img.sh        # 构建插件、harness 并打包 AppImage（不需要 APK）
```

没有 FUSE 时（容器、虚拟机）请设置 `APPIMAGE_EXTRACT_AND_RUN=1`。打包时如果发现任何来自 APK 的文件，会拒绝打包。

在源码树中直接调试引擎：

```sh
scripts/20_build.sh          # 用 NDK 构建 harness 和 libandroid.so 替身，提取 bionic 到 runtime/sysroot
scripts/prepare_assets.sh    # 下载并校验 APK 到 .deps/
scripts/10_patch_libs.sh     # 拷贝 APK 中的引擎库并打补丁，输出到 runtime/
```

插件日志默认写入 `/tmp/wetype-harness.log`。

可无桌面验证已安装的引擎（使用临时学习目录，不修改实际用户词库）：

```sh
python3 scripts/test_bionic_daemon.py --engine-dir "$HOME/.local/lib/wetype-ime/arm64" --idle-seconds 85
```

此回归覆盖候选、部分选词、学习、重置、长空闲恢复及重启持久化。插件及词表解析测试使用 `cmake -S fcitx5-wetype -B build && cmake --build build && ctest --test-dir build --output-on-failure`。

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
- `harness/`：伪造 JNI 环境、驱动引擎的 ARM64 程序
- `shim/`：`libandroid.so` 替身
- `scripts/`：下载、补丁、构建、打包和测试脚本

## 许可证

本项目使用 GPL-3.0-or-later，见 [LICENSE](LICENSE)；`harness/jni.h` 取自 AOSP Android 9，来源见 [JNI 声明说明](licenses/jni/README.md)，完整 [Apache-2.0 许可证](licenses/Apache-2.0.txt) 随源码及 AppImage 附带。AppImage 内附带的 QEMU（GPL-2.0）和 AOSP 组件（bionic、liblog、libc++、zlib）的许可说明见镜像内的 `usr/share/doc/wetype-ime/THIRD-PARTY.md`。WeType 引擎和词库归腾讯所有，不在本许可范围内，本项目也不分发它们。
