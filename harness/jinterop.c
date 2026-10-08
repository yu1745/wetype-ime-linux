#define _GNU_SOURCE
/* jinterop: 伪造迷你 JNIEnv，直接调用厂商 libwxhld_jni.so 的导出函数
 * initialize(JNIEnv*, jobject, jobject)，让胶水层用真实逻辑填引擎结构体。
 * 设计：
 *  - jobject = JObj{类名, 字段表}，字段值是我们罐头数据（canned）
 *  - jfieldID/jmethodID = 指向 token 结构的指针
 *  - 未实现的 JNIEnv 槽位 = 记日志的 stub（返回 0），跑一轮就知道胶水要什么
 *  - 槽位号由 jni.h（AOSP libnativehelper）的 JNINativeInterface 字段偏移得出，
 *    按函数名挂载，不手写数字 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include "dict_table.h"
#include <sys/stat.h>
#include <string.h>
#include <dlfcn.h>
#include <link.h>
#include <stdarg.h>
#include <pthread.h>
#include <poll.h>
#include <stddef.h>
#include "jni.h"

#define ENV_SLOTS 300

typedef struct { const char *name; char type; /* s,i,l,f,z,o */ const void *val; } JField;
typedef struct JObj { unsigned long magic; const char *cls; JField f[48]; int n; } JObj;
typedef struct { unsigned long magic; unsigned char *data; long len; } JBytes;
#define JB_MAGIC 0x4a42595445530001UL
#define JO_MAGIC 0x4a4f424a45435401UL
typedef struct { const char *name; char type; const char *sig; } JIDToken;   /* jfieldID/jmethodID */

static int g_verbose = 1;
#define LOG(...) do { if (g_verbose) { printf("[env] " __VA_ARGS__); fflush(stdout);} } while (0)

/* ---------- 罐头数据 ---------- */
static JObj g_initinfo;
static const char *CLS_INITINFO = "com/tencent/wxhld/info/InitInfo";

/* Java byte[] 模拟 */

/* 有界登记表仅用于事后 dump；运行时类型识别通过 JObj 自身 magic 标记。 */
static JBytes *g_last_bytes = NULL;   /* 最近创建的 byte[]（String([B) 构造用） */
static JObj *g_objs[8192];
static int g_objn = 0;
static JObj *obj_new(const char *cls) {
    JObj *o = calloc(1, sizeof(JObj));
    o->magic = JO_MAGIC;
    o->cls = cls;
    if (g_objn < 8192) g_objs[g_objn++] = o;
    return o;
}
static void dump_all_objs(void) {
    printf("=== 已构造对象（%d 个，只列有字段的） ===\n", g_objn);
    for (int i = 0; i < g_objn; i++) {
        JObj *o = g_objs[i];
        if (!o || o->n == 0) continue;
        printf("[obj %d] %s:", i, o->cls ? o->cls : "?");
        for (int k = 0; k < o->n; k++) {
            JField *f = &o->f[k];
            if (f->type == 's') printf(" %s=\"%s\"", f->name, (const char *)f->val);
            else if (f->type == 'i' || f->type == 'l' || f->type == 'z') printf(" %s=%ld", f->name, (long)f->val);
            else if (f->type == 'o') printf(" %s=%p", f->name, f->val);
            else if (f->type == 'f') printf(" %s=%f", f->name, f->val ? *(const float *)f->val : 0.0f);
        }
        printf("\n");
    }
    fflush(stdout);
}
static void obj_set(JObj *o, const char *name, char type, const void *val) {
    if (o->n >= 48) return;   /* 防越界（胶水层会反复写同一对象） */
    o->f[o->n].name = name; o->f[o->n].type = type; o->f[o->n].val = val; o->n++;
}
static JField *obj_get(JObj *o, const char *name) {
    for (int i = 0; i < o->n; i++)
        if (!strcmp(o->f[i].name, name)) return &o->f[i];
    return NULL;
}

/* jstring 直接用 JObj 承载，约定字段名 ".str" */
static JObj *str_obj(const char *s) {
    JObj *o = obj_new("java/lang/String");
    obj_set(o, ".str", 's', s);
    return o;
}


/* ---------- JNIEnv 实现 ---------- */
static void *stub_default(void) {
    void *lr = __builtin_return_address(0);
    LOG("UNIMPL slot called (lr=%p)\n", lr);
    return 0;
}

/* 对象数组：JObj 伪装，cls="[L..."，元素存字段里 */
typedef struct { void **elem; int len; } JArr;


/* ---- 候选文本提取与顺序 dump ---- */
static const char *cand_text(JObj *c) {
    if (!c) return NULL;
    JField *f = obj_get(c, "text");
    if (!f || !f->val) return NULL;
    if (f->type == 's') return (const char *)f->val;            /* SetObjectField 已解包 */
    if (f->type == 'o') {                                        /* 原始 String 对象 */
        JField *sf = obj_get((JObj *)f->val, ".str");
        return sf ? (const char *)sf->val : NULL;
    }
    return NULL;
}
static void dump_order(void *env, void *cg, long it, int n, const char *tag) {
    if (!cg || !it) { printf("[%s] 无 iterator\n", tag); return; }
    JArr *a = (JArr *)((void *(*)(void *, void *, long, int))cg)(env, NULL, it, n);
    if (!a) { printf("[%s] candidate_get_n -> NULL\n", tag); return; }
    for (int i = 0; i < a->len; i++) {
        const char *t = cand_text((JObj *)a->elem[i]);
        printf("[%s] rank%d = %s\n", tag, i, t ? t : "?");
    }
    fflush(stdout);
}
static char *g_clsreg[2048]; static int g_clsrn;
static const char *cls_name(void *jclass) {
    if (!jclass) return "?";
    if ((unsigned long)jclass < 0x100000000UL) {
        LOG("cls_name: suspicious token %p (caller lr=%p)\n", jclass, __builtin_return_address(0));
        return "?";
    }
    {   /* 调试：打印原始 token 与其首字 */
        static int dbg = -1;
        if (dbg < 0) { const char *e = getenv("WETYPE_CLSDBG"); dbg = e ? atoi(e) : 0; }
        if (dbg && dbg < 40) {
            dbg++;
            { const char *s = *(const char **)jclass;
              char tmp[64]; int k;
              for (k = 0; k < 40 && s && s[k]; k++) tmp[k] = (s[k] >= 32 && s[k] < 127) ? s[k] : '?';
              tmp[k] = 0;
              LOG("cls_name(%p) -> %p \"%s\" [reg=%p regn=%d diff=%ld]\n", jclass, (void *)s, tmp,
                  (void *)&g_clsreg[0], g_clsrn, (long)((char *)jclass - (char *)&g_clsreg[0])); }
        }
    }
    return *(const char **)jclass;
}

/* ---------- --daemon 行协议状态 ---------- */
static int g_daemon_mode = 0;
static int g_proto_fd = 1;             /* 协议输出 fd（原 stdout） */
static pthread_mutex_t g_cand_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cand_cv = PTHREAD_COND_INITIALIZER;
static unsigned long g_cand_callback_count = 0;
/* 候选事件队列：引擎异步搜索，一次 B 可能先收到旧输入的候选。每个事件带上
 * 同批 PendingInput.match 拼出的输入串，daemon 只回复与已发送按键一致的那个。 */
#define CAND_QUEUE 32
typedef struct { long it; int known; char input[256]; } CandEvent;
static CandEvent g_cand_q[CAND_QUEUE];
static int g_cand_qn = 0;
static char g_pi_acc[256];           /* 当前批 PendingInput.match 累积（仅字母） */
static char g_pi_last[256];          /* 最近一次完整 pendingInputs */
static int g_pi_have = 0;
static long g_pend_it = 0;           /* 已写入 newIterator、尚未回调的候选 */
static int g_pend_known = 0;
static char g_pend_input[256];
static void proto_printf(const char *fmt, ...) {
    char b[8192]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (n > 0) {
        ssize_t _w = write(g_proto_fd, b, (size_t)n);
        if (_w < 0) { int e = errno; fprintf(stderr, "[proto] write FAIL fd=%d errno=%d\n", g_proto_fd, e); }
        (void)_w;
    }
}

