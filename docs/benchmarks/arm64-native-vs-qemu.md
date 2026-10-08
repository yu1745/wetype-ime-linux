# ARM64 原生 vs 本机 qemu：候选延迟基准

## 结论

- 同一批引擎、同一参数下，ARM64 板子**原生**运行比**本机 amd64 + qemu** 快约 1.5–3.5 倍（各用例的中位数）。
- 两边硬件不同（x86 桌面 vs RK3566 开发板），比值包含硬件差异，**不能全部归因于 qemu 的开销**。要单独量化 qemu 开销，需要在同一块板子上对比 qemu 和原生（见"后续"）。
- 稳定性方面，本机 qemu 1800/1800 次正常返回；板子原生 1790/1800，10 次异常全部出现在整体模式的 `ni` 长度 40–49。
- 异常的原因**未定位**。一个未经验证的猜测：开发板的 eMMC 存储性能较弱，加上 daemon 每次请求都写 stderr 日志，可能造成偶发停顿。这只是猜测，没有做对照实验。

## 环境

| | 本机 | 开发板 |
|---|---|---|
| 硬件 | x86_64 桌面 | Orange Pi 3B，RK3566，aarch64，1.9 GB RAM |
| 系统 | Linux | Ubuntu 20.04（focal） |
| 运行方式 | `qemu-aarch64-static -L <sysroot> harness ...` | 直接运行 harness，其 ELF 解释器已用 patchelf 指向打包的 `sysroot/system/bin/linker64` |
| 引擎目录 | `~/.local/lib/wetype-ime/arm64`（harness 未打补丁） | `/opt/wetype/arm64`（由同一份引擎拷贝，harness 已打补丁） |

两边的引擎库和词库相同（同一份 APK 3.5.4 派生）。

## 方法

使用 `scripts/bench_candidate_latency.py`，参数全部取默认值：

- 模式：`incremental`（每个字符发一次 `B` 请求）和 `whole`（一次请求发送 N 个字符）
- 用例：`nihao`、`ni`、`sentence`（`nijuedexiaogouruhe`）
- 长度 1–60，每个点重复 5 次，预热 1 次
- 单次协议超时 10 秒；候选回调阈值 2500 毫秒（超过则记为 `EMPTY_TIMEOUT`）

复现：

```sh
# 本机 amd64 + qemu
python3 scripts/bench_candidate_latency.py --engine-dir ~/.local/lib/wetype-ime/arm64 \
    --qemu qemu-aarch64-static --output bench-amd64-qemu.csv

# ARM64 板子原生（harness 需已用 patchelf 指向 sysroot/system/bin/linker64）
python3 scripts/bench_candidate_latency.py --engine-dir /opt/wetype/arm64 --native \
    --output bench-arm64-native.csv
```

原始 CSV 没有入库（`.gitignore` 忽略了 `*.csv`），由上面的复现命令生成。

## 结果

### 中位数（毫秒）

比值 = 本机 qemu ÷ 板子原生。

| 模式 | 用例 | 长度 | 本机 qemu | 板子原生 | 比值 |
|---|---|---|---|---|---|
| incremental | nihao | 1 | 52.0 | 33.8 | 1.54 |
| incremental | nihao | 10 | 56.6 | 29.1 | 1.95 |
| incremental | nihao | 30 | 71.5 | 32.8 | 2.18 |
| incremental | nihao | 60 | 87.0 | 43.9 | 1.98 |
| incremental | ni | 1 | 73.0 | 31.4 | 2.32 |
| incremental | ni | 10 | 78.3 | 28.5 | 2.75 |
| incremental | ni | 30 | 87.8 | 35.8 | 2.45 |
| incremental | ni | 60 | 115.5 | 49.3 | 2.34 |
| incremental | sentence | 1 | 59.2 | 34.9 | 1.70 |
| incremental | sentence | 10 | 62.9 | 29.0 | 2.17 |
| incremental | sentence | 30 | 74.4 | 38.2 | 1.95 |
| incremental | sentence | 60 | 96.5 | 48.9 | 1.97 |
| whole | nihao | 1 | 74.2 | 34.7 | 2.14 |
| whole | nihao | 10 | 152.6 | 60.0 | 2.54 |
| whole | nihao | 30 | 355.6 | 149.2 | 2.38 |
| whole | nihao | 60 | 1058.9 | 361.2 | 2.93 |
| whole | ni | 1 | 79.1 | 33.7 | 2.35 |
| whole | ni | 10 | 151.2 | 56.8 | 2.66 |
| whole | ni | 30 | 482.8 | 174.5 | 2.77 |
| whole | ni | 60 | 974.1 | 375.5 | 2.59 |
| whole | sentence | 1 | 81.7 | 39.9 | 2.05 |
| whole | sentence | 10 | 132.4 | 64.8 | 2.04 |
| whole | sentence | 30 | 374.4 | 172.6 | 2.17 |
| whole | sentence | 60 | 859.8 | 402.4 | 2.14 |

