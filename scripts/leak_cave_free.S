/* leak_cave.S — libwxhld.so 内存泄漏回收蹦床（AArch64，非位置无关要求：
 * 所有寻址 adrp/adr 相对 + 常量立即数，运行于任意加载基址）
 *
 * 根因：引擎每次搜索泄漏一簇 async 协程帧/闭包（~1.15MB），簇外无引用，
 * 引擎清理析构从不执行。详见 scripts/30_patch_leak.py 头注释。
 *
 * 数据区 0x22F7000（LOAD3 p_memsz 扩展出 RW 零页）：
 *   +0x0    u32 gen
 *   +0x1000 8192 × {u64 ptr; u64 meta}，meta = gen<<32
 *   槽状态：ptr==0 空；ptr==1 墓碑（已被认领/已释放）；否则隔离中
 */
.set CAVE,  0x2330000
.set DATA,  0x22F7000
.set SLOT,  DATA + 0x1000
.set NSLOT, 8192
.set ZNWM_GOT,  0x22B0178
.set ZDLPV_GOT, 0x22B0148
.set GC_RET,    0x90a770

.text
.align 4
.globl cave_origin
cave_origin:

/* ============ cave_alloc: x0=size, x30=site（_Znwm PLT 蹦床） ============ */
cave_alloc:
    stp x29, x30, [sp, #-16]!
    stp x19, x20, [sp, #-16]!
    mov x19, x30                       /* site = 调用点返回地址 */
    adrp x16, . + (0x22b0000 - 0x2330000)
    ldr  x17, [x16, #0x178]
    blr  x17                           /* 真实 operator new(size) */
    mov  x20, x0
    cbz  x20, 90f

    adr  x10, cave_origin
    mov  x11, #CAVE
    sub  x11, x10, x11                 /* x11 = 运行时加载基址 */

    adrp x12, sitetab
    add  x12, x12, #:lo12:sitetab
    mov  x13, #7
1:  ldr  x14, [x12], #8
    add  x14, x14, x11
    cmp  x19, x14
    b.eq 20f
    subs x13, x13, #1
    b.ne 1b
    b    90f                           /* 非泄漏调用点 */

20: /* 命中：入表 meta = gen<<32 */
    adrp x9, . + (0x22f7000 - 0x2330000)
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
    b.eq 40f                           /* 墓碑可复用 */
    add  x10, x10, #1
    ubfx x10, x10, #0, #13
    subs x13, x13, #1
    b.eq 90f                           /* 表满：放弃隔离，不破坏正确性 */
    b    30b
40: stxp w17, x20, x15, [x12]
    cbnz w17, 30b                      /* 争用：重试本槽 */

90: mov  x0, x20
    ldp x19, x20, [sp], #16
    ldp x29, x30, [sp], #16
    ret

/* ============ cave_free: x0=ptr（_ZdlPv PLT 蹦床） ============ */
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
    mov x14, #0                        /* 命中标志 */
10: add x12, x11, x10, lsl #4
    ldxp x13, x19, [x12]               /* x13=slot.ptr, x19=meta */
    cbz x13, 20f
    cmp x13, x16
    b.ne 30f
    mov x13, #1
    stxp w17, x13, x19, [x12]          /* 独占墓碑；成功者负责真实释放 */
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
    blr  x18                           /* 真实 operator delete */
40: ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
50: ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

/* ============ cave_gc_entry: wxime_gc_sessions 入口蹦床 ============ */
cave_gc_entry:
    stp x29, x30, [sp, #-48]!
    stp x0, x1, [sp, #16]
    stp x19, x20, [sp, #32]
    adrp x9, . + (0x22f7000 - 0x2330000)
    ldar w10, [x9]
    add  w10, w10, #1
    stlr w10, [x9]                     /* gen++（仅 daemon 线程） */
    cmp  w10, #9
    b.lo 80f
    sub  w20, w10, #8                  /* 释放阈值：entry_gen <= gen-8 */
    adrp x19, . + (0x22f7000 - 0x2330000)
    add  x19, x19, #0x1000
    mov  x21, #NSLOT
60: ldxp x2, x3, [x19]                 /* x2=ptr, x3=meta */
    cmp  x2, #1
    b.ls 70f
    lsr  x4, x3, #32
    cmp  w4, w20
    b.hi 70f
    mov  x6, #1
    stxp w5, x6, x3, [x19]             /* 独占墓碑 */
    cbnz w5, 70f
    mov  x0, x2
    adrp x16, . + (0x22b0000 - 0x2330000)
    ldr  x17, [x16, #0x148]
    blr  x17                           /* 真实 operator delete */
70: add x19, x19, #16
    subs x21, x21, #1
    b.ne 60b
80: ldp x19, x20, [sp, #32]
    ldp x0, x1, [sp, #16]
    ldp x29, x30, [sp], #48
    stp x29, x30, [sp, #-0x30]!        /* gc_sessions 被替换的原首指令 */
    /* B_GC_RET_PLACEHOLDER：由补丁脚本写入 b 0x90a770 */
    mov  x9, #0x5A5A

    .align 3
sitetab:
    .quad 0xb386cc, 0xe92024, 0xb3c394, 0xb59b0c, 0xb8ba14, 0x9a9fd0, 0x2139b88
