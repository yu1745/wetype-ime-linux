/* malloc_trace.so: LD_PRELOAD 到 guest（bionic ARM64），统计 malloc/calloc/realloc/free
 * 与 C++ operator new/delete 的存活字节、尺寸直方图和「调用点（LR）」记账，
 * 定位引擎 .so 内部泄漏。直接拦截 operator new 才能拿到真实调用点
 * （否则 LR 全部落到 libc++_shared 的 operator new 里）。
 * 用法：LD_PRELOAD=malloc_trace.so MTRACE_EVERY=5000 …
 * 每 N 次分配向 stderr 打一行总账 + 存活字节最多的调用点。
 * 头部 32B {magic, size, pc, base}；外来块（linker 自举期）直接透传。 */
#include <dlfcn.h>
#include <link.h>
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

/* ---- mtrace2 扩展：命中指定尺寸时 dump 栈上的返回地址 ---- */
#define MAXHIT 8
static unsigned long hit_size[MAXHIT];
static int hit_left[MAXHIT];
static unsigned long wx_lo, wx_hi;   /* libwxhld.so 可执行段区间 */
#define NWR 8
static unsigned long wr_rng[NWR][2]; /* 各 RW PT_LOAD 段 */
static int wr_n;

/* ---- mtrace2 扩展：存活块注册表 + 引用图扫描（找持有者） ---- */
#define REG_SLOTS (1 << 19)
typedef struct { unsigned long ptr, size, pc; } RegEnt;
static RegEnt live_reg[REG_SLOTS];
#define MAXOWN 4
static unsigned long own_target[MAXOWN];
static int own_left[MAXOWN];
static int own_n;

static unsigned long reg_hash(unsigned long p) {
    return (p >> 4) * 11400714819323198485UL & (REG_SLOTS - 1);
}
static void reg_add(unsigned long p, unsigned long n, unsigned long pc) {
    if (!p) return;
    unsigned long h = reg_hash(p);
    for (int i = 0; i < 64; i++) {
        unsigned long s = (h + i) & (REG_SLOTS - 1);
        if (!live_reg[s].ptr) { live_reg[s].ptr = p; live_reg[s].size = n; live_reg[s].pc = pc; return; }
        if (live_reg[s].ptr == p) { live_reg[s].size = n; live_reg[s].pc = pc; return; }
    }
}
static void reg_del(unsigned long p) {
    if (!p) return;
    unsigned long h = reg_hash(p);
    for (int i = 0; i < 64; i++) {
        unsigned long s = (h + i) & (REG_SLOTS - 1);
        if (live_reg[s].ptr == p) { live_reg[s].ptr = 0; return; }
        if (!live_reg[s].ptr) return;
    }
}

static int cmp_ptr(const void *a, const void *b) {
    unsigned long x = ((const RegEnt *)a)->ptr;
    unsigned long y = ((const RegEnt *)b)->ptr;
    return x < y ? -1 : x > y;
}

