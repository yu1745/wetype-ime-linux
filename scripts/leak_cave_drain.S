/* 历史不安全实验，以下 fifo/UAF 假说已被证伪，严禁部署。
 * 本 blob 漏重放 gc 栈帧首指令，且 delete 透传错误。安全修复在 harness
 * iterator 所有权；证据见 docs/engine-iterator-leak.md 与 30_patch_leak.py。
 */
/* leak_cave2.S — libwxhld.so 泄漏修复蹦床 v2：排空被遗弃的 fifo_scheduler
 *
 * 根因：每搜索创建 2-3 个 async::fifo_scheduler（72B impl + 256B map，分配点
 * 0x1f7ad60，构造后即被遗弃），排入的协程续体永不执行 → 整个搜索任务图
 * （协程帧/闭包/结果向量 ~1.2MB/次）挂起滞留，无人释放。
 * 修复：挂钩 wxime_gc_sessions（宿主每次搜索后调用），把隔离表中上一代
 * fifo 的任务用引擎自带的 try_run_one_task 原地跑完。任务完成 → future
 * 兑现 → 协程 final_suspend → 引擎自身的析构链正常回收整簇内存。
 * impl/map 本身（~330B/个）不释放——可能是长命 fifo，宁可留微小残差
 * 也不引入 double-free。_Znwm/_ZdlPv 蹦床中 _ZdlPv 仅作防御透传。
 *
 * 布局：代码 @0x2330000（LOAD1 尾扩展 RX）；数据 @0x22F7000（LOAD3 memsz
 * 扩展 RW 零页）：+0 u32 gen；+8 u64 合成 outer 槽；
 * +0x1000 起 8192×{u64 ptr; u64 meta} 隔离表（meta=gen<<32）。
 */
.set CAVE,  0x2330000
.set DATA,  0x22F7000
.set SLOT,  DATA + 0x1000
.set NSLOT, 8192
.set ZNWM_GOT,  0x22B0178
.set ZDLPV_GOT, 0x22B0148
.set FIFO_ALLOC_SITE, 0x1F7AD60       /* fifo impl ctor 的 operator new 返回地址 */
.set TRY_RUN_ONE,     0x1F7B0D8       /* async::fifo_scheduler::try_run_one_task */
.set FREE_PLT,        0x21CAA60       /* free@plt（本方案未用，备用） */
.set GC_RET,          0x90A770
.set DRAIN_BUDGET,    512              /* 每 fifo 每次 gc 最多跑的任务数 */

.text
.align 4
.globl cave_origin
cave_origin:

/* ============ cave_alloc: x0=size, x30=site ============ */
cave_alloc:
    stp x29, x30, [sp, #-16]!
    stp x19, x20, [sp, #-16]!
    mov x19, x30
    adrp x16, . + (0x22b0000 - 0x2330000)
    ldr  x17, [x16, #0x178]
    blr  x17
    mov  x20, x0
    cbz  x20, 90f

    adr  x10, cave_origin
    mov  x11, #CAVE
    sub  x11, x10, x11                 /* 加载基址 */
    adrp x12, sitetab
    add  x12, x12, #:lo12:sitetab
    mov  x13, #1
1:  ldr  x14, [x12], #8
    add  x14, x14, x11
    cmp  x19, x14
    b.eq 20f
    subs x13, x13, #1
    b.ne 1b
    b    90f

20: adrp x9, . + (0x22f7000 - 0x2330000)
    ldar w15, [x9]
    lsl  x15, x15, #32
    lsr  x10, x20, #4
    mov  w10, w10
    mov  w9, #0x79B1
    movk w9, #0x9E37, lsl #16
    mul  w10, w10, w9
    ubfx x10, x10, #0, #13
    adrp x11, . + (0x22f7000 - 0x2330000)
    add  x11, x11, #0x1000
    mov  x13, #40
30: add  x12, x11, x10, lsl #4
    ldxp x14, x16, [x12]
    cbz  x14, 40f
    cmp  x14, #1
    b.eq 40f
    add  x10, x10, #1
    ubfx x10, x10, #0, #13
    subs x13, x13, #1
    b.eq 90f
    b    30b
40: stxp w17, x20, x15, [x12]
    cbnz w17, 30b
90: mov  x0, x20
    ldp x19, x20, [sp], #16
    ldp x29, x30, [sp], #16
    ret

/* ============ cave_free: x0=ptr（防御：隔离块被引擎释放时移表转发） ============ */
cave_free:
    stp x29, x30, [sp, #-32]!
    str x19, [sp, #16]
    cbz x0, 50f
    mov x16, x0
    lsr x10, x0, #4
    mov w10, w10
    mov w9, #0x79B1
    movk w9, #0x9E37, lsl #16
    mul w10, w10, w9
    ubfx x10, x10, #0, #13
    adrp x11, . + (0x22f7000 - 0x2330000)
    add x11, x11, #0x1000
    mov x15, #40
    mov x14, #0
10: add x12, x11, x10, lsl #4
    ldxp x13, x19, [x12]
    cbz x13, 20f
    cmp x13, x16
    b.ne 30f
    mov x13, #1
    stxp w17, x13, x19, [x12]
    cbnz w17, 10b
    mov x14, #1
30: add x10, x10, #1
    ubfx x10, x10, #0, #13
    subs x15, x15, #1
    b.ne 10b
20: cbz x14, 40f
    mov x0, x16
    adrp x18, . + (0x22b0000 - 0x2330000)
    ldr  x18, [x18, #0x148]
    blr  x18
40: ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
50: ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

/* ============ cave_gc_entry：排空上一代以前的 fifo ============ */
cave_gc_entry:
    stp x29, x30, [sp, #-0x70]!
    stp x0, x1, [sp, #0x10]
    stp x19, x20, [sp, #0x20]
    stp x21, x22, [sp, #0x30]
    stp x23, x24, [sp, #0x40]
    stp x25, x26, [sp, #0x50]
    stp x27, x28, [sp, #0x60]

    adr  x22, cave_origin
    mov  x9, #CAVE
    sub  x22, x22, x9                  /* x22 = 基址 */

    adrp x9, . + (0x22f7000 - 0x2330000)
    ldar w20, [x9]
    add  w20, w20, #1
    stlr w20, [x9]                     /* x20 = gen */
    cmp  w20, #2
    b.lo 80f
    sub  w21, w20, #1                  /* x21 = 阈值：entry_gen <= gen-1 */
    adrp x19, . + (0x22f7000 - 0x2330000)
    add  x19, x19, #8                  /* x19 = 合成 outer 槽 */
    adrp x23, . + (0x22f7000 - 0x2330000)
    add  x23, x23, #0x1000             /* x23 = 当前槽 */
    mov  x24, #NSLOT
    ldr  x27, =TRY_RUN_ONE
    add  x27, x27, x22                 /* x27 = try_run_one_task 运行时地址 */
60: ldr  x2, [x23]
    cmp  x2, #1
    b.ls 70f
    ldr  x3, [x23, #8]
    lsr  x4, x3, #32
    cmp  w4, w21
    b.hi 70f
    /* 排空：合成 outer=&scratch, *scratch=impl */
    str  x2, [x19]
    mov  x5, #0
61: mov  x0, x19
    blr  x27                           /* try_run_one_task(&scratch) */
    cbz  w0, 62f
    add  x5, x5, #1
    cmp  x5, #DRAIN_BUDGET
    b.lo 61b
62: /* 队列空或预算尽。旧条目（<=gen-8）遗忘以约束表大小：置墓碑。 */
    ldr  x3, [x23, #8]
    lsr  x4, x3, #32
    mov  x6, #8
    sub  w6, w20, w6
    cmp  w4, w6
    b.hi 70f
    mov  x6, #1
    stlr x6, [x23]                     /* 遗忘（不释放内存） */
70: add x23, x23, #16
    subs x24, x24, #1
    b.ne 60b
80: ldp x27, x28, [sp, #0x60]
    ldp x25, x26, [sp, #0x50]
    ldp x23, x24, [sp, #0x40]
    ldp x21, x22, [sp, #0x30]
    ldp x19, x20, [sp, #0x20]
    ldp x0, x1, [sp, #0x10]
    ldp x29, x30, [sp], #0x70
    /* B_GC_RET_PLACEHOLDER：补丁脚本写入 b 0x90a770 */
    mov  x9, #0x5A5A

    .align 3
sitetab:
    .quad FIFO_ALLOC_SITE
