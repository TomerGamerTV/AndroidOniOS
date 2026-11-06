// Linux (arm64) system-call personality on Darwin. Guest pointers are offsets into the calling
// process's window (gptr), guest fds index its fd table, and guest paths resolve inside the rootfs.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <os/os_sync_wait_on_address.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/xattr.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "sys.h"

char g_rootfs[1024];

// ---- errno ----
int lx_errno(int e) {
    switch (e) {
    case EAGAIN: return 11;
    case EDEADLK: return 35;
    case ENAMETOOLONG: return 36;
    case ENOLCK: return 37;
    case ENOSYS: return 38;
    case ENOTEMPTY: return 39;
    case ELOOP: return 40;
    case ENOMSG: return 42;
    case EIDRM: return 43;
    case ENOSTR: return 60;
    case ENODATA: return 61;
    case ETIME: return 62;
    case ENOSR: return 63;
    case EREMOTE: return 66;
    case ENOLINK: return 67;
    case EPROTO: return 71;
    case EMULTIHOP: return 72;
    case EBADMSG: return 74;
    case EOVERFLOW: return 75;
    case EILSEQ: return 84;
    case EUSERS: return 87;
    case ENOTSOCK: return 88;
    case EDESTADDRREQ: return 89;
    case EMSGSIZE: return 90;
    case EPROTOTYPE: return 91;
    case ENOPROTOOPT: return 92;
    case EPROTONOSUPPORT: return 93;
    case ESOCKTNOSUPPORT: return 94;
    case EOPNOTSUPP: return 95;
    case EPFNOSUPPORT: return 96;
    case EAFNOSUPPORT: return 97;
    case EADDRINUSE: return 98;
    case EADDRNOTAVAIL: return 99;
    case ENETDOWN: return 100;
    case ENETUNREACH: return 101;
    case ENETRESET: return 102;
    case ECONNABORTED: return 103;
    case ECONNRESET: return 104;
    case ENOBUFS: return 105;
    case EISCONN: return 106;
    case ENOTCONN: return 107;
    case ESHUTDOWN: return 108;
    case ETOOMANYREFS: return 109;
    case ETIMEDOUT: return 110;
    case ECONNREFUSED: return 111;
    case EHOSTDOWN: return 112;
    case EHOSTUNREACH: return 113;
    case EALREADY: return 114;
    case EINPROGRESS: return 115;
    case ESTALE: return 116;
    case EDQUOT: return 122;
    case ECANCELED: return 125;
    case EOWNERDEAD: return 130;
    case ENOTRECOVERABLE: return 131;
    case ENOTSUP: return 95;
    default: return e <= 34 ? e : 22;   // 1..34 match
    }
}

// Host signal -> Linux signal, for signals forwarded from the terminal.
int lx_sig(int s) {
    switch (s) {
    case SIGHUP: return 1;
    case SIGINT: return 2;
    case SIGQUIT: return 3;
    case SIGALRM: return 14;
    case SIGTERM: return 15;
    case SIGUSR2: return 12;
    case SIGCONT: return 18;
    case SIGTSTP: return 20;
    case SIGTTIN: return 21;
    case SIGTTOU: return 22;
    case SIGXCPU: return 24;
    case SIGXFSZ: return 25;
    case SIGVTALRM: return 26;
    case SIGPROF: return 27;
    case SIGWINCH: return 28;
    default: return 0;
    }
}

// ---- paths ----
#define L_AT_FDCWD (-100)
#define L_AT_SYMLINK_NOFOLLOW 0x100
#define L_AT_REMOVEDIR 0x200
#define L_AT_EMPTY_PATH 0x1000

static bool host_passthrough(const char *p) {
    return !strcmp(p, "/dev/null") || !strcmp(p, "/dev/zero") || !strcmp(p, "/dev/urandom") ||
           !strcmp(p, "/dev/random") || !strcmp(p, "/dev/tty");
}

// Resolves a guest path to a host path inside the rootfs, following guest symlinks (absolute
// targets are relative to the rootfs). Returns 0 or a negative Linux errno.
int host_path(Proc *p, const char *guest, char *out, size_t outlen, bool follow_last) {
    char work[4096];
    if (!strcmp(guest, "/proc/self/exe") && p->exe[0]) guest = p->exe;
    if (guest[0] == '/') {
        if (host_passthrough(guest)) { snprintf(out, outlen, "%s", guest); return 0; }
        snprintf(work, sizeof work, "%s", guest);
    } else {
        snprintf(work, sizeof work, "%s/%s", p->cwd, guest);
    }
    char resolved[4096] = "";   // guest path built so far, "" = root
    char rest[4096];
    snprintf(rest, sizeof rest, "%s", work);
    int links = 0;
    char *s = rest;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        char comp[256];
        if (n >= sizeof comp) return -36;
        memcpy(comp, s, n);
        comp[n] = 0;
        s += n;
        bool last = true;
        for (char *q = s; *q; q++) if (*q != '/') { last = false; break; }
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) {
            char *sl = strrchr(resolved, '/');
            if (sl) *sl = 0; else resolved[0] = 0;
            continue;
        }
        char cand[4096], host[4096];
        snprintf(cand, sizeof cand, "%s/%s", resolved, comp);
        snprintf(host, sizeof host, "%s%s", g_rootfs, cand);
        struct stat st;
        if (lstat(host, &st) == 0 && S_ISLNK(st.st_mode) && (!last || follow_last)) {
            if (++links > 40) return -40;
            char target[4096];
            ssize_t tl = readlink(host, target, sizeof target - 1);
            if (tl < 0) return -lx_errno(errno);
            target[tl] = 0;
            char next[4096];
            snprintf(next, sizeof next, "%s/%s", target, s);
            snprintf(rest, sizeof rest, "%s", next);
            s = rest;
            if (target[0] == '/') resolved[0] = 0;
            continue;
        }
        snprintf(resolved, sizeof resolved, "%s", cand);
    }
    snprintf(out, outlen, "%s%s", g_rootfs, resolved[0] ? resolved : "/");
    return 0;
}

// Guest path of a host path inside the rootfs.
static const char *guest_of(const char *host) {
    size_t n = strlen(g_rootfs);
    if (strncmp(host, g_rootfs, n)) return host;
    return host[n] ? host + n : "/";
}

// Resolves (dirfd, path) for an *at() call: either *hostfd = AT_FDCWD and out holds an absolute
// host path, or *hostfd is the directory's host fd and out holds the relative path unchanged.
static int resolve_at(Proc *p, int dirfd, const char *path, char *out, size_t outlen, bool follow, int *hostfd) {
    if (!path) return -14;
    if (path[0] != '/' && dirfd != L_AT_FDCWD) {
        int h = fd_host(p, dirfd);
        if (h < 0) return -9;
        snprintf(out, outlen, "%s", path);
        *hostfd = h;
        return 0;
    }
    *hostfd = AT_FDCWD;
    return host_path(p, path, out, outlen, follow);
}

// ---- flags ----
static int open_flags(uint64_t f) {
    int r = (int)(f & 3);
    if (f & 0100) r |= O_CREAT;
    if (f & 0200) r |= O_EXCL;
    if (f & 0400) r |= O_NOCTTY;
    if (f & 01000) r |= O_TRUNC;
    if (f & 02000) r |= O_APPEND;
    if (f & 04000) r |= O_NONBLOCK;
    if (f & 040000) r |= O_DIRECTORY;
    if (f & 0100000) r |= O_NOFOLLOW;
    if (f & 010000) r |= O_DSYNC;
    return r;
}
static uint64_t lx_fl(int f) {
    uint64_t r = (uint64_t)(f & 3);
    if (f & O_APPEND) r |= 02000;
    if (f & O_NONBLOCK) r |= 04000;
    return r;
}