/* 引擎候选回调：只记录计数，不输出候选文本或内部指针。 */
static long probe_listener(long a0, long a1, long a2, long a3) {
    if (g_daemon_mode) {   /* 通知 daemon：新候选事件到达 */
        pthread_mutex_lock(&g_cand_mu);
        unsigned long callback = ++g_cand_callback_count;
        long it = g_pend_it;
        int known = g_pend_known;
        int input_len = known ? (int)strlen(g_pend_input) : -1;
        if (it) {
            if (g_cand_qn == CAND_QUEUE) {   /* daemon 长时间未取：丢最旧（泄漏一个 iterator） */
                memmove(g_cand_q, g_cand_q + 1, sizeof g_cand_q[0] * (CAND_QUEUE - 1));
                g_cand_qn--;
            }
            CandEvent *ev = &g_cand_q[g_cand_qn++];
            ev->it = it;
            ev->known = known;
            snprintf(ev->input, sizeof ev->input, "%s", g_pend_input);
            g_pend_it = 0;
        }
        pthread_cond_broadcast(&g_cand_cv);
        pthread_mutex_unlock(&g_cand_mu);
        fprintf(stderr, "[engine] candidate callback #%lu input_len=%d known=%d\n",
                callback, input_len, known);
    }
    return 0;
}

static void *F_FindClass(void *env, const char *name) {
    LOG("FindClass(\"%s\")\n", name);
    for (int i = 0; i < g_clsrn; i++) if (!strcmp(g_clsreg[i], name)) { LOG("  -> %p (cached)\n", (void *)&g_clsreg[i]); return &g_clsreg[i]; }
    if (g_clsrn < 2048) { g_clsreg[g_clsrn] = strdup(name); LOG("  -> %p (new)\n", (void *)&g_clsreg[g_clsrn]); return &g_clsreg[g_clsrn++]; }
    LOG("  -> NULL (注册表满!)\n");
    return NULL;
}
static void *F_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
    LOG("GetFieldID(%s, %s, %s)\n", cls_name(cls), name, sig);
    JIDToken *t = malloc(sizeof(JIDToken));
    t->name = strdup(name);
    t->type = sig && sig[0] == 'L' ? 'o' : sig ? sig[0] : 'x';
    t->sig = strdup(sig ? sig : "");
    return t;
}
static void *F_GetStaticFieldID(void *env, void *cls, const char *name, const char *sig) {
    return F_GetFieldID(env, cls, name, sig);
}
static void *F_GetObjectField(void *env, void *obj, JIDToken *id) {
    LOG("GetObjectField(%s.%s)\n", obj ? ((JObj *)obj)->cls : "null", id ? id->name : "?");
    if (!obj) return NULL;
    JField *f = obj_get((JObj *)obj, id->name);
    if (f && (f->type == 'o' || f->type == 's')) {
        if (f->type == 's') return str_obj((const char *)f->val);
        return (void *)f->val;
    }
    /* auto-complete：缺失字段按签名生成空对象/空串/空数组，杜绝 null 入引擎 */
    const char *sig = id ? id->sig : "";
    if (!strcmp(sig, "Ljava/lang/String;")) return str_obj("");
    if (sig[0] == '[') {
        static JArr empties[16]; static int en;
        if (en < 16) { empties[en].elem = NULL; empties[en].len = 0; return &empties[en++]; }
        return NULL;
    }
    if (sig[0] == 'L') {
        static char clsnames[32][96]; static int cn;
        if (cn < 32) {
            const char *in = sig + 1;
            char *dst = clsnames[cn];
            while (*in && *in != ';') *dst++ = *in++;
            *dst = 0;
            return obj_new(clsnames[cn++]);
        }
    }
    return NULL;
}
static long F_GetIntField(void *env, void *obj, JIDToken *id) {
    LOG("GetIntField(%s.%s)\n", obj ? ((JObj *)obj)->cls : "null", id ? id->name : "?");
    if (!obj) return 0;
    JField *f = obj_get((JObj *)obj, id->name);
    return f && f->type == 'i' ? (long)f->val : 0;
}
static long F_GetLongField(void *env, void *obj, JIDToken *id) {
    LOG("GetLongField(%s.%s)\n", obj ? ((JObj *)obj)->cls : "null", id ? id->name : "?");
    if (!obj) return 0;
    JField *f = obj_get((JObj *)obj, id->name);
    return f && f->type == 'l' ? (long)f->val : 0;
}
static double F_GetFloatField(void *env, void *obj, JIDToken *id) {
    if (!obj) return 0;
    JField *f = obj_get((JObj *)obj, id->name);
    return f && f->type == 'f' ? *(const float *)f->val : 0;
}
static long F_GetBooleanField(void *env, void *obj, JIDToken *id) {
    if (!obj) return 0;
    JField *f = obj_get((JObj *)obj, id->name);
    return f && f->type == 'z' ? (long)f->val : 0;
}
static const char *F_GetStringUTFChars(void *env, void *jstr, unsigned char *isCopy) {
    if (isCopy) *isCopy = 1;
    if (!jstr) return NULL;
    JField *f = obj_get((JObj *)jstr, ".str");
    const char *s = f ? (const char *)f->val : "";
    char *copy = strdup(s);   /* 消费者/引擎会 free，必须 malloc */
    LOG("GetStringUTFChars -> \"%s\" (%p)\n", copy, (void *)copy);
    return copy;
}
static void F_ReleaseStringUTFChars(void *env, void *jstr, const char *c) {
    /* no-op：胶水可能先存指针再 Release，提前 free 会产生悬空指针 */
}
static void *F_NewStringUTF(void *env, const char *s) {
    LOG("NewStringUTF(\"%s\")\n", s ? s : "");
    return str_obj(s ? s : "");
}
static void *F_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
    LOG("GetMethodID(%s, %s, %s)\n", cls_name(cls), name, sig);
    JIDToken *t = malloc(sizeof(JIDToken));
    t->name = strdup(name); t->type = 'm';
    return t;
}
static void *F_CallObjectMethod(void *env, void *obj, JIDToken *id, ...) {
    LOG("CallObjectMethod(%s.%s) -> null\n", obj ? ((JObj *)obj)->cls : "null", id ? id->name : "?");
    /* 常见 getter 罐头：对 InitInfo 的方法一律回 null；后续按日志补充 */
    return NULL;
}
static void F_CallVoidMethod(void *env, void *obj, JIDToken *id, ...) {
    LOG("CallVoidMethod(%s.%s)\n", obj ? ((JObj *)obj)->cls : "null", id ? id->name : "?");
}
static long F_CallIntMethod(void *env, void *obj, JIDToken *id, ...) {
    LOG("CallIntMethod(%s.%s) -> 0\n", obj ? ((JObj *)obj)->cls : "null", id ? id->name : "?");
    return 0;
}
static void *F_NewGlobalRef(void *env, void *o) { return o; }
/* 判断是否为模拟 jobject。不要依赖 g_objs：那只是有界诊断目录。 */
static int is_jobj(void *p) {
    if (!p) return 0;
    return ((JObj *)p)->magic == JO_MAGIC;
}
/* 引擎在每批候选回调前推送 PendingInput[]，其 match 串拼起来就是本次候选对应的输入。
 * 这里在引擎线程里累积，写 newIterator 时与该 iterator 绑定。 */
