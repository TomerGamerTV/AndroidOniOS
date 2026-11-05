// onirun [-v] [--root DIR] /guest/program [args...]
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "onx.h"

int g_verbose;

void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    _exit(127);
}

int main(int argc, char **argv) {
    const char *root = getenv("ONIRUN_ROOT");
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else break;
    }
    if (i >= argc || !root) {
        fprintf(stderr, "usage: onirun [-v] --root ROOTFS /guest/program [args...]\n");
        return 2;
    }
    char *envp[] = {
        "PATH=/product/bin:/apex/com.android.runtime/bin:/system/bin:/system/xbin",
        "ANDROID_ROOT=/system", "ANDROID_DATA=/data", "ANDROID_ART_ROOT=/apex/com.android.art",
        "ANDROID_I18N_ROOT=/apex/com.android.i18n", "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
        "HOME=/data", "TMPDIR=/data/local/tmp", "SHELL=/system/bin/sh", NULL,
    };
    tr_init();
    sys_init(root);
    ExecImage img;
    int e = load_exec(argv[i], argv + i, envp, &img);
    if (e) die("onirun: cannot load %s (errno %d)", argv[i], -e);
    GuestCtx *c = calloc(1, sizeof *c);
    c->pc = img.entry;
    c->sp = img.sp;
    c->tid = (uint64_t)getpid();
    sys_register_main_thread(c);
    tr_run(c);
    return 0;
}