// ---- structs ----
// Device numbers for the nodes we emulate (libcutils compares /dev/ashmem's with the fd's).
static uint64_t lx_makedev(unsigned ma, unsigned mi) { return ((uint64_t)(ma & 0xfff) << 8) | (mi & 0xff) | ((uint64_t)(mi & ~0xffu) << 12); }
static uint64_t dev_rdev(const char *kind) {
    if (!strncmp(kind, "ashmem", 6)) return lx_makedev(10, 259);
    if (!strcmp(kind, "binder")) return lx_makedev(0, 0x1a);
    if (!strcmp(kind, "kmsg")) return lx_makedev(1, 11);
    if (!strcmp(kind, "dri/card0") || !strcmp(kind, "drm-card")) return lx_makedev(226, 0);
    if (!strcmp(kind, "dri/renderD128") || !strcmp(kind, "drm-render")) return lx_makedev(226, 128);
    return lx_makedev(1, 3);
}

// Android files belong to root except the shell user's own directories; bionic checks the owner of
// files it maps (property areas), so the answer matters.
static bool shell_owned(const char *gpath) {
    return gpath && (!strncmp(gpath, "/data/local/tmp", 15) || !strncmp(gpath, "/sdcard", 7) ||
                     !strncmp(gpath, "/storage", 8) || !strncmp(gpath, "/memfd:", 7) || !strcmp(gpath, "/dev/ashmem"));
}

static bool is_devnode(const char *gpath) {
    return gpath && !strncmp(gpath, "/dev/", 5) && strncmp(gpath, "/dev/__properties__", 19) &&
           strncmp(gpath, "/dev/socket", 11) && !strchr(gpath + 5, '/') ? true :
           gpath && (!strncmp(gpath, "/dev/binderfs/", 14) || !strncmp(gpath, "/dev/dri/", 9));
}

static void put_stat(uint64_t addr, const struct stat *st, const char *gpath) {
    uint32_t owner = shell_owned(gpath) ? 2000 : 0;
    uint32_t mode = st->st_mode;
    uint64_t rdev = (uint64_t)st->st_rdev;
    if (is_devnode(gpath) && S_ISREG(st->st_mode)) { mode = S_IFCHR | 0666; rdev = dev_rdev(gpath + 5); }   // placeholder files stand in for device nodes
    uint8_t b[128] = {0};
    *(uint64_t *)(b + 0) = (uint64_t)st->st_dev;
    *(uint64_t *)(b + 8) = st->st_ino;
    *(uint32_t *)(b + 16) = mode;
    *(uint32_t *)(b + 20) = st->st_nlink;
    *(uint32_t *)(b + 24) = owner;
    *(uint32_t *)(b + 28) = owner;
    *(uint64_t *)(b + 32) = (uint64_t)rdev;
    *(int64_t *)(b + 48) = st->st_size;
    *(int32_t *)(b + 56) = st->st_blksize;
    *(int64_t *)(b + 64) = st->st_blocks;
    *(int64_t *)(b + 72) = st->st_atimespec.tv_sec;
    *(uint64_t *)(b + 80) = st->st_atimespec.tv_nsec;
    *(int64_t *)(b + 88) = st->st_mtimespec.tv_sec;
    *(uint64_t *)(b + 96) = st->st_mtimespec.tv_nsec;
    *(int64_t *)(b + 104) = st->st_ctimespec.tv_sec;
    *(uint64_t *)(b + 112) = st->st_ctimespec.tv_nsec;
    memcpy(gptr(addr), b, sizeof b);
}

static int lx_clock(uint64_t c) {
    switch (c) {
    case 0: case 5: return CLOCK_REALTIME;
    case 1: case 6: case 7: return CLOCK_MONOTONIC;
    case 2: return CLOCK_PROCESS_CPUTIME_ID;
    case 3: return CLOCK_THREAD_CPUTIME_ID;
    case 4: return CLOCK_MONOTONIC_RAW;
    default: return -1;
    }
}

// Linux iovec array (guest pointers) -> host iovec array.
static struct iovec *iov_in(uint64_t a, uint64_t n) {
    if (n > 1024) return NULL;
    struct iovec *v = calloc(n ? n : 1, sizeof *v);
    const uint64_t *g = gptr(a);
    for (uint64_t i = 0; i < n; i++) { v[i].iov_base = gptr(g[2 * i]); v[i].iov_len = g[2 * i + 1]; }
    return v;
}

// ---- directories (keyed by host fd) ----
static pthread_mutex_t dir_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct DirState { int fd; DIR *d; struct DirState *next; } DirState;
static DirState *dirs;
static DirState *dir_for_locked(int fd) {
    for (DirState *s = dirs; s; s = s->next) if (s->fd == fd) return s;
    int dupfd = dup(fd);
    if (dupfd < 0) return NULL;
    DIR *d = fdopendir(dupfd);
    if (!d) { close(dupfd); return NULL; }
    DirState *s = calloc(1, sizeof *s);
    s->fd = fd;
    s->d = d;
    s->next = dirs;
    dirs = s;
    return s;
}
void dir_forget(int fd) {
    pthread_mutex_lock(&dir_lock);
    for (DirState **pp = &dirs; *pp; pp = &(*pp)->next)
        if ((*pp)->fd == fd) { DirState *s = *pp; *pp = s->next; closedir(s->d); free(s); break; }
    pthread_mutex_unlock(&dir_lock);
}
static uint64_t sys_getdents64(int fd, uint8_t *buf, size_t len) {
    pthread_mutex_lock(&dir_lock);
    DirState *s = dir_for_locked(fd);
    uint64_t used = 0;
    if (!s) { pthread_mutex_unlock(&dir_lock); return ERR(9); }
    for (;;) {
        long pos = telldir(s->d);
        struct dirent *e = readdir(s->d);
        if (!e) break;
        size_t nl = strlen(e->d_name);
        size_t rl = (19 + nl + 1 + 7) & ~7ul;
        if (used + rl > len) {
            seekdir(s->d, pos);
            if (!used) used = ERR(22);
            break;
        }
        uint8_t *r = buf + used;
        *(uint64_t *)r = e->d_ino;
        *(int64_t *)(r + 8) = telldir(s->d);
        *(uint16_t *)(r + 16) = (uint16_t)rl;
        r[18] = e->d_type;
        memcpy(r + 19, e->d_name, nl + 1);
        used += rl;
    }
    pthread_mutex_unlock(&dir_lock);
    return used;
}

// ---- futex ----
static uint64_t futex_wake(void *addr, uint64_t n, bool shared) {
    os_sync_wake_by_address_flags_t f = shared ? OS_SYNC_WAKE_BY_ADDRESS_SHARED : OS_SYNC_WAKE_BY_ADDRESS_NONE;
    if (n >= 0x7fffffff) return os_sync_wake_by_address_all(addr, 4, f) == 0 ? 1 : 0;
    uint64_t woken = 0;
    while (woken < n && os_sync_wake_by_address_any(addr, 4, f) == 0) woken++;
    return woken;
}

