#include "Droid-Bridging-Header.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define CS_OPS_STATUS 0
#define CS_DEBUGGED 0x10000000
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);

bool droid_bridge_jit_available(void) {
    int flags = 0;
    if (csops(getpid(), CS_OPS_STATUS, &flags, sizeof(flags)) != 0) {
        return false;
    }
    return (flags & CS_DEBUGGED) != 0;
}

bool droid_bridge_debugger_attached(void) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc info;
    size_t size = sizeof(info);
    memset(&info, 0, sizeof(info));
    if (sysctl(mib, 4, &info, &size, NULL, 0) != 0) {
        return false;
    }
    return (info.kp_proc.p_flag & P_TRACED) != 0;
}

typedef struct {
    int32_t memlimit_active;
    uint32_t memlimit_active_attr;
    int32_t memlimit_inactive;
    uint32_t memlimit_inactive_attr;
} memlimit_props_t;
extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t buffersize);

void droid_bridge_raise_memory_limit(void) {
    /* MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES = 7; only succeeds where permitted. */
    memlimit_props_t p = {-1, 0, -1, 0};
    memorystatus_control(7, getpid(), 0, &p, sizeof(p));
}

static int fill_sun(struct sockaddr_un *sa, const char *path) {
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(sa->sun_path, path);
    return 0;
}

int droid_bridge_unix_listen(const char *path) {
    struct sockaddr_un sa;
    if (fill_sun(&sa, path) < 0) return -1;
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

int droid_bridge_unix_connect(const char *path) {
    struct sockaddr_un sa;
    if (fill_sun(&sa, path) < 0) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int droid_bridge_accept(int fd) {
    int c = accept(fd, NULL, NULL);
    if (c >= 0) {
        int one = 1;
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
    return c;
}

int droid_bridge_set_nonblock(int fd, bool nonblock) {
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, nonblock ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

#define LOAD(field, name) do { \
    *(void **)&out->field = dlsym(h, name); \
    if (!out->field) { snprintf(errbuf, errlen, "missing symbol %s", name); return -1; } \
} while (0)

int droid_qemu_load(const char *path, DroidQEMU *out, char *errbuf, size_t errlen) {
    memset(out, 0, sizeof(*out));
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        snprintf(errbuf, errlen, "%s", dlerror());
        return -1;
    }
    out->handle = h;
    LOAD(qemu_init, "qemu_init");
    LOAD(qemu_main_loop, "qemu_main_loop");
    LOAD(qemu_cleanup, "qemu_cleanup");
    LOAD(display_attach, "droid_display_attach");
    LOAD(touch, "droid_touch");
    LOAD(key, "droid_key");
    LOAD(powerdown, "droid_powerdown");
    LOAD(reset, "droid_reset");
    LOAD(set_running, "droid_set_running");
    LOAD(is_running, "droid_is_running");
    return 0;
}

int droid_qemu_run(DroidQEMU *q, int argc, const char *const *argv) {
    /* QEMU keeps pointers into argv; give it its own copy. */
    char **args = calloc(argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++) {
        args[i] = strdup(argv[i]);
    }
    signal(SIGPIPE, SIG_IGN);
    q->qemu_init(argc, args);
    if (q->display_surface) {
        q->display_attach(q->display_opaque, q->display_surface, q->display_update);
    }
    int status = q->qemu_main_loop();
    q->qemu_cleanup(status);
    return status;
}
