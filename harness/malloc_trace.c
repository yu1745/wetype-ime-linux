/* malloc_trace.so: LD_PRELOAD 到 guest（bionic ARM64），统计 malloc/calloc/realloc/free
 * 与 C++ operator new/delete 的存活字节、尺寸直方图和「调用点（LR）」记账，
 * 定位引擎 .so 内部泄漏。直接拦截 operator new 才能拿到真实调用点
 * （否则 LR 全部落到 libc++_shared 的 operator new 里）。
 * 用法：LD_PRELOAD=malloc_trace.so MTRACE_EVERY=5000 …
 * 每 N 次分配向 stderr 打一行总账 + 存活字节最多的调用点。
 * 头部 32B {magic, size, pc, base}；外来块（linker 自举期）直接透传。
 *
 * 增强归因（引擎泄漏攻关新增，均默认关闭）：
 *  1) free 记账改用「分配时记录的 pc」——每个调用点的 bytes/cnt 变成真实存活
 *     语义（原来 free 落点与分配点不同就不减，长尾站点被高估）。
 *  2) MTRACE_BT_MIN=<字节数>：超过该尺寸的 operator new 顺 x29 帧指针链回溯
 *     （[mtraceBT] 行），把返回地址序列对照 [map] 基址即可得到引擎内调用链。
 *  3) 同阈值下登记大块（[mtraceBIG] 行随周期 dump 输出块头 40 字节的可见字符
 *     转储），用于指纹识别泄漏块内容（指针数组/压缩 int/字符串碎片）。 */
#include <dlfcn.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HDRSZ 32
#define MAGIC 0x6d74726163653131UL   /* "mtrace11" */
#define NBIN 12
#define NPC 8192   /* 调用点开址哈希表 */

typedef struct { unsigned long magic, size, pc; void *base; } H;
typedef struct { _Atomic unsigned long pc, bytes, cnt; } PCEnt;

static void *(*real_malloc)(size_t);
static void (*real_free)(void *);
static void *(*real_realloc)(void *, size_t);

static _Atomic unsigned long live_bytes, live_objs;
static _Atomic unsigned long n_malloc, n_free;
static _Atomic long bin_count[NBIN];
static PCEnt pc_tab[NPC];
static _Atomic int state;   /* 0=未解析 1=解析中 2=就绪 */
static unsigned long mtrace_every = 5000;
static unsigned long bt_min = 0;   /* MTRACE_BT_MIN：>0 时超过此字节数的分配打印回溯+内容抽样 */

static size_t bin_of(size_t n) {
    size_t b = 0;
    while (b + 1 < NBIN && n > (16u << b)) b++;
    return b;
}
static unsigned long pc_hash(unsigned long pc) {
    return ((pc >> 4) * 2654435761UL) & (NPC - 1);
}
static PCEnt *pc_find(unsigned long pc, int create) {
    unsigned long h = pc_hash(pc);
    for (int probe = 0; probe < 16; probe++) {
        PCEnt *e = &pc_tab[(h + probe) & (NPC - 1)];
        unsigned long ep = atomic_load_explicit(&e->pc, memory_order_relaxed);
        if (ep == pc) return e;
        if (!ep && create) {
            unsigned long expected = 0;
            if (atomic_compare_exchange_strong(&e->pc, &expected, pc)) return e;
            if (atomic_load(&e->pc) == pc) return e;
        }
    }
    return NULL;
}

static void emit(const char *s) { write(2, s, strlen(s)); }

static void dump(void) {
    char line[512];
    int off = snprintf(line, sizeof line, "[mtrace] live=%luKB objs=%lu mallocs=%lu frees=%lu bins:",
                       atomic_load(&live_bytes) >> 10, atomic_load(&live_objs),
                       atomic_load(&n_malloc), atomic_load(&n_free));
    for (int i = 0; i < NBIN && off < (int)sizeof line - 24; i++)
        off += snprintf(line + off, sizeof line - off, " %ld", atomic_load(&bin_count[i]));
    snprintf(line + off, sizeof line - off, "\n");
    emit(line);
    unsigned long idx[8];
    for (int k = 0; k < 8; k++) idx[k] = ULONG_MAX;
    for (int i = 0; i < NPC; i++) {
        if (!atomic_load_explicit(&pc_tab[i].pc, memory_order_relaxed)) continue;
        unsigned long b = atomic_load(&pc_tab[i].bytes);
        for (int k = 0; k < 8; k++) {
            unsigned long cur = idx[k] == ULONG_MAX ? 0 : atomic_load(&pc_tab[idx[k]].bytes);
            if (b > cur) {
                for (int j = 7; j > k; j--) idx[j] = idx[j - 1];
                idx[k] = (unsigned long)i;
                break;
            }
        }
    }
    for (int k = 0; k < 8; k++) {
        if (idx[k] == ULONG_MAX) break;
        PCEnt *e = &pc_tab[idx[k]];
        snprintf(line, sizeof line, "[mtrace]   pc=%#lx live=%luKB cnt=%lu\n",
                 (unsigned long)atomic_load(&e->pc),
                 atomic_load(&e->bytes) >> 10, atomic_load(&e->cnt));
        emit(line);
    }
}