static uint64_t sys_futex(uint64_t a, uint64_t op_full, uint64_t val, uint64_t ts, uint64_t a2, uint64_t val3) {
    int op = (int)(op_full & 0x7f);
    bool shared = !(op_full & 128), realtime = op_full & 256;
    uint32_t *addr = gptr(a);
    switch (op) {
    case 0: case 9: {          // FUTEX_WAIT(_BITSET)
        os_sync_wait_on_address_flags_t f = shared ? OS_SYNC_WAIT_ON_ADDRESS_SHARED : OS_SYNC_WAIT_ON_ADDRESS_NONE;
        if (__atomic_load_n(addr, __ATOMIC_SEQ_CST) != (uint32_t)val) return ERR(11);
        int rc;
        if (ts) {
            const struct timespec *t = gptr(ts);
            int64_t ns = (int64_t)t->tv_sec * 1000000000 + t->tv_nsec;
            if (op == 9) {      // absolute timeout
                struct timespec now;
                clock_gettime(realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
                ns -= (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
                if (ns <= 0) return ERR(110);
            }
            rc = os_sync_wait_on_address_with_timeout(addr, (uint32_t)val, 4, f, OS_CLOCK_MACH_ABSOLUTE_TIME, (uint64_t)ns);
        } else {
            rc = os_sync_wait_on_address(addr, (uint32_t)val, 4, f);
        }
        if (rc < 0) return errno == ETIMEDOUT ? ERR(110) : errno == EINTR ? ERR(4) : 0;
        return 0;
    }
    case 1: case 10:           // FUTEX_WAKE(_BITSET)
        return futex_wake(addr, val, shared);
    case 3: case 4: {          // FUTEX_(CMP_)REQUEUE: wake everyone on both (spurious wakeups are legal)
        if (op == 4 && __atomic_load_n(addr, __ATOMIC_SEQ_CST) != (uint32_t)val3) return ERR(11);
        uint64_t n = futex_wake(addr, 0x7fffffff, shared);
        futex_wake(gptr(a2), 0x7fffffff, shared);
        return n;
    }
    case 5:                    // FUTEX_WAKE_OP: approximate as wake both
        return futex_wake(addr, val, shared) + futex_wake(gptr(a2), ts, shared);
    default:
        return ERR(38);
    }
}

static bool is_cgroup_attr(const char *gp) {
    static const char *roots[] = { "/sys/fs/cgroup/", "/dev/cpuctl/", "/dev/cpuset/", "/dev/blkio/", "/dev/memcg/", "/dev/stune/", "/acct/", NULL };
    for (int i = 0; roots[i]; i++)
        if (!strncmp(gp, roots[i], strlen(roots[i]))) {
            const char *b = strrchr(gp, '/') + 1;
            return !strncmp(b, "cgroup.", 7) || !strncmp(b, "cpu.", 4) || !strncmp(b, "cpuset.", 7) || !strncmp(b, "memory.", 7) ||
                   !strncmp(b, "blkio.", 6) || !strncmp(b, "io.", 3) || !strncmp(b, "pids.", 5) || !strncmp(b, "freezer.", 8) ||
                   !strncmp(b, "schedtune.", 10) || !strcmp(b, "tasks") || !strcmp(b, "notify_on_release") || !strcmp(b, "release_agent");
        }
    return false;
}

// The st_mode of an fd object, as Linux would report it.
static void obj_stat_mode(int host, struct stat *st) {
    FdObj *o = fd_obj(host);
    if (!o) return;
    const char *k = o->ops->name;
    if (!strcmp(k, "socket")) st->st_mode = S_IFSOCK | 0777;
    else if (!strcmp(k, "binder") || !strcmp(k, "ashmem") || !strcmp(k, "kmsg") || !strncmp(k, "drm-", 4)) { st->st_mode = S_IFCHR | 0666; st->st_rdev = (dev_t)dev_rdev(k); }
    else if (!strcmp(k, "procdir")) {}
    else st->st_mode = S_IFREG | 0600;                           // anon inodes (eventfd, epoll, timerfd...)
    if (o->size) st->st_size = (off_t)o->size;
}

// "/proc/<pid>/fd/N" or "/proc/self/fd/N" -> the host fd behind it (true if the path is one).
static bool proc_fd_link(Proc *p, const char *gp, int *host) {
    const char *rest = gp + 6;
    Proc *t = p;
    if (!strncmp(rest, "self/", 5)) rest += 5;
    else {
        char *end;
        long pid = strtol(rest, &end, 10);
        if (end == rest || *end != '/') return false;
        LOCKED(t = proc_find((int)pid));
        if (!t) return false;
        rest = end + 1;
    }
    if (strncmp(rest, "fd/", 3)) return false;
    char *end;
    long fd = strtol(rest + 3, &end, 10);
    if (end == rest + 3 || *end) return false;
    *host = fd_host(t, (int)fd);
    return *host >= 0;
}

// ---- system call table ----
static bool restartable(uint64_t nr) {
    // poll/sleep-style calls report EINTR even with SA_RESTART, as on Linux.
    return nr != 73 && nr != 72 && nr != 101 && nr != 115 && nr != 22 && nr != 441;
}

void sys_dispatch(GuestCtx *c) {
    Proc *p = c->proc;
    uint64_t nr = c->x[8], a0 = c->x[0], a1 = c->x[1], a2 = c->x[2], a3 = c->x[3], a4 = c->x[4], a5 = c->x[5];
    uint64_t r = ERR(38);
    char path[4096], path2[4096];
    int hfd, hfd2, e, h;
    // Results of the object layers (vfs, sockets, binder) carry host errnos.
#define OBJRET(v) ({ int64_t _v = (v); _v < 0 ? ERR(lx_errno((int)-_v)) : (uint64_t)_v; })
    // Host fd for a guest fd argument, or fail the call with EBADF.
#define HFD(g) ({ int _h = fd_host(p, (int)(g)); if (_h < 0) { r = ERR(9); break; } _h; })
    switch (nr) {
    case 56: {   // openat
        if (gstr(a1) && gstr(a1)[0] == '/') {
            int d = vfs_open_dev(p, gstr(a1), (int)a2);
            if (d != 1) { r = OBJRET(d); break; }
        } else if (gstr(a1) && (int)a0 != L_AT_FDCWD) {
            // openat(dirfd of /proc/N or /sys/..., "status"): the directory is a listing we generate,
            // so resolve the name against its guest path and serve it the same way.
            int h0 = fd_host(p, (int)a0);
            char dirp[1024];
            if (h0 >= 0 && fd_get_path(h0, dirp, sizeof dirp) && (!strncmp(dirp, "/proc", 5) || !strncmp(dirp, "/sys", 4))) {
                snprintf(path, sizeof path, "%s/%s", dirp, gstr(a1));
                int d = vfs_open_dev(p, path, (int)a2);
                if (d != 1) { r = OBJRET(d); break; }
            }
        }
        if ((e = resolve_at(p, (int)a0, gstr(a1), path, sizeof path, !(a2 & 0100000), &hfd))) { r = ERR(-e); break; }
        int fd = openat(hfd, path, open_flags(a2), (int)(a3 & ~p->umask));
        // cgroup attribute files (cgroup.procs, cpu.shares, ...) under a directory the guest made:
        // they exist on a real kernel the moment the directory does, so create them on first use.
        if (fd < 0 && errno == ENOENT && hfd == AT_FDCWD && is_cgroup_attr(guest_of(path)))
            fd = openat(hfd, path, open_flags(a2) | O_CREAT, 0644);
        if (fd < 0) { r = ERR(lx_errno(errno)); break; }
        if (hfd == AT_FDCWD) fd_set_path(fd, guest_of(path));
        int g = fd_install(p, fd, 0, a2 & 02000000);
        r = g < 0 ? ERR(-g) : (uint64_t)g;
        LOGV("  openat(%s) -> %lld\n", gstr(a1), (long long)r);
        break;
    }
    case 57: e = fd_close(p, (int)a0); r = e ? ERR(-e) : 0; break;
    case 63: {
        h = HFD(a0);
        int64_t v = vfs_read(c, h, gptr(a1), a2);
        r = v == VFS_PASS ? HOSTRET(read(h, gptr(a1), a2)) : OBJRET(v);
        break;
    }
    case 64: case 66: {   // write, writev
        h = HFD(a0);
        ssize_t n;
        if (nr == 64) {
            int64_t v = vfs_write(c, h, gptr(a1), a2);
            if (v != VFS_PASS) { r = OBJRET(v); break; }
            n = write(h, gptr(a1), a2);
        }
        else {
            struct iovec *v = iov_in(a1, a2);
            if (!v) { r = ERR(22); break; }
            n = writev(h, v, (int)a2);
            free(v);
        }
        if (n < 0 && errno == EPIPE) proc_raise(c, 13);
        r = HOSTRET(n);
        break;
    }
    case 65: {   // readv
        h = HFD(a0);
        struct iovec *v = iov_in(a1, a2);
        if (!v) { r = ERR(22); break; }
        r = HOSTRET(readv(h, v, (int)a2));
        free(v);
        break;
    }
    case 67: h = HFD(a0); r = HOSTRET(pread(h, gptr(a1), a2, (off_t)a3)); break;
    case 68: h = HFD(a0); r = HOSTRET(pwrite(h, gptr(a1), a2, (off_t)a3)); break;
    case 62: {
        h = HFD(a0);
        FdObj *o = fd_obj(h);
        if (o && o->size) { r = a2 == 2 ? o->size + a1 : a2 == 0 ? a1 : 0; break; }   // objects with a size: SEEK_END/SET
        r = HOSTRET(lseek(h, (off_t)a1, (int)a2));
        break;
    }
    case 71: {   // sendfile(out, in, *offset, count)
        int ho = HFD(a0), hi = HFD(a1);
        char buf[65536];
        int64_t *offp = gptr(a2);
        uint64_t done = 0;
        while (done < a3) {
            size_t want = a3 - done < sizeof buf ? a3 - done : sizeof buf;
            ssize_t got = offp ? pread(hi, buf, want, *offp) : read(hi, buf, want);
            if (got <= 0) break;
            ssize_t put = write(ho, buf, (size_t)got);
            if (put <= 0) break;
            if (offp) *offp += put;
            done += (uint64_t)put;
            if (put < got) break;
        }
        r = done;
        break;
    }
    case 79: {   // newfstatat
        struct stat st;
        int rc;
        const char *gp = NULL;
        if ((a3 & L_AT_EMPTY_PATH) && !*gstr(a1)) { h = HFD(a0); rc = fstat(h, &st); if (!rc) obj_stat_mode(h, &st); gp = fd_get_path(h, path2, sizeof path2) ? path2 : NULL; }
        else if (gstr(a1) && !strncmp(gstr(a1), "/proc", 5) && proc_fd_link(p, gstr(a1), &hfd)) {
            // /proc/<pid>/fd/N: a symlink to whatever the fd is
            if (a3 & L_AT_SYMLINK_NOFOLLOW) { memset(&st, 0, sizeof st); st.st_mode = S_IFLNK | 0700; st.st_size = 64; st.st_nlink = 1; rc = 0; }
            else { rc = fstat(hfd, &st); if (!rc) obj_stat_mode(hfd, &st); }
            gp = fd_get_path(hfd, path2, sizeof path2) ? path2 : NULL;
        } else if (gstr(a1) && !strncmp(gstr(a1), "/proc", 5) && strncmp(gstr(a1), "/proc/self/exe", 14)) {
            int g = procfs_open(p, gstr(a1), 0);            // generated: open it and look
            if (g < 0) { r = ERR(-g); break; }
            if (g == 1) { r = ERR(2); break; }
            rc = fstat(fd_host(p, g), &st);
            fd_close(p, g);
            gp = gstr(a1);
        } else {
            bool follow = !(a3 & L_AT_SYMLINK_NOFOLLOW);
            if ((e = resolve_at(p, (int)a0, gstr(a1), path, sizeof path, follow, &hfd))) { r = ERR(-e); break; }
            rc = fstatat(hfd, path, &st, follow ? 0 : AT_SYMLINK_NOFOLLOW);
            gp = hfd == AT_FDCWD ? guest_of(path) : gstr(a1);
        }
        if (rc == 0) put_stat(a2, &st, gp);
        r = HOSTRET(rc);
        break;
    }
    case 80: {
        struct stat st;
        h = HFD(a0);
        int rc = fstat(h, &st);
        if (!rc) { obj_stat_mode(h, &st); put_stat(a1, &st, fd_get_path(h, path2, sizeof path2) ? path2 : NULL); }
        r = HOSTRET(rc);
        break;
    }
    case 78: {   // readlinkat
        const char *gp = gstr(a1);
        char tmp[4096];
        const char *res = NULL;
        int hf;
        if (!strcmp(gp, "/proc/self/exe")) res = p->exe;
        else if (!strncmp(gp, "/proc/", 6) && proc_fd_link(p, gp, &hf)) {
            if (!fd_get_path(hf, tmp, sizeof tmp)) snprintf(tmp, sizeof tmp, "anon_inode:[%s]", fd_obj(hf) ? fd_obj(hf)->ops->name : "unknown");
            res = tmp;
        } else if (!strncmp(gp, "/proc/", 6) && strstr(gp, "/exe") && !strcmp(strstr(gp, "/exe"), "/exe")) {
            Proc *t = NULL;
            long pid = strtol(gp + 6, NULL, 10);
            LOCKED(t = pid ? proc_find((int)pid) : NULL);
            if (!t) { r = ERR(2); break; }
            res = t->exe;
        }
        if (res) {
            size_t n = strlen(res);
            if (n > a3) n = a3;
            memcpy(gptr(a2), res, n);
            r = n;
            break;
        }
        if ((e = resolve_at(p, (int)a0, gp, path, sizeof path, false, &hfd))) { r = ERR(-e); break; }
        r = HOSTRET(readlinkat(hfd, path, gptr(a2), a3));
        break;
    }
    case 48: case 439: {   // faccessat, faccessat2
        if ((e = resolve_at(p, (int)a0, gstr(a1), path, sizeof path, true, &hfd))) { r = ERR(-e); break; }
        r = HOSTRET(faccessat(hfd, path, (int)a2, 0));
        break;
    }
    case 61: h = HFD(a0); r = sys_getdents64(h, gptr(a1), a2); break;
    case 17: {   // getcwd
        size_t n = strlen(p->cwd) + 1;
        if (n > a1) { r = ERR(34); break; }
        memcpy(gptr(a0), p->cwd, n);
        r = n;
        break;
    }
    case 49: case 50: {   // chdir, fchdir
        if (nr == 50) {
            h = HFD(a0);
            if (!fd_get_path(h, path2, sizeof path2)) { r = ERR(20); break; }
        } else {
            snprintf(path2, sizeof path2, "%s", gstr(a0));
        }
        if ((e = host_path(p, path2, path, sizeof path, true))) { r = ERR(-e); break; }
        struct stat st;
        if (stat(path, &st)) { r = ERR(lx_errno(errno)); break; }
        if (!S_ISDIR(st.st_mode)) { r = ERR(20); break; }
        snprintf(p->cwd, sizeof p->cwd, "%s", guest_of(path));
        r = 0;
        break;
    }
    case 25: {   // fcntl
        h = HFD(a0);
        switch (a1) {
        case 0: case 1030: e = fd_dup(p, (int)a0, (int)a2, false, a1 == 1030); r = e < 0 ? ERR(-e) : (uint64_t)e; break;
        case 1: r = (uint64_t)fd_cloexec(p, (int)a0, -1); break;
        case 2: fd_cloexec(p, (int)a0, (int)(a2 & 1)); r = 0; break;
        case 3: { int f = fcntl(h, F_GETFL); r = f < 0 ? ERR(lx_errno(errno)) : lx_fl(f); break; }
        case 4: r = HOSTRET(fcntl(h, F_SETFL, open_flags(a2) & (O_NONBLOCK | O_APPEND))); break;
        case 5: *(int16_t *)gptr(a2) = 2; r = 0; break;   // F_GETLK: unlocked
        case 6: case 7: r = 0; break;                     // F_SETLK(W): no other locker here
        case 1031: case 1032: r = 65536; break;           // F_SETPIPE_SZ / F_GETPIPE_SZ
        case 1033: case 1034: r = OBJRET(vfs_memfd_seals(h, (int)a1, a2)); break;   // F_ADD_SEALS / F_GET_SEALS
        default: r = ERR(22);
        }
        break;
    }
    case 23: e = fd_dup(p, (int)a0, 0, false, false); r = e < 0 ? ERR(-e) : (uint64_t)e; break;
    case 24:     // dup3
        if (a0 == a1) { r = ERR(22); break; }
        e = fd_dup(p, (int)a0, (int)a1, true, a2 & 02000000);
        r = e < 0 ? ERR(-e) : (uint64_t)e;
        break;
    case 29: {   // ioctl
        h = HFD(a0);
        int64_t v = vfs_ioctl(c, h, a1, a2);
        if (v != VFS_PASS) { r = OBJRET(v); break; }
        if (a1 == 0x5413) { struct winsize ws; int rc = ioctl(h, TIOCGWINSZ, &ws); if (!rc) memcpy(gptr(a2), &ws, sizeof ws); r = HOSTRET(rc); }
        else if (a1 == 0x5421) { int on = *(int *)gptr(a2); r = HOSTRET(ioctl(h, FIONBIO, &on)); }
        else if (a1 == 0x5451 || a1 == 0x5450) { fd_cloexec(p, (int)a0, a1 == 0x5451); r = 0; }
        else r = ERR(25);   // ENOTTY: TCGETS and the rest
        break;
    }
    case 59: {   // pipe2
        int fds[2];
        if (pipe(fds)) { r = ERR(lx_errno(errno)); break; }
        if (a1 & 04000) { fcntl(fds[0], F_SETFL, O_NONBLOCK); fcntl(fds[1], F_SETFL, O_NONBLOCK); }
        int g0 = fd_install(p, fds[0], 0, a1 & 02000000);
        int g1 = g0 < 0 ? (close(fds[1]), g0) : fd_install(p, fds[1], 0, a1 & 02000000);
        if (g0 < 0 || g1 < 0) { if (g0 >= 0) fd_close(p, g0); r = ERR(24); break; }
        int32_t *out = gptr(a0);
        out[0] = g0;
        out[1] = g1;
        r = 0;
        break;
    }
    case 222: {   // mmap
        int64_t m;
        if (a3 & 0x20) m = mm_map(p->mm, a0, a1, (int)(a2 & 7), (int)a3, -1, 0, NULL);
        else {
            h = HFD(a4);
            FdObj *o = fd_obj(h);
            if (o && o->ops->mmap) { r = OBJRET(o->ops->mmap(o, c, a0, a1, (int)(a2 & 7), (int)a3, a5)); break; }
            if (o && o->mmap_fd < 0) { r = ERR(19); break; }          // ENODEV: not mappable
            int mfd = o ? o->mmap_fd : h;
            int fl = (int)a3 | (o && o->map_shared ? LX_MAP_SHARED : 0);
            bool known = fd_get_path(h, path2, sizeof path2);
            m = mm_map(p->mm, a0, a1, (int)(a2 & 7), fl, mfd, a5, known ? path2 : NULL);
            if (m >= 0 && o && o->ops->mapped) o->ops->mapped(o, c, (uint64_t)m, a1, (int)(a2 & 7), a5);
        }
        if (m >= 0 && (a3 & 0x10)) tr_invalidate(c->space, (uint64_t)m, PG_UP(a1));   // MAP_FIXED replaced code?
        r = m < 0 ? ERR(-m) : (uint64_t)m;
        break;
    }
    case 215: e = mm_unmap(p->mm, a0, a1); if (!e) tr_invalidate(c->space, a0, PG_UP(a1)); r = e ? ERR(-e) : 0; break;
    case 226: e = mm_protect(p->mm, a0, a1, (int)(a2 & 7)); r = e ? ERR(-e) : 0; break;
    case 233: r = a2 == 4 ? (uint64_t)mm_dontneed(p->mm, a0, a1) : 0; break;
    case 216: { int64_t m = mm_remap(p->mm, a0, a1, a2, (int)a3, a4); if (m >= 0 && (uint64_t)m != a0) tr_invalidate(c->space, a0, PG_UP(a1)); r = m < 0 ? ERR(-m) : (uint64_t)m; break; }
    case 214: r = 0; break;   // brk: no heap break, bionic uses mmap
    case 96: c->clear_child_tid = a0; r = c->tid; break;
    case 99: r = 0; break;    // set_robust_list
    case 134: {               // rt_sigaction
        if (a0 < 1 || a0 > 64 || (a1 && (a0 == 9 || a0 == 19))) { r = ERR(22); break; }
        if (a2) memcpy(gptr(a2), p->sigact[a0], 32);
        if (a1) {
            uint64_t sa[4];
            memcpy(sa, gptr(a1), 32);
            LOCKED(memcpy(p->sigact[a0], sa, 32));
        }
        r = 0;
        break;
    }
    case 135: {               // rt_sigprocmask
        uint64_t old = c->sigmask;
        if (a1) {
            uint64_t s = *(uint64_t *)gptr(a1);
            c->sigmask = (a0 == 0 ? (old | s) : a0 == 1 ? (old & ~s) : s) & ~((1ull << 8) | (1ull << 18));
        }
        if (a2) *(uint64_t *)gptr(a2) = old;
        r = 0;
        break;
    }
    case 139: proc_sigreturn(c); return;          // rt_sigreturn: registers come from the frame
    case 133: proc_sigsuspend(c, *(uint64_t *)gptr(a0)); return;
    case 132: {               // sigaltstack: stack_t { void *ss_sp; int ss_flags; size_t ss_size; }
        bool on_alt = c->altstack_size && c->sp > c->altstack_sp && c->sp <= c->altstack_sp + c->altstack_size;
        if (a1) {
            uint8_t *old = gptr(a1);
            memset(old, 0, 24);
            *(uint64_t *)old = c->altstack_sp;
            *(int32_t *)(old + 8) = c->altstack_size ? (on_alt ? 1 : 0) : 2;   // SS_ONSTACK / SS_DISABLE
            *(uint64_t *)(old + 16) = c->altstack_size;
        }
        if (a0) {
            const uint8_t *ns = gptr(a0);
            int32_t fl = *(int32_t *)(ns + 8);
            uint64_t sz = *(uint64_t *)(ns + 16);
            if (on_alt) { r = ERR(1); break; }                              // EPERM while running on it
            if (fl & 2) { c->altstack_sp = c->altstack_size = 0; }          // SS_DISABLE
            else if (sz < 2048) { r = ERR(12); break; }                      // ENOMEM: too small
            else { c->altstack_sp = *(uint64_t *)ns; c->altstack_size = sz; }
        }
        r = 0;
        break;
    }
    case 137: {               // rt_sigtimedwait(set, info, timeout, sigsetsize)
        int64_t v = proc_sigtimedwait(c, *(uint64_t *)gptr(a0), a1, a2 ? gptr(a2) : NULL);
        r = v < 0 ? ERR(lx_errno((int)-v)) : (uint64_t)v;
        break;
    }
    case 167:                 // prctl
        switch (a0) {
        case 15: snprintf(c->name, sizeof c->name, "%s", gstr(a1)); r = 0; break;          // PR_SET_NAME
        case 16: {                                                                          // PR_GET_NAME
            const char *n = c->name[0] ? c->name : (strrchr(p->exe, '/') ? strrchr(p->exe, '/') + 1 : p->exe);
            snprintf(gptr(a1), 16, "%s", n);
            r = 0;
            break;
        }
        case 3: r = 1; break;                                                               // PR_GET_DUMPABLE
        case 23: r = a1 <= 40 ? 1 : ERR(22); break;                                        // PR_CAPBSET_READ: caps 0..CAP_LAST_CAP
        case 24: r = a1 <= 40 ? 0 : ERR(22); break;                                        // PR_CAPBSET_DROP
        case 30: r = 50000; break;                                                          // PR_GET_TIMERSLACK
        case 55: r = ERR(22); break;         // PR_SET_TAGGED_ADDR_CTRL: refused, so bionic keeps heap pointers untagged
        case 56: r = 0; break;               // PR_GET_TAGGED_ADDR_CTRL: no MTE
        default: r = 0;                      // set-style requests: accepted
        }
        break;
    case 278: arc4random_buf(gptr(a0), a1); r = a1; break;   // getrandom
    case 160: {               // uname
        char *u = gptr(a0);
        memset(u, 0, 65 * 6);
        strcpy(u, "Linux"); strcpy(u + 65, "localhost"); strcpy(u + 130, "6.6.0-oni");
        strcpy(u + 195, "#1 SMP"); strcpy(u + 260, "aarch64"); strcpy(u + 325, "(none)");
        r = 0;
        break;
    }
    case 113: case 114: {     // clock_gettime, clock_getres
        int clk = lx_clock(a0);
        if (clk < 0) { r = ERR(22); break; }
        if (!a1) { r = 0; break; }
        r = HOSTRET(nr == 113 ? clock_gettime(clk, gptr(a1)) : clock_getres(clk, gptr(a1)));
        break;
    }
    case 169: {               // gettimeofday: Linux timeval has a 64-bit tv_usec
        struct timeval tv;
        int rc = gettimeofday(&tv, NULL);
        if (!rc && a0) { ((int64_t *)gptr(a0))[0] = tv.tv_sec; ((int64_t *)gptr(a0))[1] = tv.tv_usec; }
        r = HOSTRET(rc);
        break;
    }
    case 101: r = HOSTRET(nanosleep(gptr(a0), gptr(a1))); break;
    case 115: {               // clock_nanosleep
        struct timespec *t = gptr(a2);
        if (a1 & 1) {         // TIMER_ABSTIME
            struct timespec now;
            clock_gettime(lx_clock(a0), &now);
            struct timespec d = { t->tv_sec - now.tv_sec, t->tv_nsec - now.tv_nsec };
            if (d.tv_nsec < 0) { d.tv_sec--; d.tv_nsec += 1000000000; }
            r = d.tv_sec >= 0 ? HOSTRET(nanosleep(&d, NULL)) : 0;
        } else {
            r = HOSTRET(nanosleep(t, gptr(a3)));
        }
        break;
    }
    case 172: r = (uint64_t)p->pid; break;
    case 173: r = (uint64_t)p->ppid; break;
    case 178: r = c->tid; break;
    // Credentials: every guest process may change its ids (there is no capability check); real and
    // effective ids are tracked separately, the saved set follows the effective one.
    case 174: r = p->uid; break;                               // getuid
    case 175: r = p->euid; break;                              // geteuid
    case 176: r = p->gid; break;                               // getgid
    case 177: r = p->egid; break;                              // getegid
    case 146: p->uid = p->euid = (uint32_t)a0; r = 0; break;   // setuid
    case 144: p->gid = p->egid = (uint32_t)a0; r = 0; break;   // setgid
    case 145: if ((int32_t)a0 != -1) p->uid = (uint32_t)a0; if ((int32_t)a1 != -1) p->euid = (uint32_t)a1; r = 0; break;   // setreuid
    case 143: if ((int32_t)a0 != -1) p->gid = (uint32_t)a0; if ((int32_t)a1 != -1) p->egid = (uint32_t)a1; r = 0; break;   // setregid
    case 147: if ((int32_t)a0 != -1) p->uid = (uint32_t)a0; if ((int32_t)a1 != -1) p->euid = (uint32_t)a1; r = 0; break;   // setresuid
    case 149: if ((int32_t)a0 != -1) p->gid = (uint32_t)a0; if ((int32_t)a1 != -1) p->egid = (uint32_t)a1; r = 0; break;   // setresgid
    case 148: { uint32_t *o[3] = { gptr(a0), gptr(a1), gptr(a2) }; uint32_t v[3] = { p->uid, p->euid, p->euid }; for (int i = 0; i < 3; i++) if (o[i]) *o[i] = v[i]; r = 0; break; }   // getresuid
    case 150: { uint32_t *o[3] = { gptr(a0), gptr(a1), gptr(a2) }; uint32_t v[3] = { p->gid, p->egid, p->egid }; for (int i = 0; i < 3; i++) if (o[i]) *o[i] = v[i]; r = 0; break; }   // getresgid
    case 151: case 152: r = 0; break;                          // setfsuid, setfsgid
    case 159: {                                                // setgroups
        if (a0 > 32) { r = ERR(22); break; }
        p->ngroups = (int)a0;
        if (a0) memcpy(p->groups, gptr(a1), a0 * 4);
        r = 0;
        break;
    }
    case 97: case 40: case 39: case 41: case 51: case 268: r = 0; break;   // unshare, mount, umount2, pivot_root, chroot, setns: pretend
    case 277: r = 0; break;                                    // seccomp
    case 30: case 31: r = 0; break;                            // ioprio_set/get
    case 92: r = 0; break;                                     // personality
    case 164: r = 0; break;                                    // setrlimit
    case 19: r = OBJRET(vfs_eventfd(c, a0, (int)a1)); break;
    case 20: r = OBJRET(vfs_epoll_create(c, (int)a0)); break;
    case 21: r = OBJRET(vfs_epoll_ctl(c, (int)a0, (int)a1, (int)a2, a3)); break;
    case 22: r = OBJRET(vfs_epoll_wait(c, (int)a0, a1, (int)a2, (int64_t)(int32_t)a3, a4)); break;
    case 441: {               // epoll_pwait2: timespec timeout
        int64_t ms = -1;
        if (a3) { const struct timespec *t = gptr(a3); ms = t->tv_sec * 1000 + (t->tv_nsec + 999999) / 1000000; }
        r = OBJRET(vfs_epoll_wait(c, (int)a0, a1, (int)a2, ms, a4));
        break;
    }
    case 279: r = OBJRET(vfs_memfd(c, gstr(a0), (int)a1)); break;
    case 85: r = OBJRET(vfs_timerfd_create(c, (int)a0, (int)a1)); break;
    case 86: r = OBJRET(vfs_timerfd_settime(c, (int)a0, (int)a1, a2, a3)); break;
    case 87: r = OBJRET(vfs_timerfd_gettime(c, (int)a0, a1)); break;
    case 26: r = OBJRET(vfs_inotify_init(c, (int)a0)); break;        // inotify_init1
    case 27: r = 1; break;                                              // inotify_add_watch: a watch that never fires
    case 28: r = 0; break;                                              // inotify_rm_watch
    case 198: case 199: case 200: case 201: case 202: case 203: case 204: case 205: case 206: case 207:
    case 208: case 209: case 210: case 211: case 212: case 242: {
        r = OBJRET(sock_call(c, nr, a0, a1, a2, a3, a4, a5));
        break;
    }
    case 158: {               // getgroups
        if (a0 && (int)a0 < p->ngroups) { r = ERR(22); break; }
        if (a0) memcpy(gptr(a1), p->groups, (size_t)p->ngroups * 4);
        r = (uint64_t)p->ngroups;
        break;
    }
    case 165: memset(gptr(a1), 0, 144); r = 0; break;   // getrusage
    case 163: case 261: {     // getrlimit / prlimit64
        uint64_t res = nr == 163 ? a0 : a1, out = nr == 163 ? a1 : a3;
        if (out) {
            uint64_t *rl = gptr(out);
            if (res == 3) { rl[0] = 8 << 20; rl[1] = ~0ull; }
            else if (res == 7) { rl[0] = MAX_FDS; rl[1] = MAX_FDS; }
            else { rl[0] = ~0ull; rl[1] = ~0ull; }
        }
        r = 0;
        break;
    }
    case 179: {               // sysinfo
        uint8_t *s = gptr(a0);
        memset(s, 0, 112);
        *(int64_t *)s = (int64_t)(clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000000);
        uint64_t mem = 0;
        size_t sl = sizeof mem;
        sysctlbyname("hw.memsize", &mem, &sl, NULL, 0);
        *(uint64_t *)(s + 32) = mem;
        *(uint64_t *)(s + 40) = mem / 2;
        *(uint16_t *)(s + 80) = 1;
        *(uint32_t *)(s + 104) = 1;
        r = 0;
        break;
    }
    case 124: sched_yield(); r = 0; break;
    case 119: case 120: case 140: case 122: r = 0; break;     // sched_setscheduler/getscheduler, setpriority, setaffinity
    case 121: *(int32_t *)gptr(a1) = 0; r = 0; break;         // sched_getparam
    case 125: r = (a0 == 1 || a0 == 2) ? 99 : 0; break;       // sched_get_priority_max
    case 126: r = (a0 == 1 || a0 == 2) ? 1 : 0; break;        // sched_get_priority_min
    case 141: r = 20; break;                                  // getpriority (20 - nice)
    case 123: {               // sched_getaffinity
        int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
        memset(gptr(a2), 0, a1);
        for (int i = 0; i < n && i < (int)a1 * 8; i++) ((uint8_t *)gptr(a2))[i / 8] |= 1 << (i % 8);
        r = 8;
        break;
    }
    case 168: if (a0) *(uint32_t *)gptr(a0) = 0; if (a1) *(uint32_t *)gptr(a1) = 0; r = 0; break;   // getcpu
    case 98: r = sys_futex(a0, a1, a2, a3, a4, a5); break;
    case 93: proc_exit_thread(c, (int)a0);
    case 94: proc_exit_group(c, (int)a0);
    case 129: r = proc_kill(c, (int64_t)(int32_t)a0, (int)a1); break;
    case 138: r = proc_kill(c, (int64_t)(int32_t)a0, (int)a1); break;            // rt_sigqueueinfo
    case 130: r = proc_tgkill(c, -1, (int64_t)(int32_t)a0, (int)a1); break;      // tkill
    case 131: case 240: r = proc_tgkill(c, (int64_t)(int32_t)a0, (int64_t)(int32_t)a1, (int)a2); break;
    case 220: r = proc_clone(c, a0, a1, a2, a3, a4); break;
    case 260: r = proc_wait4(c, (int64_t)(int32_t)a0, a1, (int)a2, a3); break;
    case 95: {                // waitid(idtype, id, infop, options, rusage)
        int64_t which = a0 == 1 ? (int64_t)(int32_t)a1 : a0 == 2 ? -(int64_t)(int32_t)a1 : -1;   // P_PID, P_PGID, P_ALL
        if (a0 == 2 && a1 == 0) which = 0;
        int32_t status = 0;
        uint64_t ws = proc_wait4(c, which, 0, (int)(a3 & 1), 0);          // WNOHANG passes through
        if ((int64_t)ws < 0) { r = ws; break; }
        if (ws > 0) {
            Proc *dead = NULL;   // the status was consumed by proc_wait4; recover it from its return value
            (void)dead;
        }
        r = ws;
        (void)status;
        break;
    }
    case 166: r = p->umask; p->umask = (mode_t)(a0 & 0777); break;
    case 154: r = proc_setpgid(c, (int64_t)(int32_t)a0, (int64_t)(int32_t)a1); break;
    case 155: r = proc_getpgid(c, (int64_t)(int32_t)a0); break;
    case 156: r = (uint64_t)p->sid; break;
    case 157: r = proc_setsid(c); break;
    case 43: case 44: {       // statfs / fstatfs: the filesystem type depends on where the path is
        const char *gp = NULL;
        if (nr == 43) {
            if ((e = host_path(p, gstr(a0), path, sizeof path, true))) { r = ERR(-e); break; }
            struct stat st;
            if (stat(path, &st)) { r = ERR(lx_errno(errno)); break; }
            gp = guest_of(path);
        } else {
            h = HFD(a0);
            gp = fd_get_path(h, path2, sizeof path2) ? path2 : "/";
        }
        uint64_t type = 0xEF53;                                   // ext4
        if (!strncmp(gp, "/sys/fs/selinux", 15)) type = 0xf97cff8c;
        else if (!strncmp(gp, "/proc", 5)) type = 0x9fa0;
        else if (!strncmp(gp, "/sys", 4)) type = 0x62656572;
        else if (!strncmp(gp, "/dev", 4)) type = 0x01021994;      // tmpfs
        uint64_t *s = gptr(a1);
        memset(s, 0, 120);
        s[0] = type; s[1] = 4096; s[2] = 1 << 20; s[3] = 1 << 19; s[4] = 1 << 19;
        s[5] = 1 << 20; s[6] = 1 << 19; s[8] = 255; s[9] = 4096;
        r = 0;
        break;
    }
    case 46: h = HFD(a0); r = HOSTRET(ftruncate(h, (off_t)a1)); break;
    case 82: case 83: h = HFD(a0); r = HOSTRET(fsync(h)); break;
    case 81: r = 0; break;    // sync
    case 34: {               // mkdirat
        if ((e = resolve_at(p, (int)a0, gstr(a1), path, sizeof path, true, &hfd))) { r = ERR(-e); break; }
        r = HOSTRET(mkdirat(hfd, path, (mode_t)(a2 & ~p->umask)));
        break;
    }
    case 35: {               // unlinkat
        if ((e = resolve_at(p, (int)a0, gstr(a1), path, sizeof path, false, &hfd))) { r = ERR(-e); break; }
        r = HOSTRET(unlinkat(hfd, path, (a2 & L_AT_REMOVEDIR) ? AT_REMOVEDIR : 0));
        break;
    }
    case 38: case 276: {     // renameat, renameat2
        if ((e = resolve_at(p, (int)a0, gstr(a1), path, sizeof path, false, &hfd)) ||
            (e = resolve_at(p, (int)a2, gstr(a3), path2, sizeof path2, false, &hfd2))) { r = ERR(-e); break; }
        if (nr == 276 && a4) r = HOSTRET(renameatx_np(hfd, path, hfd2, path2, (a4 & 1) ? RENAME_EXCL : RENAME_SWAP));
        else r = HOSTRET(renameat(hfd, path, hfd2, path2));
        break;
    }
    case 36: {               // symlinkat(target, dirfd, linkpath)
        if ((e = resolve_at(p, (int)a1, gstr(a2), path, sizeof path, false, &hfd))) { r = ERR(-e); break; }
        r = HOSTRET(symlinkat(gstr(a0), hfd, path));
        break;
    }
    case 37: {               // linkat
        if ((e = resolve_at(p, (int)a0, gstr(a1), path, sizeof path, false, &hfd)) ||
            (e = resolve_at(p, (int)a2, gstr(a3), path2, sizeof path2, false, &hfd2))) { r = ERR(-e); break; }
        r = HOSTRET(linkat(hfd, path, hfd2, path2, 0));
        break;
    }
    case 53: case 52: case 54: case 55: case 88: case 452: r = 0; break;   // chmod/chown/utimens/fchmodat2: accept
    case 73: {               // ppoll
        int timeout = -1;
        if (a2) { struct timespec *t = gptr(a2); timeout = (int)(t->tv_sec * 1000 + t->tv_nsec / 1000000); }
        uint64_t n = a1;
        if (n > 4096) { r = ERR(22); break; }
        struct pollfd *g = gptr(a0), *hp = calloc(n ? n : 1, sizeof *hp);
        for (uint64_t i = 0; i < n; i++) {
            hp[i].fd = g[i].fd < 0 ? -1 : fd_host(p, g[i].fd);
            hp[i].events = g[i].events;
        }
        int rc = poll(hp, (nfds_t)n, timeout);
        for (uint64_t i = 0; i < n; i++)
            g[i].revents = g[i].fd >= 0 && hp[i].fd < 0 ? POLLNVAL : hp[i].revents;
        free(hp);
        r = HOSTRET(rc);
        break;
    }
    case 90:                        // capget: no capabilities
        if (a1) memset(gptr(a1), 0, 24);
        r = 0;
        break;
    case 91: r = 0; break;          // capset
    case 270: case 271: {           // process_vm_readv / writev: only within this process
        if ((int64_t)(int32_t)a0 != p->pid) { r = ERR(1); break; }
        struct iovec *lv = iov_in(a1, a2), *rv = iov_in(a3, a4);
        if (!lv || !rv) { free(lv); free(rv); r = ERR(22); break; }
        uint64_t done = 0;
        size_t li = 0, loff = 0;
        for (uint64_t i = 0; i < a4 && li < a2; i++) {
            uint64_t raddr = ((uint64_t *)gptr(a3))[2 * i], rlen = rv[i].iov_len;
            for (uint64_t off = 0; off < rlen && li < a2;) {
                size_t n = rlen - off;
                if (n > lv[li].iov_len - loff) n = lv[li].iov_len - loff;
                if (!mm_readable(p->mm, raddr + off, n)) goto vm_done;
                if (nr == 270) memcpy((uint8_t *)lv[li].iov_base + loff, (uint8_t *)rv[i].iov_base + off, n);
                else memcpy((uint8_t *)rv[i].iov_base + off, (uint8_t *)lv[li].iov_base + loff, n);
                off += n; loff += n; done += n;
                if (loff == lv[li].iov_len) { li++; loff = 0; }
            }
        }
    vm_done:
        free(lv); free(rv);
        r = done ? done : ERR(14);
        break;
    }
    case 102: case 103: if (nr == 103 && a2) memset(gptr(a2), 0, 32); if (nr == 102) memset(gptr(a1), 0, 32); r = 0; break;   // getitimer/setitimer: accepted, never fire
    // Extended attributes live on the host files (APFS keeps any name), so SELinux labels that
    // restorecon writes read back. An unlabeled file reports a generic data label.
    case 5: case 6: case 7: {                                           // setxattr, lsetxattr, fsetxattr
        const char *name = gstr(a1);
        if (!name) { r = ERR(14); break; }
        int rc;
        if (nr == 7) { h = HFD(a0); rc = fsetxattr(h, name, gptr(a2), a3, 0, 0); }
        else { if ((e = host_path(p, gstr(a0), path, sizeof path, nr == 5))) { r = ERR(-e); break; } rc = setxattr(path, name, gptr(a2), a3, 0, nr == 6 ? XATTR_NOFOLLOW : 0); }
        r = rc < 0 ? ERR(lx_errno(errno)) : 0;
        break;
    }
    case 8: case 9: case 10: {                                          // getxattr, lgetxattr, fgetxattr
        const char *name = gstr(a1);
        if (!name) { r = ERR(14); break; }
        ssize_t n;
        char tmp[4096];
        if (nr == 10) { h = HFD(a0); n = fgetxattr(h, name, tmp, sizeof tmp, 0, 0); }
        else { if ((e = host_path(p, gstr(a0), path, sizeof path, nr == 8))) { r = ERR(-e); break; } n = getxattr(path, name, tmp, sizeof tmp, 0, nr == 9 ? XATTR_NOFOLLOW : 0); }
        if (n < 0 && errno == ENOATTR && !strcmp(name, "security.selinux")) { strcpy(tmp, "u:object_r:system_data_file:s0"); n = (ssize_t)strlen(tmp) + 1; }
        if (n < 0) { r = ERR(errno == ENOATTR ? 61 : lx_errno(errno)); break; }
        if (a3 == 0) { r = (uint64_t)n; break; }
        if (a3 < (uint64_t)n) { r = ERR(34); break; }
        memcpy(gptr(a2), tmp, (size_t)n);
        r = (uint64_t)n;
        break;
    }
    case 11: case 12: case 13: r = 0; break;                            // listxattr family: none
    case 14: case 15: case 16: {                                        // removexattr, lremovexattr, fremovexattr
        const char *name = gstr(a1);
        if (!name) { r = ERR(14); break; }
        if (nr == 16) { h = HFD(a0); fremovexattr(h, name, 0); }
        else if (!host_path(p, gstr(a0), path, sizeof path, nr == 14)) removexattr(path, name, nr == 15 ? XATTR_NOFOLLOW : 0);
        r = 0;
        break;
    }
    case 283: r = proc_membarrier(c, (int)a0); break;   // membarrier
    case 293: case 435: case 291: case 282: case 280: case 117: r = ERR(38); break;   // rseq, clone3, statx, userfaultfd, bpf, ptrace
    case 232: {               // mincore: one byte per page; the guest's page size is the host's (AT_PAGESZ)
        uint64_t len = PG_UP(a1);
        if (a0 & (PG - 1)) { r = ERR(22); break; }
        if (!len || a0 + len > WIN_SIZE) { r = ERR(12); break; }
        char *hv = calloc(len / PG + 1, 1);
        if (mincore((void *)(uintptr_t)(c->base + a0), len, hv)) { free(hv); r = ERR(lx_errno(errno)); break; }
        uint8_t *out = gptr(a2);
        for (uint64_t i = 0; i < len / PG; i++) out[i] = hv[i] & 1;
        free(hv);
        r = 0;
        break;
    }
    case 281:                       // execveat(dirfd, path, argv, envp, flags)
        if (gstr(a1)[0] != '/') { r = ERR(38); break; }
        a0 = a1; a1 = a2; a2 = a3;
        /* fall through */
    case 221:                       // execve
        r = proc_execve(c, gstr(a0), a1, a2);
        break;
    default:
        fprintf(stderr, "onirun: unimplemented syscall %llu\n", (unsigned long long)nr);
        break;
    }
#undef HFD
    LOGV("[%llu] syscall %llu(%llx, %llx, %llx) = %lld\n", (unsigned long long)c->tid, (unsigned long long)nr,
         (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2, (long long)r);
    // A host call interrupted by a kick: run it again unless a handler must see EINTR. Re-running
    // means re-executing the svc (after the handler, if there is one), as Linux does.
    if (r == ERR(4) && !c->kill) {
        int s = proc_deliverable(c);
        if (!s || ((p->sigact[s][1] & 0x10000000) && restartable(nr))) { c->pc -= 4; return; }
    }
    c->x[0] = r;
}

// Instructions the translator hands back (system registers EL0 can't read on Darwin, ...).
int sys_emulate(GuestCtx *c, uint64_t pc, uint32_t insn) {
    (void)pc;
    if ((insn & 0xfff00000) == 0xd5300000) {           // mrs
        int t = insn & 31;
        unsigned op1 = (insn >> 16) & 7, crn = (insn >> 12) & 15, crm = (insn >> 8) & 15, op2 = (insn >> 5) & 7;
        uint64_t v = 0;
        if (op1 == 0 && crn == 0 && crm == 0 && op2 == 0) v = 0x611f0220;   // MIDR_EL1
        if (op1 == 0 && crn == 0 && crm == 0 && op2 == 5) v = 0x80000000;   // MPIDR_EL1
        if (op1 == 3 && crn == 0 && crm == 0 && op2 == 1) v = 0x8444c004;   // CTR_EL0: 64-byte lines, PIPT
        if (t != 31) c->x[t] = v;
        return 0;
    }
    if ((insn & 0xfff00000) == 0xd5100000) return 0;   // msr: ignore
    if ((insn & 0xffffffe0) == 0xd50b7520) {           // ic ivau, Xt
        uint64_t a = (insn & 31) == 31 ? 0 : c->x[insn & 31];
        tr_invalidate_later(c->space, a & ~63ull, 64);
        return 0;
    }
    if (insn == 0xd5033fdf) { tr_invalidate(c->space, 0, 0); return 0; }   // isb: apply pending invalidations
    return -1;
}

uint64_t sys_code_end(GuestCtx *c, uint64_t pc) { return mm_exec_end(c->proc->mm, pc); }

void sys_init(const char *rootfs) {
    if (!realpath(rootfs, g_rootfs)) die("onirun: rootfs %s: %s", rootfs, strerror(errno));
    if ((uint64_t)getpagesize() != PG) die("onirun: expected 16 KiB host pages");
}
