#!/usr/bin/env python3
"""30_patch_leak.py — libwxhld.so 每搜索 ~1.2MB 泄漏：补丁生成器（研究版）。

★ 结论先行：本仓库验证表明该泄漏【无法在不破坏正确性的前提下二进制补丁】，
  本脚本默认拒绝打补丁（--accept-risk 显式确认才生成）。
  两个已实现并实测的策略均会破坏引擎（数据见本文件"实验记录"与最终报告）：
  · free  策略：代际隔离后裸 operator delete 泄漏簇 → 第 10 次搜索挂死
               （watchdog：簇被引擎后续异步路径重新遍历/恢复）。
  · drain 策略：替引擎排空被遗弃的 fifo_scheduler 队列 → 第 2 次搜索 SIGSEGV
               （排入的任务捕获了搜索瞬态对象，晚执行=use-after-free）。

根因（详见报告）：每次搜索异步任务图（协程帧+闭包簇，~1.2MB）滞留：
  · 堆引用图扫描（mtrace2_heapscan）：簇外无引用（libwxhld 全局段+全堆均无），
    根闭包悬空 —— 由挂起的协程栈/队列持有；
  · 引擎自带析构（0xb8bce0/0xb8e260/…）从不执行（39600B 块计数线性增长、
    永不释放）；gc_sessions/reset_cloud_svr_cache 等 6 个 API 实测均不回收；
  · 每搜索 2-3 个 72B fifo_scheduler impl（0x1f7ad60 分配）连同其任务队列
    被遗弃：任务永不执行 → future 永不兑现 → 协程永不 final_suspend。

本脚本价值：可复现实验基座。--strategy free|drain --accept-risk 生成补丁 so，
配合 scripts/drive_mtrace.py / mtrace2_heapscan.c 复测两条路线的失败方式。

用法：
  python3 30_patch_leak.py                     # 打印结论并退出(2)
  python3 30_patch_leak.py --strategy drain --accept-risk [--in ...] [--out ...]
补丁布局（两策略共用）：
  · cave 代码 @0x2330000：LOAD1 p_filesz/p_memsz 尾部扩展（文件追加，RX）
  · 数据区 @0x22F7000..0x2318000：LOAD3 p_memsz 扩展（RW 零页）
    +0 u32 gen；+8 u64 合成 outer 槽；+0x1000 8192×16B 隔离表
  · _Znwm@plt(0x21ca260)→cave_alloc（按 LR 识别泄漏调用点，其余透传）
  · _ZdlPv@plt(0x21ca200)→cave_free（防御：隔离块被引擎释放时移表转发）
  · wxime_gc_sessions(0x90a76c) 首指令→cave_gc_entry 蹦床
幂等：输出已是补丁版时校验后直接成功。
"""
import argparse
import base64
import struct
import sys
from pathlib import Path

BUILD_ID = "b9b47a045eda5bf5c31cbf9c3c67b2f1226396ce"
CAVE = 0x2330000
DATA = 0x22F7000
DATA_END = 0x2318000
ZNWM_PLT = 0x21CA260
ZDLPV_PLT = 0x21CA200
GC_SESSIONS = 0x90A76C
GC_RET = 0x90A770
NOP = bytes.fromhex("1f2003d5")
ORIG_ZNWM_STUB = bytes.fromhex("300700d0" "11be40f9" "10e20591" "20021fd6")
ORIG_ZDLPV_STUB = bytes.fromhex("300700d0" "11a640f9" "10220591" "20021fd6")
ORIG_GC_FIRST = bytes.fromhex("fd7bbda9")   # stp x29,x30,[sp,#-0x30]!