static void pending_input_field(const char *name, void *val) {
    if (!strcmp(name, "match")) {
        JField *sf = is_jobj(val) ? obj_get((JObj *)val, ".str") : NULL;
        if (!sf || !sf->val) return;
        pthread_mutex_lock(&g_cand_mu);
        size_t n = strlen(g_pi_acc);
        for (const char *p = sf->val; *p && n + 1 < sizeof g_pi_acc; ++p)
            if (*p >= 'a' && *p <= 'z') g_pi_acc[n++] = *p;
        g_pi_acc[n] = 0;
        pthread_mutex_unlock(&g_cand_mu);
    } else if (!strcmp(name, "pendingInputs")) {
        pthread_mutex_lock(&g_cand_mu);
        memcpy(g_pi_last, g_pi_acc, sizeof g_pi_last);
        g_pi_acc[0] = 0;
        g_pi_have = 1;
        pthread_mutex_unlock(&g_cand_mu);
    }
}
static void F_SetObjectField(void *env, void *obj, JIDToken *id, void *val) {
    LOG("SetObjectField(%s.%s)\n", is_jobj(obj) ? ((JObj *)obj)->cls : "?", id ? id->name : "?");
    if (!is_jobj(obj)) return;
    JObj *o = (JObj *)obj;
    if (g_daemon_mode && id) pending_input_field(id->name, val);
    if (is_jobj(val)) {
        JField *sf = obj_get((JObj *)val, ".str");
        if (sf) obj_set(o, id->name, 's', sf->val);
        else obj_set(o, id->name, 'o', val);
    } else {
        obj_set(o, id->name, 'o', val);
    }
}
static void F_SetIntField(void *env, void *obj, JIDToken *id, long v) {
    LOG("SetIntField(%s.%s = %ld)\n", obj ? ((JObj *)obj)->cls : "null", id ? id->name : "?", v);
    if (obj) obj_set((JObj *)obj, id->name, 'i', (void *)v);
}
static long g_last_iterator = 0;   /* 胶水层写入 CandidateList.newIterator 时捕获 */
static void F_SetLongField(void *env, void *obj, JIDToken *id, long v) {
    LOG("SetLongField(%s.%s = %ld)\n", is_jobj(obj) ? ((JObj *)obj)->cls : "null", id ? id->name : "?", v);
    if (id && !strcmp(id->name, "newIterator") && v) {
        g_last_iterator = v;
        if (g_daemon_mode) {
            pthread_mutex_lock(&g_cand_mu);
            g_pend_it = v;
            g_pend_known = g_pi_have;
            memcpy(g_pend_input, g_pi_last, sizeof g_pend_input);
            g_pi_have = 0;
            g_pi_acc[0] = 0;
            pthread_mutex_unlock(&g_cand_mu);
        }
        printf("*** 捕获 newIterator = %#lx ***\n", (unsigned long)v);
        fflush(stdout);
    }
    if (obj) obj_set((JObj *)obj, id->name, 'l', (void *)v);
}
static void F_SetFloatField(void *env, void *obj, JIDToken *id, double v) {
    LOG("SetFloatField(%s.%s = %f)\n", obj ? ((JObj *)obj)->cls : "null", id ? id->name : "?", v);
    if (obj) {
        float fv = (float)v;
        obj_set((JObj *)obj, id->name, 'f', &fv); /* 注意：栈地址，仅调试用 */
    }
}
static void F_SetBooleanField(void *env, void *obj, JIDToken *id, long v) {
    LOG("SetBooleanField(%s.%s = %ld)\n", obj ? ((JObj *)obj)->cls : "null", id ? id->name : "?", v);
    if (obj) obj_set((JObj *)obj, id->name, 'z', (void *)v);
}
static void *F_AllocObject(void *env, void *cls) {
    LOG("AllocObject(%s)\n", cls_name(cls));
    return obj_new(cls_name(cls));
}
static void *F_NewObject(void *env, void *cls, void *mid, void *args) {
    const char *cn = cls_name(cls);
    LOG("NewObject(%s)\n", cn);
    if (cn && !strcmp(cn, "java/lang/String") && g_last_bytes) {
        {   /* 新构造的 String：用最近创建的 byte[] 还原文本 */
            JBytes *b = g_last_bytes;
            long n = (b->len > 0 && b->len < (1 << 20)) ? b->len : 0;
            char *str = calloc(1, (size_t)n + 1);
            if (b->data && n > 0) memcpy(str, b->data, (size_t)n);
            JObj *o = obj_new("java/lang/String");
            obj_set(o, ".str", 's', str);
            printf("[newJstring] \"%s\"\n", str);
            fflush(stdout);
            return o;
        }
    }
    return obj_new(cn);
}
static void *F_GetStaticMethodID(void *env, void *cls, const char *name, const char *sig) {
    LOG("GetStaticMethodID(%s, %s, %s)\n", cls_name(cls), name, sig);
    JIDToken *t = malloc(sizeof(JIDToken));
    t->name = strdup(name); t->type = 'm';
    return t;
}
static void *F_GetStaticObjectField(void *env, void *cls, JIDToken *id) {
    LOG("GetStaticObjectField(%s.%s) -> null\n", cls_name(cls), id ? id->name : "?");
    return NULL;
}
static long F_GetStaticIntField(void *env, void *cls, JIDToken *id) {
    LOG("GetStaticIntField(%s.%s) -> 0\n", cls_name(cls), id ? id->name : "?");
    return 0;
}
static void *F_GetObjectRefType(void *env, void *o) { return (void *)1L; }

/* RegisterNatives(env, cls, methods[], n)：把 Java↔Native 映射全部打印出来。
   JNINativeMethod = { const char* name; const char* sig; void* fnPtr; } */
typedef struct { const char *name; const char *sig; void *fn; } JNMethod;
static long F_RegisterNatives(void *env, void *cls, const JNMethod *methods, long n) {
    LOG("RegisterNatives(%s, %ld methods)\n", cls_name(cls), n);
    for (long i = 0; i < n; i++)
        LOG("  [%ld] %s%s <- %p\n", i, methods[i].name, methods[i].sig, methods[i].fn);
    return 0; /* JNI_OK */
}
static void *F_GetDirectBufferAddress(void *env, void *buf) { return NULL; }
static void F_DeleteGlobalRef(void *env, void *o) { }
static void F_DeleteLocalRef(void *env, void *o) { }
static void *F_NewLocalRef(void *env, void *o) { return o; }
static void *F_ExceptionOccurred(void *env) { return NULL; }
static void F_ExceptionClear(void *env) { }
static unsigned char F_ExceptionCheck(void *env) { return 0; }
/* 静态 void 回调（WxhldApi.onBatchEvent / onReportEngineException）：候选由 harness
   自己的 native 监听器接收，这里空实现；不读参数，可同时挂 ...、V、A 三种变体。 */
static void F_CallStaticVoidMethod(void *env, void *cls, JIDToken *id, ...) {
    LOG("CallStaticVoidMethod(%s.%s)\n", cls_name(cls), id ? id->name : "?");
}
/* 伪 JavaVM：JNI_OnLoad 会调 GetEnv（槽 6，双重间接同 JNIEnv） */
static void *g_env;            /* 前向声明：定义在 env 布表区 */
static void tab_v(int i, void *fn);
static void *vmtab[32];
static void *vmptr;
/* 必须返回与 JNI_OnLoad 相同的 VM：引擎 env_stat 后台任务会用它 GetEnv */
static long F_GetJavaVM(void *env, void **vm) { LOG("GetJavaVM\n"); *vm = &vmptr; return 0; }
static long V_GetEnv(void *vm, void **penv, long version) {
    LOG("JavaVM->GetEnv(%#lx)\n", version);
    *penv = g_env;
    return 0; /* JNI_OK */
}
static long V_AttachCurrentThread(void *vm, void **penv, void *args) {
    LOG("AttachCurrentThread\n");
    *penv = g_env;
    return 0;
}
static long V_DetachCurrentThread(void *vm) { return 0; }
static long V_DestroyJavaVM(void *vm) { return 0; }
/* 按函数名取 JNI 函数表槽位 */
#define ENV_SLOT(name) (offsetof(struct JNINativeInterface, name) / sizeof(void *))
#define VM_SLOT(name) (offsetof(struct JNIInvokeInterface, name) / sizeof(void *))
static void build_vm(void) {
    for (int i = 0; i < 32; i++) vmtab[i] = (void *)stub_default;
    tab_v(VM_SLOT(DestroyJavaVM), V_DestroyJavaVM);
    tab_v(VM_SLOT(AttachCurrentThread), V_AttachCurrentThread);
    tab_v(VM_SLOT(DetachCurrentThread), V_DetachCurrentThread);
    tab_v(VM_SLOT(GetEnv), V_GetEnv);
    tab_v(VM_SLOT(AttachCurrentThreadAsDaemon), V_AttachCurrentThread);
    vmptr = vmtab;
}
typedef long (*onload_fn)(void *vm, void *reserved);
static void tab_v(int i, void *fn) { if (i >= 0 && i < 32) vmtab[i] = fn; }
static long F_GetArrayLength(void *env, void *arr) {
    long n = 0;
    if (arr) {
        if (((JBytes *)arr)->magic == JB_MAGIC) n = ((JBytes *)arr)->len;   /* byte[] */
        else n = ((JArr *)arr)->len;                                        /* 对象数组 */
    }
    LOG("GetArrayLength -> %ld\n", n);
    return n;
}
static void *F_NewObjectArray(void *env, long len, void *cls, void *init) {
    LOG("NewObjectArray(%ld, %s)\n", len, cls_name(cls));
    JArr *a = malloc(sizeof(JArr));
    a->len = (int)len;
    a->elem = calloc(len > 0 ? len : 1, sizeof(void *));
    return a;
}
static void F_SetObjectArrayElement(void *env, void *arr, long idx, void *v) {
    LOG("SetObjectArrayElement[%ld]\n", idx);
    JArr *a = arr;
    if (a && idx >= 0 && idx < a->len) a->elem[idx] = v;
}
static void dump_obj(JObj *o) {
    if (!o) { printf("    <null>\n"); return; }
    printf("    %s {\n", o->cls);
    for (int i = 0; i < o->n; i++) {
        if (o->f[i].name[0] == '.') continue;
        if (o->f[i].type == 's') printf("      %s = \"%s\"\n", o->f[i].name, (const char *)o->f[i].val);
        else if (o->f[i].type == 'i' || o->f[i].type == 'l' || o->f[i].type == 'z')
            printf("      %s = %ld\n", o->f[i].name, (long)o->f[i].val);
        else if (o->f[i].type == 'f') printf("      %s = %f\n", o->f[i].name, *(float *)o->f[i].val);
        else if (is_jobj((void *)o->f[i].val)) {
            JObj *sv = (JObj *)o->f[i].val;
            JField *sf = obj_get(sv, ".str");
            if (sf) printf("      %s = \"%s\"\n", o->f[i].name, (const char *)sf->val);
            else {
                printf("      %s = obj@%p n=%d cls=%p fields:", o->f[i].name, (void *)sv, sv->n, (void *)sv->cls);
                for (int k = 0; k < sv->n; k++)
                    printf(" [%s:%c=%p]", sv->f[k].name ? sv->f[k].name : "?", sv->f[k].type, (void *)sv->f[k].val);
                printf("\n");
            }
        } else printf("      %s = %p\n", o->f[i].name, o->f[i].val);
    }
    printf("    }\n");
}
static void *F_GetObjectArrayElement(void *env, void *arr, long idx) {
    JArr *a = arr;
    LOG("GetObjectArrayElement[%ld/%d]\n", idx, a ? a->len : -1);
    return (a && idx >= 0 && idx < a->len) ? a->elem[idx] : NULL;
}
static void *F_GetObjectClass(void *env, void *o) {
    LOG("GetObjectClass(%p, isjobj=%d)\n", o, is_jobj(o));
    if (!o) return NULL;
    JObj *jo = (JObj *)o;
    void *r = F_FindClass(env, jo->cls);
    LOG("  -> %p\n", r);
    return r;
}
static long F_GetVersion(void *env) { return 0x00010006; /* JNI 1.6 */ }
static long F_EnsureLocalCapacity(void *env, long n) { return 0; }
static long F_PushLocalFrame(void *env, long n) { return 0; }
static void *F_PopLocalFrame(void *env, void *r) { return r; }