static void ownership_scan(unsigned long target) {
    /* 收集存活块 */
    static RegEnt arr[REG_SLOTS / 2];
    int cnt = 0;
    for (int i = 0; i < REG_SLOTS && cnt < REG_SLOTS / 2; i++)
        if (live_reg[i].ptr) arr[cnt++] = live_reg[i];
    qsort(arr, cnt, sizeof arr[0], cmp_ptr);
    char buf[256];
    int off = snprintf(buf, sizeof buf, "[own-scan] live blocks=%d target=%#lx\n", cnt, target);
    write(2, buf, off);
    int ntgt = 0, nedge = 0;
    for (int t = cnt - 1; t >= 0 && nedge < 600; t--) {
        if (arr[t].size != target) continue;
        ntgt++;
        if (ntgt > 8) continue;
        int refs = 0;
        for (int j = 0; j < cnt && refs < 8 && nedge < 600; j++) {
            unsigned long sz = arr[j].size;
            if (sz < 0x18) continue;
            unsigned long words = sz >> 3;
            if (words > 8192) words = 8192;   /* 上限 64KB */
            unsigned long *m = (unsigned long *)arr[j].ptr;
            for (unsigned long w = 0; w < words; w++) {
                unsigned long v = m[w];
                if (v < arr[t].ptr || v >= arr[t].ptr + arr[t].size) continue;
                /* 二分定位 v 属于哪个块 */
                int lo = 0, hi = cnt - 1, k = -1;
                while (lo <= hi) { int mid = (lo + hi) / 2; if (arr[mid].ptr <= v) { k = mid; lo = mid + 1; } else hi = mid - 1; }
                if (k < 0 || arr[k].ptr != arr[t].ptr) continue;
                off = snprintf(buf, sizeof buf, "[own-edge] tgt=%#lx(t+%#lx) pc=%#lx <- ref pc=%#lx off=%#lx sz=%#lx\n",
                               arr[t].ptr, v - arr[t].ptr, arr[t].pc, arr[j].pc, w * 8, arr[j].size);
                write(2, buf, off);
                refs++; nedge++;
                break;
            }
        }
        if (!refs) { off = snprintf(buf, sizeof buf, "[own-edge] tgt=%#lx pc=%#lx <- NO-REF\n", arr[t].ptr, arr[t].pc); write(2, buf, off); }
    }
    /* 扫 libwxhld.so 各 RW 段（全局变量）找指向目标块的指针 */
    for (int g = 0; g < wr_n; g++) {
        unsigned long gm = wr_rng[g][0] & ~7UL, gend = wr_rng[g][1];
        off = snprintf(buf, sizeof buf, "[own-scan] scanning globals %lx-%lx (%luKB)\n", gm, gend, (gend - gm) >> 10);
        write(2, buf, off);
        int gedge = 0;
        for (int t = 0; t < cnt && gedge < 40; t++) {
            if (arr[t].size != target) continue;
            for (unsigned long a = gm; a < gend && gedge < 40; a += 8) {
                unsigned long v = *(unsigned long *)a;
                if (v < arr[t].ptr || v >= arr[t].ptr + arr[t].size) continue;
                int lo2 = 0, hi2 = cnt - 1, k2 = -1;
                while (lo2 <= hi2) { int mid = (lo2 + hi2) / 2; if (arr[mid].ptr <= v) { k2 = mid; lo2 = mid + 1; } else hi2 = mid - 1; }
                if (k2 < 0 || arr[k2].ptr != arr[t].ptr) continue;
                off = snprintf(buf, sizeof buf, "[own-g] glob seg%d+0x%lx -> tgt=%#lx(t+%#lx) pc=%#lx\n",
                               g, a - gm, arr[t].ptr, v - arr[t].ptr, arr[t].pc);
                write(2, buf, off);
                gedge++;
            }
        }
    }
    off = snprintf(buf, sizeof buf, "[own-scan] done targets=%d edges=%d\n", ntgt, nedge);
    write(2, buf, off);
}

static long probe_off;
static long probe_len;
static int probe_left;
static unsigned long probe_size;

static void maybe_probe(size_t n, unsigned long userp) {
    if (!probe_size || n != probe_size || probe_left <= 0) return;
    probe_left--;
    unsigned long addr = userp + (unsigned long)((long)probe_off);
    char buf[96];
    int off = snprintf(buf, sizeof buf, "[probe] blk=%#lx sz=%#lx dump@%#lx (+%ld):", userp, n, addr, probe_off);
    write(2, buf, off);
    unsigned char *m = (unsigned char *)addr;
    for (long i = 0; i < probe_len; i++) {
        off = snprintf(buf, sizeof buf, " %02x", m[i]);
        write(2, buf, off);
    }
    write(2, "\n", 1);
}

static void scan_wx_range(void);

static void maybe_ownscan(size_t n) {
    if (!wx_lo) scan_wx_range();
    for (int i = 0; i < own_n; i++) {
        if (own_target[i] && n == own_target[i] && own_left[i] > 0) {
            own_left[i]--;
            if (!wx_lo) scan_wx_range();
            ownership_scan(own_target[i]);
        }
    }
}

