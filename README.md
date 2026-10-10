# WeType Linux

在 Linux 上通过 Fcitx 5 使用微信输入法（WeType）引擎。引擎是 Android ARM64 版本，运行在 Android 自己的 C 运行时（AOSP bionic）上：x86_64 主机通过 QEMU user 模式运行，ARM64 主机原生运行，不需要 QEMU。

> **非官方项目**，与腾讯无关，也未获其认可。WeType、微信输入法、微信是腾讯的商标。使用 WeType 引擎须遵守腾讯的相关条款。

本仓库和 AppImage 只包含本项目自己的代码，以及可再分发的运行时（取自 AOSP 系统镜像的 bionic；x86_64 版另含 QEMU），不包含任何 WeType 文件。安装时会从腾讯官方服务器下载 WeType 3.5.4 APK，校验 SHA-256 后在本机打补丁，思路类似 Minecraft 的 Paper。

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
| 逗号、句号按中英上下文切换 | 支持 | 组词时按本次提交文本；空态按当前输入上下文 |

本地词库和学习数据的保存不等于云同步。学习数据默认位于 `~/.local/share/wetype-ime/dict`，设置了 `XDG_DATA_HOME` 时使用 `$XDG_DATA_HOME/wetype-ime/dict`；高级调试可用 `WETYPE_WORK_DIR` 覆盖。

发布两种 AppImage：`WeTypeIME-Engine-x86_64.AppImage`（x86_64 Linux，自带 QEMU）和 `WeTypeIME-Engine-aarch64.AppImage`（ARM64 Linux，原生运行，不含 QEMU）。Fcitx5 插件在 Ubuntu 24.04 上随包构建，要求宿主有 glibc ≥ 2.38 和 Fcitx5 5.1.x（`libFcitx5Core.so.7`）；更旧的发行版（如 Ubuntu 20.04）请用 `fcitx5-wetype/build.sh` 按本机 Fcitx5 从源码构建插件。默认在缺少有效 APK 缓存时联网下载；也可通过 `--apk` 使用已下载的 APK。英文释义是本地查表，不进行网络查询。上述功能清单不构成对专有引擎内部网络行为的保证。

## 安装

先安装依赖：

```sh
sudo apt install python3 unzip curl          # Debian / Ubuntu
sudo dnf install python3 unzip curl          # Fedora
sudo pacman -S --needed python unzip curl    # Arch
```

ARM64 主机还需要 `patchelf`（安装时把 harness 的 ELF 解释器指向包内的 bionic `linker64`，之后原生运行，不改动 `/system`）：

```sh
sudo apt install patchelf                    # Debian / Ubuntu；Fedora、Arch 同名
```

然后运行：

```sh
./WeTypeIME-Engine-$(uname -m).AppImage install       # 安装到 ~/.local；用 sudo 则安装到 /usr
```

安装完成后重启 Fcitx5，并在输入法配置中添加“微信拼音”。

**候选翻页**：组词时 `-` `=`（以及 `PgUp`/`PgDn`、`↑`/`↓`）按整页翻页，到达边界也不会把这两个字符漏进文档；每页候选数跟随 Fcitx5 全局设置“候选词数量”（1–10，默认 5），候选前显示的数字即选词键（每页 10 个时第 10 个为 `0`）。

其他命令：

```sh
./WeTypeIME-Engine-$(uname -m).AppImage install --apk 文件   # 使用已下载的 APK（仅支持 3.5.4，其他版本未经测试，会被拒绝）
./WeTypeIME-Engine-$(uname -m).AppImage uninstall            # 卸载（保留词库和用户数据）
./WeTypeIME-Engine-$(uname -m).AppImage demo nihao           # 命令行测试候选词
```

APK 约 214 MB，只下载一次，缓存在 `~/.cache/wetype-ime`。用户学习数据在 `~/.local/share/wetype-ime`。

## 从源码构建

x86_64 AppImage 的构建环境为 x86_64 的 Debian / Ubuntu：

```sh
sudo apt install build-essential cmake libfcitx5core-dev file qemu-user-static \
  e2fsprogs unzip python3 curl
```