# 两策略的 cave blob（源码见 leak_cave_drain.S / leak_cave_free.S，
# 由 NDK clang -Ttext=0x2330000 固定地址汇编）。占位 mov x9,#0x5A5A
# 会被本脚本改写为 b 0x90a770。
STRATEGIES = {
    # 排空遗弃 fifo：挂 gc 排空上一代以前队列；impl 不释放。
    # 实测：第 2 次搜索 SIGSEGV（任务捕获搜索瞬态对象）。
    "drain": dict(blob_len=0x258, off_alloc=0x0, off_free=0xC4,
                  off_gc=0x15C, off_b_gc=0x240, b64="/Xu/qfNTv6nzAx6qEPz/kBG+QPkgAj/W9AMAqtQEALQK//8Qa0ag0ksBC8sMAACQjCEJkS0AgNKOhUD4zgELi38CDuuAAABUrQUA8WH//1QZAAAUKf7/8C/934jvfWDTiv5E0+oDCiopNo9S6cazckp9CRtKMUDTK/7/8GsFQJENBYDSbBEKi45Bf8gOAQC03wUA8cAAAFRKBQCRSjFA060FAPGAAABU9///F5Q9Mcix/v814AMUqvNTwaj9e8GowANf1v17vqnzCwD5IAQAtPADAKoK/ETT6gMKKik2j1LpxrNySn0JG0oxQNMr/v/wawVAkQ8FgNIOAIDSbBEKi41Nf8htAQC0vwEQ66EAAFQtAIDSjU0xyDH//zUuAIDSSgUAkUoxQNPvBQDxgf7/VK4AALTgAxCqEvz/kFKmQPlAAj/W8wtA+f17wqjAA1/W8wtA+f17wqjAA1/W/Xu5qeAHAanzUwKp9VsDqfdjBKn5awWp+3MGqVb0/xBpRqDS1gIJyyn+//A0/d+IlAYAETT9n4ifCgBxYwQAVJUGAFEz/v/wcyIAkTf+//D3BkCRGACE0vsEAFh7AxaL4gJA+V8EAPGpAgBU4wZA+WT8YNOfABVrKAIAVGICAPkFAIDS4AMTqmADP9aAAAA0pQQAkb8ACPFj//9U4wZA+WT8YNMGAYDShgIGS58ABmtoAABUJgCA0ub+n8j3QgCRGAcA8eH8/1T7c0ap+WtFqfdjRKn1W0Op81NCqeAHQan9e8eoSUuL0h8gA9VgrfcBAAAAANiw9wEAAAAA"),
    # 代际裸释放 7 个泄漏调用点（0xb386cc/0xe92024/0xb3c394/0xb59b0c/
    # 0xb8ba14/0x9a9fd0/0x2139b88），延迟 8 代。
    # 实测：第 10 次搜索 watchdog 挂死（簇被后续异步路径重访）。
    "free": dict(blob_len=0x220, off_alloc=0x0, off_free=0xC4,
                 off_gc=0x15C, off_b_gc=0x1E0, b64="/Xu/qfNTv6nzAx6qEPz/kBG+QPkgAj/W9AMAqtQEALQK//8Qa0ag0ksBC8sMAACQjKEHke0AgNKOhUD4zgELi38CDuuAAABUrQUA8WH//1QZAAAUKf7/8C/934jvfWDTiv5E0+oDCiopNo9S6cazckp9CRtKMUDTK/7/8GsFQJENBYDSbBEKi45Bf8gOAQC03wUA8cAAAFRKBQCRSjFA060FAPGAAABU9///F5Q9Mcix/v814AMUqvNTwaj9e8GowANf1v17vqnzCwD5IAQAtPADAKoK/ETT6gMKKik2j1LpxrNySn0JG0oxQNMr/v/wawVAkQ8FgNIOAIDSbBEKi41Nf8htAQC0vwEQ66EAAFQtAIDSjU0xyDH//zUuAIDSSgUAkUoxQNPvBQDxgf7/VK4AALTgAxCqEvz/kFKmQPlAAj/W8wtA+f17wqjAA1/W8wtA+f17wqjAA1/W/Xu9qeAHAanzUwKpKf7/8Cr934hKBQARKv2fiF8lAHGjAgBUVCEAUTP+//BzBkCRFQCE0mIOf8hfBADxaQEAVGT8YNOfABRrCAEAVCYAgNJmDiXIpQAANeADAqoQ/P+QEaZA+SACP9ZzQgCRtQYA8SH+/1TzU0Kp4AdBqf17w6j9e72pSUuL0h8gA9XMhrMAAAAAACQg6QAAAAAAlMOzAAAAAAAMm7UAAAAAABS6uAAAAAAA0J+aAAAAAACImxMCAAAAAA=="),
}


