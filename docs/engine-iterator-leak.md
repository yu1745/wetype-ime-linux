# 搜索协程簇泄漏：恢复 iterator 所有权，不打 ELF 补丁

## 结论

安全路径在 harness/JNI 边界，而不是裸 free 或替引擎执行任务。
`create_session` 已注册 JNI listener；daemon 原来额外注册的 native 探针
也会收到一个独立的 candidate iterator，却只用它作为通知，完全忽略其所有权。
与此同时，harness 查找的 JNI 析构符号长度错误：`_Z17...` 应为 `_Z25...`。
两个问题必须一起修，单改符号不足以回收被第二个 listener 持有的搜索图。

现在 daemon 直接在假 JNI 的 `WxhldApi.onBatchEvent` 处通知候选队列，沿用
`SetLongField(newIterator)` / PendingInput 字段钩子捕获并复制数据，不再增加
native listener。当前 iterator 在换页/新候选替换、丢弃迟到结果、C、Q 时通过
官方 JNI 析构 API 归还。缺少析构符号时启动失败，不再静默接受泄漏。
不执行额外任务、不遍历或裸释放引擎堆、不重启进程；GC 默认仍为 3000ms。
非 daemon 的一次性诊断探针保持原状，此修复针对长期运行的 daemon。

## 调用级与分配级证据

* JNI 动态导出：`_Z25delete_candidate_iteratorP7_JNIEnvP8_jobjectl @0x3706c`。
  其全部正常指令为 `mov x0,x2; b wxime_delete_candidate_iterator@plt`，
  ABI 是 `void(JNIEnv*, jobject, long)`，不是任意协程地址上的析构蹦床。
* Core `wxime_delete_candidate_iterator @0x918618`：取 iterator+8 的
  shared control block、原子递减 shared count；到零时通过 vtable+0x10
  执行引擎析构，再 release_weak，最后 operator delete iterator。
  引擎自身处理多态容器、协程取消/析构及内部引用，不需要推测 TTL 摘链。
* 基线 `c_callback_holder.h:154` 对同一 version 每次出现两条不同
  `malloc new_iterator`；修复后每个事件只有一条。新增配对回归验证原始
  allocation/deletion 日志的多重集（包含地址复用，不能只比较地址集合）。
* 对三个连续 B 请求分别在安全点取 malloc 调用点账本，
  `+0xb92a80` 的 0x1720 帧始终只有 **1** 个（当前 iterator 的惰性结果），
  `+0xb386cc/+0xe92024/+0xb8ba14` 始终各 **1** 个；C 后均为 **0**。
  `+0xb59b0c` 的两个向量缓冲在 C 后也为 0；`+0xb3c394` 留 4704B
  缓冲，但不随请求增长。这是正确持有与正确析构，不是泄漏簇强制回收。
* 原版三轮采样记录了 **5725** 次 72B operator-new，fifo impl 分配点
  `+0x1f7ad60` 命中 **0** 次。静态分支/重定位交叉引用也仅发现
  run_all_tasks 内部调用 try_run_one_task 的 PLT；没有搜索调用 fifo 的
  证据。原“每搜索遗弃 2–3 个 fifo”的解释不成立。
* 旧 drain 蹦床未重放被覆盖的 `stp x29,x30,[sp,#-0x30]!`，而直接跳到
  `gc_sessions+4`。GC_OFF 对照在首次 gc 返回就产生 ASCII fault，尚未
  进入排队任务执行（gen<2）。另有未登记 delete 被吞、任务预算放在
  caller-saved x5、代际减法下溢等错误。实验副本修正 ABI 后 GC_OFF
  12 轮无崩溃，但仍 2196 kB/req；不能用旧崩溃证明瞬态对象 UAF。
  旧 free blob 也有 delete 透传与 x21 保存问题。研究 blob 继续默认拒打。

## 测量（kB/req，全部使用私有 engine；无 preload 的 RSS 测试）