/* 字节数组：引擎配置可能传 dict 文件内容 */
static void *F_NewByteArray(void *env, long len) {
    LOG("NewByteArray(%ld)\n", len);
    JBytes *b = malloc(sizeof(JBytes));
    b->magic = JB_MAGIC; b->len = len; b->data = len > 0 ? calloc(1, len) : NULL;
    g_last_bytes = b;
    return b;
}
static void F_SetByteArrayRegion(void *env, void *arr, long start, long len, const void *buf) {
    JBytes *b = (JBytes *)arr;
    LOG("SetByteArrayRegion(start=%ld, len=%ld)\n", start, len);
    if (!b || !b->data || !buf || start < 0 || len < 0 || start + len > b->len) return;
    memcpy(b->data + start, buf, (size_t)len);
}
static void *F_GetByteArrayElements(void *env, void *arr, unsigned char *isCopy) {
    JBytes *b = arr;
    LOG("GetByteArrayElements(%ld bytes)\n", b ? b->len : -1);
    if (isCopy) *isCopy = 0;
    return b ? b->data : NULL;
}
static void F_ReleaseByteArrayElements(void *env, void *arr, void *e, long mode) { }

/* 布表：JNI env 是双重间接（env → 表指针 → 表），g_env 必须指向表指针 */
static void *envtab[ENV_SLOTS];
static void *envptr;
static void *g_env = &envptr;

static void tab(int i, void *fn) { if (i >= 0 && i < ENV_SLOTS) envtab[i] = fn; }

static void build_env(void) {
    for (int i = 0; i < ENV_SLOTS; i++) envtab[i] = (void *)stub_default;
    envptr = envtab;
    tab(ENV_SLOT(GetVersion), F_GetVersion);
    tab(ENV_SLOT(FindClass), F_FindClass);
    tab(ENV_SLOT(ExceptionOccurred), F_ExceptionOccurred);
    tab(ENV_SLOT(ExceptionClear), F_ExceptionClear);
    tab(ENV_SLOT(ExceptionCheck), F_ExceptionCheck);
    tab(ENV_SLOT(PushLocalFrame), F_PushLocalFrame);
    tab(ENV_SLOT(PopLocalFrame), F_PopLocalFrame);
    tab(ENV_SLOT(NewGlobalRef), F_NewGlobalRef);
    tab(ENV_SLOT(DeleteGlobalRef), F_DeleteGlobalRef);
    tab(ENV_SLOT(DeleteLocalRef), F_DeleteLocalRef);
    tab(ENV_SLOT(NewLocalRef), F_NewLocalRef);
    tab(ENV_SLOT(EnsureLocalCapacity), F_EnsureLocalCapacity);
    tab(ENV_SLOT(AllocObject), F_AllocObject);
    /* F_NewObject / F_Call*Method 不读 Java 参数，...、V、A 三种变体共用一个实现 */
    tab(ENV_SLOT(NewObject), F_NewObject);
    tab(ENV_SLOT(NewObjectV), F_NewObject);
    tab(ENV_SLOT(NewObjectA), F_NewObject);
    tab(ENV_SLOT(GetObjectClass), F_GetObjectClass);
    tab(ENV_SLOT(GetMethodID), F_GetMethodID);
    tab(ENV_SLOT(CallObjectMethod), F_CallObjectMethod);
    tab(ENV_SLOT(CallObjectMethodV), F_CallObjectMethod);
    tab(ENV_SLOT(CallObjectMethodA), F_CallObjectMethod);
    tab(ENV_SLOT(CallIntMethod), F_CallIntMethod);
    tab(ENV_SLOT(CallIntMethodV), F_CallIntMethod);
    tab(ENV_SLOT(CallIntMethodA), F_CallIntMethod);
    tab(ENV_SLOT(CallVoidMethod), F_CallVoidMethod);
    tab(ENV_SLOT(CallVoidMethodV), F_CallVoidMethod);
    tab(ENV_SLOT(CallVoidMethodA), F_CallVoidMethod);
    tab(ENV_SLOT(GetFieldID), F_GetFieldID);
    tab(ENV_SLOT(GetObjectField), F_GetObjectField);
    tab(ENV_SLOT(GetBooleanField), F_GetBooleanField);
    tab(ENV_SLOT(GetIntField), F_GetIntField);
    tab(ENV_SLOT(GetLongField), F_GetLongField);
    tab(ENV_SLOT(GetFloatField), F_GetFloatField);
    tab(ENV_SLOT(SetObjectField), F_SetObjectField);
    tab(ENV_SLOT(SetBooleanField), F_SetBooleanField);
    tab(ENV_SLOT(SetIntField), F_SetIntField);
    tab(ENV_SLOT(SetLongField), F_SetLongField);
    tab(ENV_SLOT(SetFloatField), F_SetFloatField);
    tab(ENV_SLOT(GetStaticMethodID), F_GetStaticMethodID);
    tab(ENV_SLOT(CallStaticVoidMethod), F_CallStaticVoidMethod);
    tab(ENV_SLOT(CallStaticVoidMethodV), F_CallStaticVoidMethod);
    tab(ENV_SLOT(CallStaticVoidMethodA), F_CallStaticVoidMethod);
    tab(ENV_SLOT(GetStaticFieldID), F_GetStaticFieldID);
    tab(ENV_SLOT(GetStaticObjectField), F_GetStaticObjectField);
    tab(ENV_SLOT(GetStaticIntField), F_GetStaticIntField);
    tab(ENV_SLOT(NewStringUTF), F_NewStringUTF);
    tab(ENV_SLOT(GetStringUTFChars), F_GetStringUTFChars);
    tab(ENV_SLOT(ReleaseStringUTFChars), F_ReleaseStringUTFChars);
    tab(ENV_SLOT(GetArrayLength), F_GetArrayLength);
    tab(ENV_SLOT(NewObjectArray), F_NewObjectArray);
    tab(ENV_SLOT(GetObjectArrayElement), F_GetObjectArrayElement);
    tab(ENV_SLOT(SetObjectArrayElement), F_SetObjectArrayElement);
    tab(ENV_SLOT(NewByteArray), F_NewByteArray);
    tab(ENV_SLOT(GetByteArrayElements), F_GetByteArrayElements);
    tab(ENV_SLOT(ReleaseByteArrayElements), F_ReleaseByteArrayElements);
    tab(ENV_SLOT(SetByteArrayRegion), F_SetByteArrayRegion);
    tab(ENV_SLOT(RegisterNatives), F_RegisterNatives);
    tab(ENV_SLOT(GetJavaVM), F_GetJavaVM);
    tab(ENV_SLOT(GetDirectBufferAddress), F_GetDirectBufferAddress);
    tab(ENV_SLOT(GetObjectRefType), F_GetObjectRefType);
}