def b_insn(pc, target):
    off = (target - pc) >> 2
    assert -(1 << 25) <= off < (1 << 25)
    return struct.pack("<I", 0x14000000 | (off & 0x3FFFFFF))


def fail(msg):
    print("PATCH-LEAK FAIL: " + msg)
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strategy", choices=sorted(STRATEGIES), default=None)
    ap.add_argument("--accept-risk", action="store_true",
                    help="确认已知该策略会破坏引擎，仅用于复现实验")
    ap.add_argument("--in", dest="inp",
                    default="/tmp/wetype-patch/engine/lib/libwxhld.so.orig")
    ap.add_argument("--out", dest="out",
                    default="/tmp/wetype-patch/engine/lib/libwxhld.so")
    args = ap.parse_args()
    if not args.strategy or not args.accept_risk:
        print(__doc__)
        print("未同时指定 --strategy 与 --accept-risk：拒绝打补丁（退出码 2）。")
        sys.exit(2)

    st = STRATEGIES[args.strategy]
    blob = bytearray(base64.b64decode(st["b64"]))
    assert len(blob) == st["blob_len"]
    if bytes(blob[st["off_b_gc"]:st["off_b_gc"] + 4]) != bytes.fromhex("494b8bd2"):
        fail("cave blob 占位符不符（需重汇编 leak_cave_%s.S）" % args.strategy)
    blob[st["off_b_gc"]:st["off_b_gc"] + 4] = b_insn(
        CAVE + st["off_b_gc"], GC_RET)

    src, dst = Path(args.inp), Path(args.out)
    data = bytearray(src.read_bytes())
    print(f"patch-leak[{args.strategy}]: {src} ({len(data)}) -> {dst}")
    already = bytes(data[ZNWM_PLT:ZNWM_PLT + 4]) == b_insn(
        ZNWM_PLT, CAVE + st["off_alloc"])
    if not already:
        if bytes.fromhex(BUILD_ID) not in data:
            fail("BuildID 不符")
        for off, exp, what in ((ZNWM_PLT, ORIG_ZNWM_STUB, "_Znwm@plt"),
                               (ZDLPV_PLT, ORIG_ZDLPV_STUB, "_ZdlPv@plt")):
            if bytes(data[off:off + 16]) != exp:
                fail(what + " 原始字节不符")
        if bytes(data[GC_SESSIONS:GC_SESSIONS + 4]) != ORIG_GC_FIRST:
            fail("wxime_gc_sessions 首指令不符")

    out = bytearray(data)
    if len(out) < CAVE + st["blob_len"]:
        out.extend(b"\0" * (CAVE + st["blob_len"] - len(out)))
    out[CAVE:CAVE + st["blob_len"]] = blob
    out[ZNWM_PLT:ZNWM_PLT + 16] = b_insn(ZNWM_PLT, CAVE + st["off_alloc"]) + NOP * 3
    out[ZDLPV_PLT:ZDLPV_PLT + 16] = b_insn(ZDLPV_PLT, CAVE + st["off_free"]) + NOP * 3
    out[GC_SESSIONS:GC_SESSIONS + 4] = b_insn(GC_SESSIONS, CAVE + st["off_gc"])

    e_phoff, = struct.unpack_from("<Q", out, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", out, 0x36)
    n_load = 0
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, = struct.unpack_from("<I", out, off)
        if p_type != 1:
            continue
        p_vaddr, = struct.unpack_from("<Q", out, off + 16)
        n_load += 1
        if n_load == 1:
            struct.pack_into("<QQ", out, off + 32, CAVE + st["blob_len"],
                             CAVE + st["blob_len"])
        elif n_load == 3:
            struct.pack_into("<Q", out, off + 40, DATA_END - p_vaddr)
    if n_load < 3:
        fail("PT_LOAD 数量异常")
    dst.write_bytes(bytes(out))
    print(f"patch-leak[{args.strategy}]: OK {len(out)} 字节 "
          f"(cave@{CAVE:#x}, 数据区 {DATA:#x}-{DATA_END:#x})；"
          f"注意：该策略实测会破坏引擎，仅限复现。")


if __name__ == "__main__":
    main()
