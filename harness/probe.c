/* probe: 在 qemu-aarch64 下 dlopen 引擎库，报告成功/失败与可选符号探测 */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <lib.so> [symbol ...]\n", argv[0]);
        return 2;
    }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) {
        fprintf(stderr, "dlopen FAILED: %s\n", dlerror());
        return 1;
    }
    printf("dlopen ok: %s\n", argv[1]);
    for (int i = 2; i < argc; i++) {
        void *s = dlsym(h, argv[i]);
        printf("  dlsym(%-40s) = %p\n", argv[i], s);
        dlerror();
    }
    return 0;
}
