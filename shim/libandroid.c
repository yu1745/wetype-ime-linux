/* libandroid.so 替身：只提供微信输入法引擎闭包（libandromeda、libwxhld_jni）引用的
   ALooper / AAsset 函数。真 libandroid 依赖 binder 与 framework，无法在 Linux 上加载。
   签名与返回值按 NDK <android/looper.h>、<android/asset_manager.h>。 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* ---- 假 looper：没有 fd 会就绪。pollOnce 像空闲的真 looper 一样等满超时
   （负数即无限等待）后返回 ALOOPER_POLL_TIMEOUT。 ---- */
static int fake_looper;

void *ALooper_prepare(int opts) { (void)opts; return &fake_looper; }
void ALooper_acquire(void *looper) { (void)looper; }
void ALooper_release(void *looper) { (void)looper; }
int ALooper_pollOnce(int timeoutMs, int *outFd, int *outEvents, void **outData) {
    if (outFd) *outFd = -1;
    if (outEvents) *outEvents = 0;
    if (outData) *outData = 0;
    if (timeoutMs < 0) for (;;) pause();
    usleep((useconds_t)timeoutMs * 1000);
    return -3; /* ALOOPER_POLL_TIMEOUT */
}
int ALooper_addFd(void *looper, int fd, int ident, int events,
                  int (*cb)(int, int, void *), void *data) {
    (void)looper; (void)fd; (void)ident; (void)events; (void)cb; (void)data;
    return 1;
}
int ALooper_removeFd(void *looper, int fd) { (void)looper; (void)fd; return 1; }

/* ---- AAsset：磁盘文件后端，按相对根目录查找 ---- */
typedef struct { FILE *f; long size; } wasset_t;
static const char *g_asset_roots[] = {
    "assets/", "runtime/assets/", ".deps/wechat-ime/assets/",
};
void *AAssetManager_fromJava(void *env, void *am) {
    (void)env; (void)am;
    static int wam_token;
    return &wam_token;
}
void *AAssetManager_open(void *mgr, const char *path, int mode) {
    (void)mgr; (void)mode;
    char full[600];
    for (unsigned i = 0; i < sizeof(g_asset_roots) / sizeof(g_asset_roots[0]); i++) {
        snprintf(full, sizeof full, "%s%s", g_asset_roots[i], path);
        FILE *f = fopen(full, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        wasset_t *a = malloc(sizeof *a);
        if (!a) { fclose(f); return NULL; }
        a->f = f;
        a->size = size;
        return a;
    }
    return NULL;
}
/* 返回 fd，outStart/outLength 为资产在该 fd 中的区间 */
int AAsset_openFileDescriptor(void *asset, off_t *outStart, off_t *outLength) {
    wasset_t *a = asset;
    if (!a || !a->f) return -1;
    *outStart = 0;
    *outLength = a->size;
    return dup(fileno(a->f));
}
void AAsset_close(void *asset) {
    wasset_t *a = asset;
    if (a) { if (a->f) fclose(a->f); free(a); }
}