| harness / 试验 | 轮数 | 总增长/请求 | 前半 / 后半斜率 |
|---|---:|---:|---:|
| 基线 | 30 | 1549 | 1762 / 1337 |
| 只修析构符号，保留额外 listener | 30 | 1486 | 1771 / 1201 |
| 同上 | 100 | 1358 | 1422 / 1294 |
| JNI-only + 正确析构（冷启动短测） | 30 | 351 | 728 / -26 |
| 同上 | 100 | 143 | 214 / 72 |
| 同上，单字 n | 30 | 594 | 653 / 534 |
| 诊断：GC_MS=0（未采用） | 30 | 375 | 748 / 2 |
| 诊断：bionic M_PURGE（未采用） | 30 | 397 | 793 / 1 |
| **最终默认配置、原判据** | **300** | **52** | **104 / 1** |

最终 300 次请求在同一进程、同一会话中持续执行 B（除测试原有 warm-up/C），
未重启或通过每请求 C 摊掉泄漏。匿名 RSS：**56040 → 71744 kB**，
增长 **15704 kB**；最后 C 后 **65380 kB**。后半 150 轮斜率为 1，
说明 30/100 轮冷启动中的一次性缓存/堆高水位增长并非剩余线性泄漏；
短测仍不满足阈值，不能把它们报成 PASS。300 轮满足原 `<100` 判据，
同时覆盖要求的 100 轮长稳，无 SIGSEGV 或 watchdog 挂死。
100 轮日志的 iterator 为 **200/200**，300 轮日志为 **602/602**；
20 次五种短输入配对回归为 **35/35**。基线负对照五次输入剩余 **16** 个
iterator，零归还，按预期 FAIL。

功能回归全部 PASS，额外包括 **85 秒空闲后输入**、部分选词、学习与
持久化（重启仅用于原功能测试的持久化验证，不是修复策略）。候选对拍
完整 CAND 字节一致：nihao **436B**、zhongguo **454B**、
nihaozhongguo **446B**、xiao **431B**、n **424B**。

引擎相对实验基线字节不变，`.orig` 与运行文件 SHA256 均为
`6002395d6509b49bc7551a4c970aee79b1400f5ee3814fe5da3d6626ffe421d3`。
没有写入用户安装目录；旧风险 blob、临时 purge/GC 参数均未作为最终修复。

## 复测

在包含本修复的 worktree 内运行；仅从安装读取引擎到私有 lab：

```sh
bash scripts/make_leak_lab.sh /tmp/wetype-fix
NDK=/home/wangyu/wetype-ime-linux/.deps/tools/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
$NDK/clang --target=aarch64-linux-android28 -O0 -g \
  -o /tmp/wetype-fix/bin/jinterop harness/jinterop.c -ldl
/tmp/wetype-fix/wrun python3 /wt/scripts/test_memory_leak.py \
  --engine-dir /ws/engine --harness /ws/bin/jinterop --rounds 300
/tmp/wetype-fix/wrun python3 /wt/scripts/test_bionic_daemon.py \
  --engine-dir /ws/engine --harness /ws/bin/jinterop --idle-seconds 85
/tmp/wetype-fix/wrun python3 /wt/scripts/test_iterator_ownership.py \
  --engine-dir /ws/engine --harness /ws/bin/jinterop
```

对拍需事先把**未修复的 harness** 编为 `/tmp/wetype-fix/bin/jinterop-baseline`，
不可拿两个修复版冒充基线。两次运行使用独立空用户词库：

```sh
/tmp/wetype-fix/wrun python3 /wt/scripts/test_candidate_parity.py \
  --engine-dir /ws/engine --before /ws/bin/jinterop-baseline --after /ws/bin/jinterop
```

临时证据日志位于 `/tmp/wetype-fix/tmp/`：baseline30.log、fixed30.log、
fixed100.log、event30.log、event100.log、final300.log、final-e2e.log、
snapshot-baseline.log、snapshot.log、fifo-verification.txt、delete-jni.txt、
delete-core.txt。前序/最终的测量均明确区分，不能用插桩后的 RSS 代替正式判据。