static int phdr_cb(struct dl_phdr_info *info, size_t sz, void *data) {
    if (!info->dlpi_name || !strstr(info->dlpi_name, "libwxhld.so")) return 0;
    unsigned long lo = 0, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const Elf64_Phdr *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD) continue;
        unsigned long s = info->dlpi_addr + ph->p_vaddr;
        unsigned long e = s + ph->p_memsz;
        if (ph->p_flags & PF_X) {
            if (!lo || s < lo) lo = s;
            if (e > hi) hi = e;
        } else if ((ph->p_flags & PF_W) && wr_n < NWR) {
            wr_rng[wr_n][0] = s;
            wr_rng[wr_n][1] = e;
            wr_n++;
        }
    }
    if (lo) { *(unsigned long *)data = lo; *((unsigned long *)data + 1) = hi; }
    return 0;
}

static void scan_wx_range(void) {
    unsigned long r[4] = {0, 0, 0, 0};
    dl_iterate_phdr(phdr_cb, r);
    wx_lo = r[0]; wx_hi = r[1];
}

static void maybe_stackdump(size_t n) {
    for (int i = 0; i < MAXHIT; i++) {
        if (hit_size[i] && n == hit_size[i] && hit_left[i] > 0) {
            hit_left[i]--;
            if (!wx_lo) scan_wx_range();
            unsigned long *fp = (unsigned long *)__builtin_frame_address(0);
            char buf[160];
            int off = snprintf(buf, sizeof buf, "[mtrace-stack] size=%#lx pc=%#lx\n", n,
                               (unsigned long)__builtin_return_address(1));
            write(2, buf, off);
            for (int k = 0; k < 640; k++) {
                unsigned long v = fp[k];
                if (v >= wx_lo && v < wx_hi) {
                    off = snprintf(buf, sizeof buf, "[mtrace-stack]   fp+%#x = %#lx\n", k * 8, v);
                    write(2, buf, off);
                }
            }
            write(2, "[mtrace-stack] end\n", 19);
            return;
        }
    }
}

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

static void count_new(size_t n, unsigned long pc) {
    maybe_stackdump(n);
    maybe_ownscan(n);
    atomic_fetch_add(&live_bytes, n);
    atomic_fetch_add(&live_objs, 1);
    atomic_fetch_add(&bin_count[bin_of(n)], 1);
    PCEnt *e = pc_find(pc, 1);
    if (e) { atomic_fetch_add(&e->bytes, n); atomic_fetch_add(&e->cnt, 1); }
    unsigned long total = atomic_fetch_add(&n_malloc, 1) + 1;
    if (mtrace_every && total % mtrace_every == 0) dump();
}
static void count_del(size_t n, unsigned long pc) {
    atomic_fetch_sub(&live_bytes, n);
    atomic_fetch_sub(&live_objs, 1);
    atomic_fetch_sub(&bin_count[bin_of(n)], 1);
    atomic_fetch_add(&n_free, 1);
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
    scan_wx_range();
    const char *h = getenv("MTRACE_STACKDUMP");
    if (h) {
        /* 格式: size:count,size:count,... （16进制） */
        const char *p = h;
        for (int i = 0; i < MAXHIT && *p; i++) {
            hit_size[i] = strtoul(p, (char **)&p, 16);
            if (*p == '.' || *p == ':') { p++; hit_left[i] = (int)strtoul(p, (char **)&p, 16); }
            if (*p == '+' || *p == ',') p++;
        }
    }
    const char *ow = getenv("MTRACE_OWNERS");
    if (ow) {
        const char *p = ow;
        for (int i = 0; i < MAXOWN && *p; i++) {
            char *q;
            own_target[i] = strtoul(p, &q, 16);
            own_n = i + 1;
            if (*q == '.' || *q == ':') own_left[i] = (int)strtoul(q + 1, &q, 16);
            else own_left[i] = 1;
            if (*q == '+') p = q + 1; else break;
        }
    }
    const char *pb = getenv("MTRACE_PROBE");
    if (pb) {
        char *q;
        probe_size = strtoul(pb, &q, 16);
        if (*q == '.') { probe_off = strtol(q + 1, &q, 16); }
        if (*q == '.') { probe_len = strtol(q + 1, &q, 16); }
        if (*q == '.') { probe_left = (int)strtoul(q + 1, NULL, 16); }
    }
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
    reg_add((unsigned long)((char *)base + HDRSZ), n, pc);
    maybe_probe(n, (unsigned long)((char *)base + HDRSZ));
    count_new(n, pc);
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
    reg_del((unsigned long)old);
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
    reg_del((unsigned long)p);
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