static void bt_walk(void) {
    /* 大分配：展开 x29 帧指针链，打印返回地址序列（模块基址需对照 [map] 行换算） */
    char line[512];
    int off = snprintf(line, sizeof line, "[mtraceBT] size>=BT_MIN:\n");
    emit(line);
    unsigned long fp = (unsigned long)__builtin_frame_address(0);
    off = 0;
    for (int i = 0; i < 16 && fp; i++) {
        unsigned long *fr = (unsigned long *)fp;
        /* 帧布局：[fp] = 上级 fp, [fp+8] = lr */
        if (fr[1] < 0x1000) break;
        off += snprintf(line + off, sizeof line - off - 1, " %#lx", fr[1]);
        if (off > 400) break;
        unsigned long nxt = fr[0];
        if (nxt <= fp) break;   /* 链必须递增 */
        fp = nxt;
    }
    snprintf(line + off, sizeof line - off - 1, "\n");
    emit(line);
}
/* 大块登记环：周期 dump 时回读内容（字符串线索） */
#define BIGRING 512
static struct { unsigned long pc; void *p; size_t n; } big_ring[BIGRING];
static int big_n = 0;
static void big_dump(void) {
    char line[512];
    int start = big_n > 60 ? big_n - 60 : 0;
    for (int i = start; i < big_n && i < BIGRING; i++) {
        unsigned char *q = (unsigned char *)big_ring[i].p;
        if (!q) continue;
        int off = snprintf(line, sizeof line, "[mtraceBIG] pc=%#lx n=%zu head:", big_ring[i].pc, big_ring[i].n);
        for (int k = 0; k < 40 && off < 440; k++) {
            unsigned char c = q[k];
            off += snprintf(line + off, sizeof line - off - 1,
                            (c >= 32 && c < 127) ? "%c" : (c ? " <%02x>" : " \\0"), c);
        }
        snprintf(line + off, sizeof line - off - 1, "\n");
        emit(line);
    }
}
static void count_new(size_t n, unsigned long pc) {
    atomic_fetch_add(&live_bytes, n);
    atomic_fetch_add(&live_objs, 1);
    atomic_fetch_add(&bin_count[bin_of(n)], 1);
    PCEnt *e = pc_find(pc, 1);
    if (e) { atomic_fetch_add(&e->bytes, n); atomic_fetch_add(&e->cnt, 1); }
    unsigned long total = atomic_fetch_add(&n_malloc, 1) + 1;
    if (mtrace_every && total % mtrace_every == 0) { dump(); big_dump(); }
}
static void count_del(size_t n, unsigned long pc) {
    atomic_fetch_sub(&live_bytes, n);
    atomic_fetch_sub(&live_objs, 1);
    atomic_fetch_sub(&bin_count[bin_of(n)], 1);
    atomic_fetch_add(&n_free, 1);
    /* 用分配时记录的 pc 记账：live 语义准确（含小对象长尾） */
    PCEnt *e = pc_find(pc, 0);
    if (e) { atomic_fetch_sub(&e->bytes, n); atomic_fetch_sub(&e->cnt, 1); }
}

/* dlsym 自举：期间分配走静态 arena */
static unsigned char boot[1 << 16];
static size_t boot_used;
static void *boot_alloc(size_t n, size_t align) {
    size_t off = (boot_used + (align - 1)) & ~(align - 1);
    n = (n + 15) & ~(size_t)15;
    if (off + n > sizeof boot) return NULL;
    boot_used = off + n;
    return boot + off;
}

static void resolve(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&state, &expected, 1)) return;
    real_malloc = dlsym(RTLD_NEXT, "malloc");
    real_free = dlsym(RTLD_NEXT, "free");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    const char *e = getenv("MTRACE_EVERY");
    if (e) mtrace_every = strtoul(e, NULL, 0);
    e = getenv("MTRACE_BT_MIN");
    if (e) bt_min = strtoul(e, NULL, 0);
    atomic_store(&state, 2);
}
static int ready(void) {
    if (atomic_load(&state) == 2) return 1;
    resolve();
    return atomic_load(&state) == 2;
}