harness 使用 Android NDK r27c（27.2.12479018）交叉编译为 Android ARM64/API 28；Fcitx5 插件仍由宿主 C++ 编译器原生构建，无需 Android Studio、Java 或完整 Android SDK。

```sh
bash scripts/prepare_ndk.sh  # 从 Google 下载到 .deps/tools/，按官方校验值检查；后续复用缓存
export ANDROID_NDK_HOME="$PWD/.deps/tools/android-ndk-r27c"
```

也可用 `ANDROID_NDK_HOME` 指向已有兼容 NDK；不设置时构建脚本回落到上述项目缓存。CI 固定使用 r27c。构建时还会下载 AOSP Android 9 的 ARM64 模拟器系统镜像（约 407 MB，缓存在 `.deps/aosp/`），校验 SHA-256 后提取 bionic 运行时。NDK 只有 x86_64 Linux 主机版，所以 harness 与 bionic 运行时总是在 x86_64 主机上构建；ARM64 AppImage 复用这些产物在 ARM64 主机上打包（见下）。

```sh
scripts/e2_img.sh        # 构建插件、harness 并打包 AppImage（不需要 APK）
```

没有 FUSE 时（容器、虚拟机）请设置 `APPIMAGE_EXTRACT_AND_RUN=1`。打包时如果发现任何来自 APK 的文件，会拒绝打包。

**ARM64 AppImage**在 ARM64 主机上打包：先在 x86_64 主机上构建并导出 harness 与运行时，再到 ARM64 主机上用同一份源码打包。CI 自动完成这两步。

```sh
# x86_64 主机
bash scripts/20_build.sh
tar -cf engine-runtime.tar runtime/libandroid.so runtime/sysroot harness/jinterop

# ARM64 主机（需要 build-essential cmake libfcitx5core-dev file patchelf curl）
tar -xf engine-runtime.tar
WETYPE_SKIP_NDK_BUILD=1 scripts/e2_img.sh        # 生成 WeTypeIME-Engine-aarch64.AppImage，包内不含 QEMU
scripts/smoke_native_harness.sh AppDir/usr/lib/wetype-ime/arm64   # 原生冒烟测试，不需要 APK
```

在源码树中直接调试引擎：

```sh
scripts/20_build.sh          # 用 NDK 构建 harness 和 libandroid.so 替身，提取 bionic 到 runtime/sysroot
scripts/prepare_assets.sh    # 下载并校验 APK 到 .deps/
scripts/10_patch_libs.sh     # 拷贝 APK 中的引擎库并打补丁，输出到 runtime/
```

插件日志默认写入 `$XDG_STATE_HOME/wetype-ime/harness.log`（未设置时 `~/.local/state/wetype-ime/harness.log`），超过 16 MiB 自动轮转为 `.1`；设 `WETYPE_HARNESS_LOG` 可改到别处（如 `/dev/null` 关闭日志）。

可无桌面验证已安装的引擎（使用临时学习目录，不修改实际用户词库）：

```sh
python3 scripts/test_bionic_daemon.py --engine-dir "$HOME/.local/lib/wetype-ime/arm64" --idle-seconds 85
```

ARM64 主机上加 `--native`（不经 QEMU；回归使用临时拷贝并用 `patchelf` 设置解释器，不改动已安装的 harness）。ARM64 与 x86_64 的候选延迟对比见 [docs/benchmarks/arm64-native-vs-qemu.md](docs/benchmarks/arm64-native-vs-qemu.md)。

此回归覆盖候选、部分选词、学习、重置、长空闲恢复及重启持久化。插件及词表解析测试使用 `cmake -S fcitx5-wetype -B build && cmake --build build && ctest --test-dir build --output-on-failure`。

## 可选候选英文释义

将 UTF-8 词表放到 `~/.local/share/wetype-ime/glossary-en.tsv`，设置了 `XDG_DATA_HOME` 时则放到 `$XDG_DATA_HOME/wetype-ime/glossary-en.tsv`。重启 Fcitx5 后加载，无网络请求。词表格式为 `中文词<TAB>[词性. ]英文释义`，例如：