static JObj g_sesscfg;   /* daemon 重建会话复用 */

/* ---------- daemon：候选缓存与会话管理 ---------- */
#define MAX_CAND 72
static char g_ct[MAX_CAND][256];   /* 候选文本 */
static JBytes *g_cid[MAX_CAND];    /* 候选 id byte[] */
static int g_ccover[MAX_CAND];     /* 候选覆盖的输入字母数（cover_input_len） */
static int g_ct_n = 0;
static long g_cur_it = 0;          /* 当前 iterator（上一轮取过候选后保留） */

/* 引擎每次最多返回 10 个；连续分批读取并缓存 text+id 供 SEL。 */
static int daemon_fetch_cands(void *sym_cg, void *sym_dci, long it) {
    g_ct_n = 0;
    if (!sym_cg || !it) return 0;
    for (int batch = 0; batch < MAX_CAND / 10 && g_ct_n < MAX_CAND; ++batch) {
        JArr *a = (JArr *)((void *(*)(void *, void *, long, int))sym_cg)(g_env, NULL, it, 10);
        if (!a || a->len <= 0) break;
        const int before = g_ct_n;
        for (int i = 0; i < a->len && g_ct_n < MAX_CAND; i++) {
            JObj *c = (JObj *)a->elem[i];
            const char *t = cand_text(c);
            if (!t || !*t) continue;
            snprintf(g_ct[g_ct_n], sizeof g_ct[0], "%s", t);
            JField *idf = obj_get(c, "id");
            g_cid[g_ct_n] = (idf && idf->val) ? (JBytes *)idf->val : NULL;
            JField *cf = obj_get(c, "cover_input_len");
            g_ccover[g_ct_n] = (cf && cf->type == 'i') ? (int)(long)cf->val : 0;
            g_ct_n++;
        }
        if (a->len < 10 || g_ct_n == before) break;
    }
    /* 旧 iterator 归还引擎 */
    if (g_cur_it && g_cur_it != it && sym_dci)
        ((void (*)(void *, void *, long))sym_dci)(g_env, NULL, g_cur_it);
    g_cur_it = it;
    return g_ct_n;
}

static void daemon_free_it(void *sym_dci, long it) {
    if (it && it != g_cur_it && sym_dci) ((void (*)(void *, void *, long))sym_dci)(g_env, NULL, it);
}

/* 释放队列中尚未取走的候选事件（上一轮迟到的回调）。 */
static void daemon_drop_events(void *sym_dci) {
    CandEvent evs[CAND_QUEUE];
    pthread_mutex_lock(&g_cand_mu);
    int n = g_cand_qn;
    memcpy(evs, g_cand_q, sizeof evs[0] * n);
    g_cand_qn = 0;
    pthread_mutex_unlock(&g_cand_mu);
    for (int i = 0; i < n; i++) daemon_free_it(sym_dci, evs[i].it);
    if (n) fprintf(stderr, "[daemon] dropped %d late candidate events\n", n);
}

static long mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* 等与 expected（C 以来发送的全部字母）一致的候选并返回其 iterator。
 * 引擎按搜索完成顺序回调，旧输入的候选可能先到：释放它们并继续等；长批量输入
 * 在两次搜索之间可能静默数百毫秒，所以已知输入只以 timeout_ms 为限。
 * expected 为空（部分选词后剩余串未知）时，最后一次回调后 quiet_ms 内无新回调
 * 即退回最新候选。超时退回最新候选，仍无回调返回 0。 */
static long daemon_wait_cand(const char *expected, int timeout_ms, int quiet_ms,
                             void *sym_dci, int *exact) {
    const long t0 = mono_ms();
    long best = 0, last_event = 0;
    int skipped = 0;
    *exact = 0;
    pthread_mutex_lock(&g_cand_mu);
    for (;;) {
        if (g_cand_qn) {
            CandEvent evs[CAND_QUEUE];
            int n = g_cand_qn;
            memcpy(evs, g_cand_q, sizeof evs[0] * n);
            g_cand_qn = 0;
            pthread_mutex_unlock(&g_cand_mu);
            for (int i = 0; i < n; i++) {
                if (best) { daemon_free_it(sym_dci, best); skipped++; }
                best = evs[i].it;
                *exact = evs[i].known && !strcmp(evs[i].input, expected);
                if (!*exact)
                    fprintf(stderr, "[daemon] candidate event input_len=%d known=%d sent_len=%zu (not current)\n",
                            evs[i].known ? (int)strlen(evs[i].input) : -1, evs[i].known, strlen(expected));
            }
            last_event = mono_ms();
            if (*exact) break;
            pthread_mutex_lock(&g_cand_mu);
            continue;
        }
        const long now = mono_ms();
        long until = t0 + timeout_ms;
        if (best && !*expected && last_event + quiet_ms < until) until = last_event + quiet_ms;
        if (now >= until) { pthread_mutex_unlock(&g_cand_mu); break; }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (until - now) / 1000;
        ts.tv_nsec += ((until - now) % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_cand_cv, &g_cand_mu, &ts);
    }
    fprintf(stderr, "[daemon] wait_cand took %ld ms exact=%d skipped=%d found=%d\n",
            mono_ms() - t0, *exact, skipped, best != 0);
    return best;
}

/* 输入一个键：op='d'/'u' */
static void daemon_key(void *sym_pi, long sid, char op, const char *payload) {
    static char buf[64];
    snprintf(buf, sizeof buf, "%c %s", op, payload);
    JObj in; memset(&in, 0, sizeof in); in.cls = "java/lang/String";
    obj_set(&in, ".str", 's', buf);
    ((int (*)(void *, void *, long, void *, void *))sym_pi)(g_env, NULL, sid, &in, NULL);
}

/* 新建会话并挂探针 */
static long daemon_new_session(void *sym_cs, void *sym_asl) {
    long sid = ((long (*)(void *, void *, void *))sym_cs)(g_env, NULL, &g_sesscfg);
    if (sid > 0 && sym_asl)
        ((void (*)(long, void *, void *))sym_asl)(sid, (void *)probe_listener, (void *)0x1234);
    return sid;
}

/* 回复候选行。spans 模式下每项为 "<覆盖字母数>:<文本>"，供前端部分选词。 */
static void daemon_reply_cands(int got, int spans) {
    if (got > 0) {
        proto_printf("CAND");
        fprintf(stderr, "[daemon] response=CAND count=%d iterator=%#lx\n", g_ct_n, g_cur_it);
        for (int i = 0; i < g_ct_n; i++) {
            if (spans) proto_printf("\t%d:%s", g_ccover[i], g_ct[i]);
            else proto_printf("\t%s", g_ct[i]);
        }
        proto_printf("\n");
    } else {
        proto_printf("EMPTY\n");
    }
    fflush(NULL);
}

/* ---------- 命令读取 ----------
 * 自带缓冲的按行读取，以便在不阻塞的情况下查看管道里是否还有积压的命令。 */
static char g_in[8192];
static size_t g_in_len;

/* 缓冲区里没有完整行时读一次 fd 0；block=0 时只在可读时读。返回 0 表示没有新数据。 */
static int cmd_fill(int block) {
    if (!block) {
        struct pollfd pfd = {0, POLLIN, 0};
        if (poll(&pfd, 1, 0) <= 0) return 0;
    }
    if (g_in_len == sizeof g_in) g_in_len = 0;   /* 超长行：丢弃 */
    ssize_t r;
    do r = read(0, g_in + g_in_len, sizeof g_in - g_in_len); while (r < 0 && errno == EINTR);
    if (r <= 0) return 0;
    g_in_len += (size_t)r;
    return 1;
}

/* 取下一行（不含换行）。block=0 时没有完整行立即返回 0；阻塞模式下 EOF 返回 0。 */
static int cmd_read(char *line, size_t cap, int block) {
    for (;;) {
        char *nl = memchr(g_in, '\n', g_in_len);
        if (nl) {
            size_t n = (size_t)(nl - g_in), keep = n < cap - 1 ? n : cap - 1;
            memcpy(line, g_in, keep);
            line[keep] = 0;
            g_in_len -= n + 1;
            memmove(g_in, nl + 1, g_in_len);
            return 1;
        }
        if (!cmd_fill(block)) return 0;
    }
}

/* 管道里是否已经有下一条完整的 B 命令（不消费）。 */
static int cmd_next_is_batch(void) {
    while (!memchr(g_in, '\n', g_in_len))
        if (!cmd_fill(0)) return 0;
    return g_in_len > 2 && g_in[0] == 'B' && g_in[1] == ' ';
}