static void *tag(void *base, size_t n, unsigned long pc) {
    if (!base) return NULL;
    H *h = (H *)base;
    h->magic = MAGIC; h->size = n; h->base = base; h->pc = pc;
    count_new(n, pc);
    if (bt_min && n >= bt_min && big_n < BIGRING) {
        big_ring[big_n].pc = pc; big_ring[big_n].p = (char *)base + HDRSZ; big_ring[big_n].n = n;
        big_n++;
    }
    return (char *)base + HDRSZ;
}

void *malloc_pc(size_t n, unsigned long pc) {
    if (!ready()) { void *b = boot_alloc(n + HDRSZ, 16); if (!b) return NULL; return tag(b, n, pc); }
    return tag(real_malloc(n + HDRSZ), n, pc);
}
void *calloc_pc(size_t n, unsigned long pc) {
    if (!ready()) { void *p = boot_alloc(n + HDRSZ, 16); if (!p) return NULL; memset(p, 0, n + HDRSZ); return tag(p, n, pc); }
    void *base = real_malloc(n + HDRSZ);
    if (!base) return NULL;
    memset(base, 0, n + HDRSZ);
    return tag(base, n, pc);
}
void *memalign_pc(size_t n, size_t align, unsigned long pc) {
    if (align <= 16) return malloc_pc(n, pc);
    void *base;
    if (!ready()) { base = boot_alloc(n + HDRSZ + align, 16); }
    else base = real_malloc(n + HDRSZ + align);
    if (!base) return NULL;
    char *p = (char *)(((uintptr_t)base + HDRSZ + align - 1) & ~(uintptr_t)(align - 1));
    H *h = (H *)(p - HDRSZ);
    h->magic = MAGIC; h->size = n; h->base = base; h->pc = pc;
    count_new(n, pc);
    return p;
}

void *malloc(size_t n) {
    return malloc_pc(n, (unsigned long)__builtin_return_address(0));
}
void *calloc(size_t a, size_t b) {
    return calloc_pc(a * b, (unsigned long)__builtin_return_address(0));
}
void *realloc(void *old, size_t n) {
    if (!old) return malloc(n);
    H *h = (H *)((char *)old - HDRSZ);
    if (h->magic != MAGIC) {   /* 外来块：透传，不计账 */
        if (!ready()) return boot_alloc(n, 16);
        return real_realloc(old, n);
    }
    size_t oldn = h->size;
    unsigned long pc = h->pc;
    if (!ready()) return NULL;
    void *base = real_realloc(h->base, n + HDRSZ);
    if (!base) return NULL;
    count_del(oldn, pc);
    return tag(base, n, pc);
}
void free(void *p) {
    if (!p) return;
    if ((unsigned char *)p >= boot && (unsigned char *)p < boot + sizeof boot) return;
    H *h = (H *)((char *)p - HDRSZ);
    if (h->magic != MAGIC) {
        if (ready()) real_free(p);
        return;
    }
    if (!ready()) return;
    count_del(h->size, h->pc);
    real_free(h->base);
}

/* ---- C++ operator new/delete 拦截：拿真实调用点 ---- */
void *_Znwm(size_t n) { return malloc_pc(n, (unsigned long)__builtin_return_address(0)); }
void *_Znam(size_t n) { return malloc_pc(n, (unsigned long)__builtin_return_address(0)); }
void _ZdlPv(void *p) { free(p); }
void _ZdlPvm(void *p, size_t) { free(p); }
void _ZdaPv(void *p) { free(p); }
void _ZdaPvm(void *p, size_t) { free(p); }
void *_ZnwmRKSt9nothrow_t(size_t n, const void *) { return malloc_pc(n, (unsigned long)__builtin_return_address(0)); }
void *_ZnamRKSt9nothrow_t(size_t n, const void *) { return malloc_pc(n, (unsigned long)__builtin_return_address(0)); }
void _ZdlPvRKSt9nothrow_t(void *p, const void *) { free(p); }
void _ZdaPvRKSt9nothrow_t(void *p, const void *) { free(p); }
void *_ZnwmSt11align_val_t(size_t n, size_t a) { return memalign_pc(n, a, (unsigned long)__builtin_return_address(0)); }
void *_ZnamSt11align_val_t(size_t n, size_t a) { return memalign_pc(n, a, (unsigned long)__builtin_return_address(0)); }
void _ZdlPvSt11align_val_t(void *p, size_t) { free(p); }
void _ZdaPvSt11align_val_t(void *p, size_t) { free(p); }
