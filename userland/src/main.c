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
    const char *sockets[16] = {0};
    int nsock = 0, init_mode = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--socket") && i + 1 < argc && nsock < 15) sockets[nsock++] = argv[++i];
        else if (!strcmp(argv[i], "--init")) init_mode = 1;
        else break;
    }
    if ((i >= argc && !init_mode) || !root) {
        fprintf(stderr, "usage: onirun [-v] [--socket NAME]... --root ROOTFS /guest/program [args...]\n"
                        "       onirun [-v] --root ROOTFS --init          start the services in ROOTFS/oni/init.txt\n"
                        "  --socket NAME  create /dev/socket/NAME listening and pass it as ANDROID_SOCKET_NAME (like init)\n");
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
    sys_main(init_mode ? NULL : argv[i], argv + i, envp, sockets);
}