```text
你好	n. hello
世界	n. world
```

兼容 [qingjian](https://github.com/qingjian-team/qingjian) 的 `glossary-en.tsv` 格式；本仓库不附带词表。匹配到的释义以斜体追加在候选后，仅供展示，不改变候选顺序、选词键或上屏内容。缺少词表或未匹配到词条时，候选显示不变。候选数量仍跟随 Fcitx5 全局配置。

英文释义只影响展示；标点与异步空格规则见下节。

## 智能逗号与句号

- 组词时按 `,` 或 `.`，依据**本次实际提交的候选或原始拼音**选择全角/半角，不使用上一条提交的语言。
- 没有组词时，应用提供的有效周围文本用于更新光标前上下文；不提供周围文本的客户端使用**同一个输入上下文**最近提交的文本。光标起点、无历史或周围文本失效按未知处理。已知中文/CJK 上下文输出 `，。`，英文或未知上下文保留原始半角按键。
- 周围文本只在应用更新时刷新；刚提交的文本不会被尚未更新的旧周围文本覆盖。不同输入上下文的状态不共享。
- 无周围文本时，透传数字也会使后续句点保持半角，以免把小数点变成句号。应用标记的密码、邮箱、URL、数字等结构化字段保留半角分隔符。
- 只转换逗号和句号，不转换其他标点；快捷键及按键释放仍透传。没有接入账号或网络查询。

## 异步空格选词

- 拼音的最新候选尚未返回时，空格会等待该次候选，而不是立即提交原始拼音；旧前缀预览不能代替当前候选。
- 等待期间后续可打印按键及释放按顺序排队。一次空格只选择一个候选片段；连续空格可继续选择剩余拼音，组词结束后的空格正常透传。
- 退格先删除排队的最新输入；队列没有输入时删除拼音，并继续等待编辑后的候选。Esc/reset 取消整个未提交组词。
- Enter、无候选或恢复失败时按原文提交，包括已输入的空格与后续文字。等待最多 5 秒，队列最多 256 个按键事件；到限同样原文提交，不静默丢字。
- 导航或快捷键先结束等待、提交原文，再透传，避免后续文字落到移动后的光标处。切换输入法也保留完整原文；切换应用沿用不额外上屏的行为，预编辑包含排队文字，最终是否上屏由前端处理。

可在私有 D-Bus 与临时配置中运行回归（不修改桌面输入法）：

```sh
WETYPE_ENGINE_DIR=/path/to/engine WETYPE_E2E_KEYBOARD=1 \
WETYPE_E2E_SCRIPT="$PWD/fcitx5-wetype/e2e/pending_space_test.py" \
bash fcitx5-wetype/e2e/run_paging_e2e.sh 5
```

`pending_engine_fixture.py` 是仅供隔离测试的协议响应器；用 `QEMU_AARCH64` 指向它，配合 `WETYPE_PENDING_FIXTURE=empty|error|skip|startup-failure` 和 `pending_failure_test.py` 验证确定性失败路径，不代表真实引擎测试。

## 目录结构

- `fcitx5-wetype/`：Fcitx 5 插件
- `harness/`：伪造 JNI 环境、驱动引擎的 ARM64 程序
- `shim/`：`libandroid.so` 替身
- `scripts/`：下载、补丁、构建、打包和测试脚本

## 许可证

本项目使用 GPL-3.0-or-later，见 [LICENSE](LICENSE)；`harness/jni.h` 取自 AOSP Android 9，来源见 [JNI 声明说明](licenses/jni/README.md)，完整 [Apache-2.0 许可证](licenses/Apache-2.0.txt) 随源码及 AppImage 附带。x86_64 AppImage 内附带的 QEMU（GPL-2.0）和两种 AppImage 内的 AOSP 组件（bionic、liblog、libc++、zlib）的许可说明见镜像内的 `usr/share/doc/wetype-ime/THIRD-PARTY.md`。WeType 引擎和词库归腾讯所有，不在本许可范围内，本项目也不分发它们。