/* ---------- daemon 主循环 ---------- */
static void run_daemon(void *h, long sid) {
    void *sym_pi  = dlsym(h, "_Z13process_inputP7_JNIEnvP8_jobjectlP8_jstringP11_jbyteArray");
    void *sym_cg  = dlsym(h, "_Z15candidate_get_nP7_JNIEnvP8_jobjectli");
    void *sym_sc  = dlsym(h, "_Z16select_candidateP7_JNIEnvP8_jobjectlP8_jstringP11_jbyteArrayS4_S6_P13_jobjectArrayS2_");
    void *sym_cs  = dlsym(h, "_Z14create_sessionP7_JNIEnvP8_jobjectS2_");
    void *sym_ds  = dlsym(h, "_Z15destroy_sessionP7_JNIEnvP8_jobjectl");
    void *sym_asl = dlsym(h, "wxime_add_session_listener");
    void *sym_dci = dlsym(h, "_Z17delete_candidate_iteratorP7_JNIEnvP8_jobjectl");
    if (!sym_dci) sym_dci = dlsym(h, "delete_candidate_iterator");

    char line[512];
    char sent[256] = "";    /* C 以来发送的字母，用于识别当前输入的候选 */
    size_t sent_n = 0;
    int sent_known = 1;     /* 0: 部分选词或超长后无法与 PendingInput 对齐 */
    int spans = 0;          /* OPT spans: 候选带覆盖长度，S 部分选词回复剩余候选 */
    proto_printf("READY\n");
    while (cmd_read(line, sizeof line, 1)) {
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        if (L == 0) continue;
        fprintf(stderr, "[daemon] command=%c bytes=%zu session=%ld\n",
                line[0], L, sid);
        if (!strcmp(line, "Q")) break;
        if (!strcmp(line, "PING")) { proto_printf("PONG\n"); fflush(NULL); fprintf(stderr, "[daemon] -> PONG\n"); continue; }
        if (!strcmp(line, "OPT spans")) { spans = 1; proto_printf("OK\n"); fflush(NULL); continue; }
        if (!strcmp(line, "SAVE")) { proto_printf("OK\n"); fflush(NULL); continue; }  /* 选词时引擎已落盘 */
        if ((line[0] == 'L' || line[0] == 'B') && line[1] == ' ') {
            /* L 单键兼容；B 可带多个字母，只在末尾取一次候选。
             * 引擎跟不上时管道里会积压多条 B：一并送入，只对最后一条回复候选，
             * 之前的各回复 SKIP（一问一答的配对不变）。 */
            char keys[sizeof line];
            snprintf(keys, sizeof keys, "%s", line + 2);
            int skipped = 0;
            if (line[0] == 'B') {
                char next[sizeof line];
                while (strlen(keys) + 64 < sizeof keys && cmd_next_is_batch() &&
                       cmd_read(next, sizeof next, 0)) {
                    strncat(keys, next + 2, sizeof keys - strlen(keys) - 1);
                    skipped++;
                }
                if (skipped) fprintf(stderr, "[daemon] coalesced %d queued batches\n", skipped + 1);
            }
            if (!*keys || !sid) {
                for (int i = 0; i < skipped; i++) proto_printf("SKIP\n");
                proto_printf("EMPTY\n"); fflush(NULL); continue;
            }
            daemon_drop_events(sym_dci);
            for (const char *p = keys; *p; ++p) {
                char key[2] = {*p, '\0'};
                daemon_key(sym_pi, sid, 'd', key);
                daemon_key(sym_pi, sid, 'u', key);   /* d→u 紧跟, 引擎按序处理 */
                if (sent_known && sent_n + 1 < sizeof sent) { sent[sent_n++] = *p; sent[sent_n] = 0; }
                else sent_known = 0;
                if (line[0] == 'L') break;
            }

            int exact = 0;
            long it = daemon_wait_cand(sent_known ? sent : "", 2500, 300, sym_dci, &exact);
            int got = it ? daemon_fetch_cands(sym_cg, sym_dci, it) : -1;
            fprintf(stderr, "[daemon] input batch chars=%zu composition_len=%zu callbacks=%lu exact=%d iterator=%#lx candidates=%d\n",
                    strlen(keys), sent_n, g_cand_callback_count, exact, it, got);
            for (int i = 0; i < skipped; i++) proto_printf("SKIP\n");
            daemon_reply_cands(got, spans);
            continue;
        }
        /* S <rank> 选词。spans 模式下若候选只覆盖前缀，引擎保留剩余拼音并重新搜索：
         * 回复剩余部分的 CAND/EMPTY；整段上屏仍回复 OK。 */
        if (line[0] == 'S') {
            int idx = atoi(line + 1);
            fprintf(stderr, "[daemon] select index=%d candidate_count=%d\n", idx, g_ct_n);
            if (idx < 0 || idx >= g_ct_n || !g_cid[idx]) { proto_printf("ERR\n"); fflush(NULL); continue; }
            static JObj sel; memset(&sel, 0, sizeof sel); sel.cls = "java/lang/String";
            obj_set(&sel, ".str", 's', g_ct[idx]);
            const int cover = g_ccover[idx];
            const int partial = spans && sent_known && cover > 0 && (size_t)cover < sent_n;
            daemon_drop_events(sym_dci);
            ((void (*)(void *, void *, long, void *, void *, void *, void *, void *, void *, void *))sym_sc)
                (g_env, NULL, sid, &sel, g_cid[idx], NULL, NULL, NULL, NULL, NULL);
            if (!partial) {
                sent_n = 0; sent[0] = 0; sent_known = 0;   /* 剩余串未知，按静默回退 */
                proto_printf("OK\n");
                fflush(NULL);
                continue;
            }
            sent_n -= (size_t)cover;
            memmove(sent, sent + cover, sent_n + 1);
            int exact = 0;
            long it = daemon_wait_cand(sent, 2500, 300, sym_dci, &exact);
            int got = it ? daemon_fetch_cands(sym_cg, sym_dci, it) : -1;
            fprintf(stderr, "[daemon] partial select cover=%d rest_len=%zu exact=%d candidates=%d\n",
                    cover, sent_n, exact, got);
            daemon_reply_cands(got, spans);
            continue;
        }
        if (!strcmp(line, "C") && sym_cs && sym_ds) {      /* C 清空输入（重建会话） */
            fprintf(stderr, "[daemon] reset begin old_session=%ld iterator=%#lx\n", sid, g_cur_it);
            daemon_drop_events(sym_dci);
            if (g_cur_it && sym_dci) { ((void (*)(void *, void *, long))sym_dci)(g_env, NULL, g_cur_it); g_cur_it = 0; }
            ((void (*)(void *, void *, long))sym_ds)(g_env, NULL, sid);
            sid = daemon_new_session(sym_cs, sym_asl);
            g_ct_n = 0; g_last_iterator = 0;
            sent_n = 0; sent[0] = 0; sent_known = 1;
            fprintf(stderr, "[daemon] reset end new_session=%ld\n", sid);
            proto_printf("OK\n");
            fflush(NULL);
            continue;
        }
        proto_printf("ERR\n");
        fflush(NULL);
    }
    if (g_cur_it && sym_dci) ((void (*)(void *, void *, long))sym_dci)(g_env, NULL, g_cur_it);
    if (sid > 0 && sym_ds) {   /* 销毁会话促使引擎落盘用户词库 */
        ((void (*)(void *, void *, long))sym_ds)(g_env, NULL, sid);
        usleep(300000);
    }
    proto_printf("BYE\n");
    fflush(NULL);
    _exit(0);
}

/* ---------- main ---------- */
typedef int (*init_fn)(void *env, void *thiz, void *info);