整体模式大致随长度线性增长：板子上约 6 毫秒每个字符（`nihao` 长度 60 为 361 毫秒）。增量模式基本持平，每次只处理新增的一个字符，板子上的中位数在 26–53 毫秒之间。

### 可靠性

| | 本机 qemu | 板子原生 |
|---|---|---|
| 正常返回 | 1800 / 1800 | 1790 / 1800 |
| 异常 | 无 | 9 次 `EMPTY_TIMEOUT`（响应 2.7–5.2 秒）+ 1 次 `CRASH_OR_TIMEOUT`（长度 49，10 秒无响应，daemon 重启一次） |
| 异常位置 | — | 仅整体模式 `ni`，长度 40–49，每个长度 1 次（第 1 次重复） |

同样的长度（`whole ni` 40–49），本机 qemu 的中位数为 528–704 毫秒，但**没有超时**。所以板子上的失败更像是偶发停顿，而不是持续变慢。

板子上 `whole ni` 在长度 28–39 的中位数为 155–219 毫秒，但 p90 有 0.9–3.3 秒的尖峰。本机 qemu 在同一区间的 p90 为 0.45–0.9 秒，没有出现秒级的尖峰。

## 未解决的问题

- **板子超时的原因未定位。** 猜测：eMMC 存储性能较弱，而 daemon 的 stderr 日志（`WETYPE_HARNESS_LOG`）在每次请求时写入，两次运行各约 400 MB。本机的日志写在 SSD 上，不会有同样的影响。未经对照实验验证。一个可行的验证方法是把日志改到 tmpfs（如 `/dev/shm`）或 `/dev/null` 后重跑板子上的基准，观察超时是否消失。
- **qemu 开销未隔离。** 本机和板子的 CPU 不同。要得到干净的 qemu 与原生对比，应在板子上也安装 `qemu-user-static`，用同一台机器跑两次。

## 环境说明

- 同一台板子上的 fcitx5 端到端测试（隔离的 D-Bus + fcitx5 5.1.7 + 原生 harness）全部通过：分页 19/19，标点 24/24，英文释义 21/21，异步空格 20/20。引擎守护进程回归 5/5（原生）。
- fcitx5 为交叉编译产物（ubuntu:20.04 容器内，目标 focal arm64）。使用 5.1.7 而非 5.1.19，因为 gcc-10 不支持 5.1.19 需要的 `<source_location>`。编译时去掉了 spell 模块的词典下载和 appdata 生成步骤。这些是测试构建的取舍，不影响候选逻辑。交叉编译脚本未纳入仓库。
- 板子使用的 fcitx5 运行时依赖：`libjson-c4`（需额外安装），其余（dbus、uuid、uv、xkbcommon、expat、systemd）由 focal 提供。
- 本报告中的板子引擎是手动用 patchelf 处理的。之后 ARM64 AppImage 的安装器已实现这一步（`install` 时执行 `patchelf --set-interpreter`），并在同一块板子上用安装后的引擎重跑了回归和 e2e。
