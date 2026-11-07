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

static struct { const char *path; char **argv, **envp; const char **sockets; } run;
static void run_guest(void *a) { (void)a; sys_main(run.path, run.argv, run.envp, run.sockets); }

int onirun_main(int argc, char **argv) {
    const char *root = getenv("ONIRUN_ROOT");
    const char *sockets[16] = {0};
    int nsock = 0, init_mode = 0, window = 0;
    (void)window;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--socket") && i + 1 < argc && nsock < 15) sockets[nsock++] = argv[++i];
        else if (!strcmp(argv[i], "--init")) init_mode = 1;
        else if (!strcmp(argv[i], "--window")) window = 1;
        else if (!strcmp(argv[i], "--install") && i + 2 < argc) {        // unpack a mkrootfs-tar.py archive
            int r = onirun_install_rootfs(argv[i + 1], argv[i + 2], NULL, NULL);
            if (r) fprintf(stderr, "onirun: install failed: %s\n", strerror(-r));
            return r ? 1 : 0;
        }
        else break;
    }
    if ((i >= argc && !init_mode) || !root) {
        fprintf(stderr, "usage: onirun [-v] [--socket NAME]... --root ROOTFS /guest/program [args...]\n"
                        "       onirun [-v] --root ROOTFS --init          start the services in ROOTFS/oni/init.txt\n"
                        "  --socket NAME  create /dev/socket/NAME listening and pass it as ANDROID_SOCKET_NAME (like init)\n"
                        "  --window       show the guest's screen in a window\n"
                        "       onirun --install ROOTFS.tar DIR   unpack a rootfs archive (tools/mkrootfs-tar.py)\n");
        return 2;
    }
    // The guest environment: Android's defaults, plus classpath variables from the host
    // environment when set (init derives them from /system/etc/classpaths on a device).
    char *envp[64] = {
        "PATH=/product/bin:/apex/com.android.runtime/bin:/system/bin:/system/xbin",
        "ANDROID_ROOT=/system", "ANDROID_DATA=/data", "ANDROID_ART_ROOT=/apex/com.android.art",
        "ANDROID_I18N_ROOT=/apex/com.android.i18n", "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
        "HOME=/data", "TMPDIR=/data/local/tmp", "SHELL=/system/bin/sh",
        "ANDROID_STORAGE=/storage", "ANDROID_ASSETS=/system/app", "EXTERNAL_STORAGE=/sdcard",
        "ASEC_MOUNTPOINT=/mnt/asec", "DOWNLOAD_CACHE=/data/cache",
    };
    int ne = 14;
    const char *pass[] = { "BOOTCLASSPATH", "DEX2OATBOOTCLASSPATH", "SYSTEMSERVERCLASSPATH", "STANDALONE_SYSTEMSERVER_JARS",
                           "ANDROID_SOCKET_zygote", "ANDROID_SOCKET_usap_pool_primary", "TERM", NULL };
    for (int k = 0; pass[k] && ne < 63; k++) {
        const char *v = getenv(pass[k]);
        if (!v) continue;
        char *kv = malloc(strlen(pass[k]) + strlen(v) + 2);
        sprintf(kv, "%s=%s", pass[k], v);
        envp[ne++] = kv;
    }
    envp[ne] = NULL;
    tr_init();
    sys_init(root);
    input_init();
    run.path = init_mode ? NULL : argv[i]; run.argv = argv + i; run.envp = envp; run.sockets = sockets;
#ifndef ONIRUN_LIB
    if (window) display_main(run_guest, NULL);     // --window: the macOS viewer (display.m)
    else
#endif
    run_guest(NULL);
    return 0;
}

#ifndef ONIRUN_LIB
int main(int argc, char **argv) { return onirun_main(argc, argv); }
#endif