/* 打印全部模块基址（崩溃地址归因用） */
static int print_module(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; (void)data;
    if (info->dlpi_name && *info->dlpi_name)
        printf("[map] %p %s\n", (void *)info->dlpi_addr, info->dlpi_name);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--daemon")) g_daemon_mode = 1;
    if (g_daemon_mode) {
        g_verbose = 0;                 /* [env] 日志全关 */
        g_proto_fd = dup(1);           /* 协议走原 stdout */
        dup2(2, 1);                    /* 引擎/库的所有 stdout 输出 → stderr */
        setvbuf(stdout, NULL, _IONBF, 0);
    }
    const char *jni_lib = argc > 1 ? argv[1] : "runtime/libwxhld_jni.so";
    g_verbose = argc > 2 ? atoi(argv[2]) : 1;
    if (g_daemon_mode) g_verbose = 0;

    void *h = dlopen(jni_lib, RTLD_NOW);
    if (!h) { printf("dlopen %s FAILED: %s\n", jni_lib, dlerror()); return 1; }
    printf("dlopen ok: %s\n", jni_lib);

    void *sym = dlsym(h, "_Z10initializeP7_JNIEnvP8_jobjectS2_");
    if (!sym) { printf("dlsym initialize FAILED: %s\n", dlerror()); return 1; }
    printf("initialize @ %p\n", sym);

    build_env();
    build_vm();

    dl_iterate_phdr(print_module, NULL);
    fflush(stdout);
    build_vm();

    /* 先走 JNI_OnLoad（注册日志回调/Native 方法表），再调 initialize */
    void *sym_onload = dlsym(h, "JNI_OnLoad");
    if (sym_onload) {
        int vr = ((onload_fn)sym_onload)((void *)&vmptr, NULL);
        printf("JNI_OnLoad -> %#x\n", vr);
        fflush(stdout);
    }

    /* 罐头 InitInfo：字段名来自胶水运行时自报家门 */
    memset(&g_initinfo, 0, sizeof g_initinfo);
    g_initinfo.magic = JO_MAGIC;
    g_initinfo.cls = CLS_INITINFO;
    static int platform_val = 1, devperf = 2;
    obj_set(&g_initinfo, "platform", 'i', (void *)(long)platform_val);
    obj_set(&g_initinfo, "uin", 's', "123456789012345");
    {
        const char *wd = getenv("WETYPE_WORK_DIR");
        if (!wd || !*wd) wd = "/tmp/wetype-work";
        obj_set(&g_initinfo, "workDir", 's', wd);
        const char *sp = getenv("WETYPE_ASSET_DIR");
        if (!sp || !*sp) sp = ".deps/wechat-ime/assets";
        obj_set(&g_initinfo, "sourcePath", 's', sp);
    }
    obj_set(&g_initinfo, "appVer", 's', "3.5.4");
    obj_set(&g_initinfo, "systemVersion", 's', "linux-6.1");
    obj_set(&g_initinfo, "deviceName", 's', "aarch64-linux");
    obj_set(&g_initinfo, "devicePerformance", 'i', (void *)(long)devperf);
    obj_set(&g_initinfo, "logLevel", 'i', (void *)(long)0);
    obj_set(&g_initinfo, "packageType", 'i', (void *)(long)0);
    obj_set(&g_initinfo, "debugLogConfig", 's', "{}");
    /* WevisDeviceScoreInfo：浮点分数不能为 0，否则 BLAS 零尺寸崩溃 */
    {
        static JObj wevis; static float cpu_s = 1.0f, gpu_s = 1.0f;
        memset(&wevis, 0, sizeof wevis);
        wevis.cls = "com/tencent/wxhld/info/WevisDeviceScoreInfo";
        obj_set(&wevis, "cpu_score", 'f', &cpu_s);
        obj_set(&wevis, "gpu_score", 'f', &gpu_s);
        obj_set(&g_initinfo, "wevisDeviceScoreInfo", 'o', &wevis);
    }

    printf("calling initialize(env, NULL, InitInfo{platform=1,user_id=10000001})...\n");
    fflush(stdout);
    int r = ((init_fn)sym)(g_env, NULL, &g_initinfo);
    printf("initialize -> %d (0x%x)\n", r, r);
    fflush(stdout);

    /* 等引擎异步初始化日志 */
    printf("--- brief async init wait ---\n");
    fflush(stdout);
    usleep(300000);

    /* 词库加载：DictInfo{ path, id:I(type), version:I, assetManager }，元数据来自 index.json */
    void *sym_cd = dlsym(h, "_Z11config_dictP7_JNIEnvP8_jobjectP13_jobjectArrayP8_jstring");
    if (sym_cd) {
        const char *dict_dir = getenv("WETYPE_DICT_DIR");
        if (!dict_dir || !*dict_dir)
            dict_dir = ".deps/wechat-ime/assets/config/beta";
        static void *elems[64];
        int dn = 0;
        /* 环境变量 WETYPE_SKIP_DICTS="a.bin,b.bin" 可跳过指定词库（二分定位用） */
        const char *skip = getenv("WETYPE_SKIP_DICTS");
        char skips[512]; skips[0] = 0;
        if (skip) snprintf(skips, sizeof skips, "%s", skip);
        /* hp_base（超参）必须最先加载：图初始化依赖超参先行 */
        unsigned order[64], on = 0;
        for (unsigned i = 0; i < sizeof(g_dict_table)/sizeof(g_dict_table[0]); i++)
            if (!strcmp(g_dict_table[i].name, "hp_base.bin")) order[on++] = i;
        for (unsigned i = 0; i < sizeof(g_dict_table)/sizeof(g_dict_table[0]); i++)
            if (strcmp(g_dict_table[i].name, "hp_base.bin")) order[on++] = i;
        for (unsigned ii = 0;
             ii < sizeof(g_dict_table)/sizeof(g_dict_table[0]) && dn < 64; ii++) {
            unsigned i = order[ii];
            static char paths[64][600];
            if (skip && !strcmp(skip, "ALL")) { printf("  [skip] %s\n", g_dict_table[i].name); continue; }
            if (skip) {
                char tmp[512]; snprintf(tmp, sizeof tmp, "%s", skips);
                int dropped = 0;
                for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ","))
                    if (!strcmp(tok, g_dict_table[i].name)) { dropped = 1; break; }
                if (dropped) { printf("  [skip] %s\n", g_dict_table[i].name); continue; }
            }
            JObj *di = obj_new("com/tencent/wxhld/info/DictInfo");
            snprintf(paths[dn], sizeof paths[dn], "%s/%s", dict_dir, g_dict_table[i].name);
            obj_set(di, "path", 's', paths[dn]);
            obj_set(di, "id", 'i', (void *)(long)g_dict_table[i].type);
            obj_set(di, "version", 'i', (void *)(long)g_dict_table[i].version);
            elems[dn] = di;
            dn++;
        }
        static JArr g_arr; g_arr.elem = elems; g_arr.len = dn;
        printf("calling config_dict(%d dicts)...\n", dn);
        fflush(stdout);
        if (mkdir("/tmp/wetype-work", 0755) && errno != EEXIST) {}
        static char udpath[600];
        {
            const char *wd = getenv("WETYPE_WORK_DIR");
            if (!wd || !*wd) wd = "/tmp/wetype-work";
            snprintf(udpath, sizeof udpath, "%s/userdict", wd);
        }
        static JObj g_userdict;
        memset(&g_userdict, 0, sizeof g_userdict);
        g_userdict.cls = "java/lang/String";
        obj_set(&g_userdict, ".str", 's', udpath);
        int cr = ((int (*)(void *, void *, void *, void *))sym_cd)(g_env, NULL, &g_arr, &g_userdict);
        printf("config_dict -> %d\n", cr);
        fflush(stdout);
        usleep(400000);
    }

    /* 会话链路：让胶水自报 SessionConfig 字段 */
    void *sym_cs = dlsym(h, "_Z14create_sessionP7_JNIEnvP8_jobjectS2_");
    if (sym_cs) {
        memset(&g_sesscfg, 0, sizeof g_sesscfg);
        g_sesscfg.cls = "com/tencent/wxhld/info/SessionConfig";
        /* 真实 app 的 SessionConfig 默认值（keyboard_type=0 即拼音） */
        obj_set(&g_sesscfg, "app_name", 's', "com.tencent.mm");
        obj_set(&g_sesscfg, "app_scene", 'i', (void *)2);
        static int kb_type = 0;
        { const char *kb = getenv("WETYPE_KB"); if (kb) kb_type = atoi(kb); }
        obj_set(&g_sesscfg, "keyboard_type", 'i', (void *)(long)kb_type);
        obj_set(&g_sesscfg, "with_async_session_wrapper", 'z', (void *)1);
        obj_set(&g_sesscfg, "with_search_candidate_instantly_session_wrapper", 'z', (void *)1);
        obj_set(&g_sesscfg, "enable_pre_input", 'z', (void *)1);
        obj_set(&g_sesscfg, "enable_auto_most_likely", 'z', (void *)1);
        obj_set(&g_sesscfg, "enable_user_hot_word_recommend", 'z', (void *)1);
        printf("calling create_session(env, NULL, SessionConfig{app_name=com.tencent.mm,kb=0})...\n");
        fflush(stdout);
        long sid = ((long (*)(void *, void *, void *))sym_cs)(g_env, NULL, &g_sesscfg);
        printf("create_session -> %ld\n", sid);
        fflush(stdout);

        /* 注册我们自己的 native 监听器：抓引擎回调参数（iterator 应在此） */
        void *sym_asl = dlsym(h, "wxime_add_session_listener");
        if (!sym_asl) sym_asl = dlsym(h, "_Z24wxime_add_session_listenerlPFvPvS_iiES_");
        if (sym_asl) {
            printf("registering probe listener on session %ld...\n", sid);
            fflush(stdout);
            ((void (*)(long, void *, void *))sym_asl)(sid, (void *)probe_listener, (void *)0x1234);
            printf("probe listener registered\n");
            fflush(stdout);
        } else {
            printf("wxime_add_session_listener NOT FOUND\n");
        }

        if (g_daemon_mode) run_daemon(h, sid);   /* 不返回 */

        if (g_daemon_mode) goto one_shot_done;   /* daemon 已接管 */
        void *sym_pi = dlsym(h, "_Z13process_inputP7_JNIEnvP8_jobjectlP8_jstringP11_jbyteArray");
        if (sid > 0 && sym_pi) {
            /* WETYPE_INPUT 支持 ';' 分隔的多段输入（逐键） */
            const char *input_str = getenv("WETYPE_INPUT");
            if (!input_str) input_str = "d nihao";
            char seqbuf[1024];
            snprintf(seqbuf, sizeof seqbuf, "%s", input_str);
            for (char *tok = strtok(seqbuf, ";"); tok; tok = strtok(NULL, ";")) {
                static JObj g_inputs[64]; static int gi = 0;
                if (gi >= 64) break;
                JObj *g_input = &g_inputs[gi++];
                memset(g_input, 0, sizeof *g_input);
                g_input->cls = "java/lang/String";
                obj_set(g_input, ".str", 's', tok);
                printf("process_input(sid=%ld, \"%s\")...\n", sid, tok);
                fflush(stdout);
                int pr = ((int (*)(void *, void *, long, void *, void *))sym_pi)(g_env, NULL, sid, g_input, NULL);
                printf("  -> %d\n", pr);
                fflush(stdout);
                usleep(120000);
            }
            usleep(400000);

            /* dump 胶水层构造的 Java 对象（找 CandidateList.newIterator） */
            dump_all_objs();

            /* 取候选词 */
            void *sym_cg = dlsym(h, "_Z15candidate_get_nP7_JNIEnvP8_jobjectli");
            if (sym_cg && !getenv("WETYPE_NO_CAND") && !getenv("WETYPE_TEST_LEARN")) {
                /* 注意：candidate_get_n 的 long 参数是 ITERATOR（非 session id） */
                printf("calling candidate_get_n(iterator=%#lx, n=10)...\n", (unsigned long)g_last_iterator);
                fflush(stdout);
                void *arr = ((void *(*)(void *, void *, long, int))sym_cg)(g_env, NULL, g_last_iterator, 10);
                if (arr) {
                    JArr *a = (JArr *)arr;
                    printf("=== got %d candidates ===\n", a->len);
                    for (int i = 0; i < a->len; i++) {
                        printf("  candidate[%d]:\n", i);
                        dump_obj((JObj *)a->elem[i]);
                    }
                } else {
                    printf("candidate_get_n -> NULL\n");
                }
                fflush(stdout);
            }
        }

    /* ---- 选词学习验证：同进程两次输入 nihao，中间选词“拟好”，比较候选顺序 ---- */
    if (getenv("WETYPE_TEST_LEARN")) {
        void *sym_cg = dlsym(h, "_Z15candidate_get_nP7_JNIEnvP8_jobjectli");
        void *sym_cs2 = dlsym(h, "_Z14create_sessionP7_JNIEnvP8_jobjectS2_");
        void *sym_ds = dlsym(h, "_Z15destroy_sessionP7_JNIEnvP8_jobjectl");
        void *sym_sc = dlsym(h, "_Z16select_candidateP7_JNIEnvP8_jobjectlP8_jstringP11_jbyteArrayS4_S6_P13_jobjectArrayS2_");
        if (!(sym_cg && sym_cs2 && sym_ds && sym_sc)) {
            printf("[learn] 缺符号 cg=%p cs=%p ds=%p sc=%p\n", sym_cg, sym_cs2, sym_ds, sym_sc);
            _exit(3);
        }
        /* 1. 会话 A：输入 nihao，记录原始顺序 */
        static JObj inA; memset(&inA, 0, sizeof inA); inA.cls = "java/lang/String";
        obj_set(&inA, ".str", 's', "d n");
        ((int (*)(void *, void *, long, void *, void *))sym_pi)(g_env, NULL, sid, &inA, NULL);
        usleep(150000);
        inA.f[0].val = "u n"; ((int (*)(void *, void *, long, void *, void *))sym_pi)(g_env, NULL, sid, &inA, NULL);
        usleep(50000);
        const char *keys = "ihao";
        for (const char *k = keys; *k; k++) {
            static char buf[8];
            snprintf(buf, sizeof buf, "d %c", *k);
            inA.f[0].val = buf; ((int (*)(void *, void *, long, void *, void *))sym_pi)(g_env, NULL, sid, &inA, NULL);
            usleep(150000);
            snprintf(buf, sizeof buf, "u %c", *k);
            inA.f[0].val = buf; ((int (*)(void *, void *, long, void *, void *))sym_pi)(g_env, NULL, sid, &inA, NULL);
            usleep(50000);
        }
        printf("=== 会话A 原始候选顺序 (nihao) ===\n");
        static JBytes *sel_id = NULL;
        {
            JArr *a = (JArr *)((void *(*)(void *, void *, long, int))sym_cg)(g_env, NULL, g_last_iterator, 10);
            if (!a) { printf("[learn] A 候选取失败\n"); _exit(5); }
            for (int i = 0; i < a->len; i++) {
                JObj *c = (JObj *)a->elem[i];
                const char *t = cand_text(c);
                printf("[A] rank%d = %s\n", i, t ? t : "?");
                if (t && !strcmp(t, "\xe6\x8b\x9f\xe5\xa5\xbd")) {   /* 拟好 */
                    JField *idf = obj_get(c, "id");
                    if (idf && idf->val) sel_id = (JBytes *)idf->val;
                }
            }
            fflush(stdout);
        }
        if (!sel_id) { printf("[learn] 候选里没找到 拟好\n"); _exit(6); }
        if (sel_id->magic != JB_MAGIC || sel_id->len <= 0) { printf("[learn] id 无效 len=%ld\n", sel_id->len); _exit(7); }

        /* 2. select_candidate(text, id, ...) */
        static JObj sel; memset(&sel, 0, sizeof sel); sel.cls = "java/lang/String";
        obj_set(&sel, ".str", 's', "\xe6\x8b\x9f\xe5\xa5\xbd"); /* 拟好 */
        printf("[learn] select_candidate(sid=%ld, 拟好, id_len=%ld)...\n", sid, sel_id->len);
        ((void (*)(void *, void *, long, void *, void *, void *, void *, void *, void *, void *))sym_sc)
            (g_env, NULL, sid, &sel, sel_id, NULL, NULL, NULL, NULL, NULL);
        fflush(stdout);
        usleep(300000);

        /* 3. 销毁会话 A（促使引擎落盘/入学习队列），再建会话 B */
        ((void (*)(void *, void *, long))sym_ds)(g_env, NULL, sid);
        usleep(300000);
        long sid2 = ((long (*)(void *, void *, void *))sym_cs2)(g_env, NULL, &g_sesscfg);
        printf("[learn] 会话B sid=%ld\n", sid2);
        if (sid2 <= 0) { printf("[learn] 会话B创建失败\n"); _exit(4); }

        /* 4. 会话 B 再输 nihao，比较顺序 */
        inA.f[0].val = "d n"; ((int (*)(void *, void *, long, void *, void *))sym_pi)(g_env, NULL, sid2, &inA, NULL);
        usleep(150000);
        inA.f[0].val = "u n"; ((int (*)(void *, void *, long, void *, void *))sym_pi)(g_env, NULL, sid2, &inA, NULL);
        usleep(50000);
        for (const char *k = keys; *k; k++) {
            static char buf2[8];
            snprintf(buf2, sizeof buf2, "d %c", *k);
            inA.f[0].val = buf2; ((int (*)(void *, void *, long, void *, void *))sym_pi)(g_env, NULL, sid2, &inA, NULL);
            usleep(150000);
            snprintf(buf2, sizeof buf2, "u %c", *k);
            inA.f[0].val = buf2; ((int (*)(void *, void *, long, void *, void *))sym_pi)(g_env, NULL, sid2, &inA, NULL);
            usleep(50000);
        }
        printf("=== 会话B 选词后候选顺序 (nihao) ===\n");
        dump_order(g_env, sym_cg, g_last_iterator, 10, "B");
        printf("=== 对比：若 B 中 拟好 上到 rank0/rank1 前列，说明用户词库生效 ===\n");
        fflush(stdout);
    }

    }
one_shot_done:
    printf("--- done, _exit ---\n");
    fflush(stdout);
    _exit(0);
}
