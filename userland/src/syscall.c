// Linux (arm64) system-call personality on Darwin.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <os/os_sync_wait_on_address.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "onx.h"

char g_rootfs[1024];
static const char *gstr(uint64_t a) { return (const char *)(uintptr_t)a; }
static void *gptr(uint64_t a) { return (void *)(uintptr_t)a; }
static char g_cwd[1024] = "/";
char g_exe_path[1024];

static uint64_t g_sigactions[65][4];

// Protects the syscall layer's tables (fd paths, directory streams, mapping notes, threads).
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCKED(stmt) do { pthread_mutex_lock(&g_lock); stmt; pthread_mutex_unlock(&g_lock); } while (0)

// Guest path of each open fd, for /proc/self/fd/N (bionic's realpath() relies on it).
#define MAX_FDS 4096
static char *fd_path[MAX_FDS];
static void fd_set_path_locked(int fd, const char *p) {
    if (fd < 0 || fd >= MAX_FDS) return;
    free(fd_path[fd]);
    fd_path[fd] = p ? strdup(p) : NULL;
}
static void fd_set_path(int fd, const char *p) { LOCKED(fd_set_path_locked(fd, p)); }
static void fd_copy(int from, int to) {
    if (from >= 0 && from < MAX_FDS && to >= 0 && to < MAX_FDS) LOCKED(fd_set_path_locked(to, fd_path[from]));
}

// ---- errno ----
static int lx_errno(int e) {
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
#define ERR(e) ((uint64_t)(int64_t)-(e))
#define HOSTRET(expr) ({ __typeof__(expr) _r = (expr); _r < 0 ? ERR(lx_errno(errno)) : (uint64_t)_r; })

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
int host_path(const char *guest, int dirfd, char *out, size_t outlen, bool follow_last) {
    char work[4096];
    if (!strcmp(guest, "/proc/self/exe") && g_exe_path[0]) guest = g_exe_path;
    if (guest[0] == '/') {
        if (host_passthrough(guest)) { snprintf(out, outlen, "%s", guest); return 0; }
        snprintf(work, sizeof work, "%s", guest);
    } else if (dirfd == L_AT_FDCWD) {
        snprintf(work, sizeof work, "%s/%s", g_cwd, guest);
    } else {
        // Relative to an open directory: let the host resolve it (see callers).
        return -EBADF;
    }
    char resolved[4096] = "";   // guest path built so far, "" = root
    char rest[4096];
    snprintf(rest, sizeof rest, "%s", work);
    int links = 0;
    char *p = rest;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char comp[256];
        if (n >= sizeof comp) return -36;
        memcpy(comp, p, n);
        comp[n] = 0;
        p += n;
        bool last = true;
        for (char *q = p; *q; q++) if (*q != '/') { last = false; break; }
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) {
            char *s = strrchr(resolved, '/');
            if (s) *s = 0; else resolved[0] = 0;
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
            snprintf(next, sizeof next, "%s/%s", target, p);
            snprintf(rest, sizeof rest, "%s", next);
            p = rest;
            if (target[0] == '/') resolved[0] = 0;
            continue;
        }
        snprintf(resolved, sizeof resolved, "%s", cand);
    }
    snprintf(out, outlen, "%s%s", g_rootfs, resolved[0] ? resolved : "/");
    return 0;
}

// Resolves (dirfd, path) for an *at() call: on success either *hostfd = AT_FDCWD and out holds an
// absolute host path, or *hostfd = dirfd and out holds the relative path unchanged.
static int resolve_at(int dirfd, const char *path, char *out, size_t outlen, bool follow, int *hostfd) {
    if (path[0] != '/' && dirfd != L_AT_FDCWD) {
        snprintf(out, outlen, "%s", path);
        *hostfd = dirfd;
        return 0;
    }
    *hostfd = AT_FDCWD;
    return host_path(path, L_AT_FDCWD, out, outlen, follow);
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
    if (f & 02000000) r |= O_CLOEXEC;
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
static void put_stat(uint64_t addr, const struct stat *st) {
    uint8_t b[128] = {0};
    *(uint64_t *)(b + 0) = (uint64_t)st->st_dev;
    *(uint64_t *)(b + 8) = st->st_ino;
    *(uint32_t *)(b + 16) = st->st_mode;
    *(uint32_t *)(b + 20) = st->st_nlink;
    *(uint32_t *)(b + 24) = 2000;            // files appear owned by the shell user
    *(uint32_t *)(b + 28) = 2000;
    *(uint64_t *)(b + 32) = (uint64_t)st->st_rdev;
    *(int64_t *)(b + 48) = st->st_size;
    *(int32_t *)(b + 56) = st->st_blksize;
    *(int64_t *)(b + 64) = st->st_blocks;
    *(int64_t *)(b + 72) = st->st_atimespec.tv_sec;
    *(uint64_t *)(b + 80) = st->st_atimespec.tv_nsec;
    *(int64_t *)(b + 88) = st->st_mtimespec.tv_sec;
    *(uint64_t *)(b + 96) = st->st_mtimespec.tv_nsec;
    *(int64_t *)(b + 104) = st->st_ctimespec.tv_sec;
    *(uint64_t *)(b + 112) = st->st_ctimespec.tv_nsec;
    memcpy((void *)(uintptr_t)addr, b, sizeof b);
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

// ---- directories ----
typedef struct DirState { int fd; DIR *d; struct DirState *next; } DirState;
static DirState *dirs;
static DirState *dir_for(int fd) {
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
static void dir_forget_locked(int fd) {
    for (DirState **pp = &dirs; *pp; pp = &(*pp)->next)
        if ((*pp)->fd == fd) { DirState *s = *pp; *pp = s->next; closedir(s->d); free(s); return; }
}
static void dir_forget(int fd) { LOCKED(dir_forget_locked(fd)); }
static uint64_t sys_getdents64(int fd, uint8_t *buf, size_t len) {
    DirState *s = dir_for(fd);
    if (!s) return ERR(9);
    size_t used = 0;
    for (;;) {
        long pos = telldir(s->d);
        errno = 0;
        struct dirent *e = readdir(s->d);
        if (!e) break;
        size_t nl = strlen(e->d_name);
        size_t rl = (19 + nl + 1 + 7) & ~7ul;
        if (used + rl > len) {
            seekdir(s->d, pos);
            if (!used) return ERR(22);
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
    return used;
}

// ---- file mappings (for symbolizing guest addresses) ----
typedef struct MapRec { uint64_t start, end, off; char *path; struct MapRec *next; } MapRec;
static MapRec *maps;
static void map_note(uint64_t start, uint64_t len, uint64_t off, int fd) {
    pthread_mutex_lock(&g_lock);
    if (fd >= 0 && fd < MAX_FDS && fd_path[fd]) {
        MapRec *m = calloc(1, sizeof *m);
        m->start = start; m->end = start + len; m->off = off; m->path = strdup(fd_path[fd]);
        m->next = maps; maps = m;
    }
    pthread_mutex_unlock(&g_lock);
}
void sys_note_mapping(uint64_t start, uint64_t len, uint64_t off, const char *path) {
    MapRec *m = calloc(1, sizeof *m);
    m->start = start; m->end = start + len; m->off = off; m->path = strdup(path);
    m->next = maps; maps = m;
}
void sys_symbolize(uint64_t addr, char *out, size_t n) {
    for (MapRec *m = maps; m; m = m->next)
        if (addr >= m->start && addr < m->end) {
            snprintf(out, n, "%s+0x%llx", m->path, (unsigned long long)(addr - m->start + m->off));
            return;
        }
    snprintf(out, n, "0x%llx", (unsigned long long)addr);
}
void sys_backtrace(GuestCtx *c) {
    char b[1200];
    sys_symbolize(c->pc, b, sizeof b); fprintf(stderr, "  pc %s\n", b);
    sys_symbolize(c->x[30], b, sizeof b); fprintf(stderr, "  lr %s\n", b);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 16 && fp && !(fp & 7); i++) {
        uint64_t ret = ((uint64_t *)(uintptr_t)fp)[1];
        sys_symbolize(ret & 0x0000ffffffffffffull, b, sizeof b);
        fprintf(stderr, "  #%d %s\n", i, b);
        fp = ((uint64_t *)(uintptr_t)fp)[0];
    }
}

// ---- memory ----
static int host_prot(uint64_t p) {
    int r = (int)(p & (PROT_READ | PROT_WRITE));
    if (p & PROT_EXEC) r |= PROT_READ;   // guest code is read by the translator, never run in place
    return r;
}

static uint64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, int64_t fd, uint64_t off) {
    int hf = (flags & 1) ? MAP_SHARED : MAP_PRIVATE;
    bool fixed = flags & 0x10, noreplace = flags & 0x100000, anon = flags & 0x20;
    if (fixed) hf |= MAP_FIXED;
    if (anon) { hf |= MAP_ANON; fd = -1; }
    int hp = host_prot(prot);
    long pg = getpagesize();
    if (!anon && (off % pg)) {
        // Offset not aligned to the 16 KiB host page: copy instead of mapping the file.
        void *m = mmap((void *)(uintptr_t)addr, len, PROT_READ | PROT_WRITE, (hf & ~MAP_SHARED) | MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) return ERR(lx_errno(errno));
        if (pread((int)fd, m, len, (off_t)off) < 0) { munmap(m, len); return ERR(lx_errno(errno)); }
        mprotect(m, len, hp);
        return (uint64_t)(uintptr_t)m;
    }
    void *m = mmap((void *)(uintptr_t)addr, len, hp, hf, (int)fd, (off_t)off);
    if (m == MAP_FAILED) return ERR(lx_errno(errno));
    if (!anon) map_note((uint64_t)(uintptr_t)m, len, off, (int)fd);
    if (noreplace && addr && (uint64_t)(uintptr_t)m != addr) { munmap(m, len); return ERR(17); }
    return (uint64_t)(uintptr_t)m;
}

static int region_prot(uint64_t addr) {
    mach_vm_address_t a = addr;
    mach_vm_size_t size;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj;
    if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
        return PROT_READ | PROT_WRITE;
    return info.protection & (PROT_READ | PROT_WRITE);
}

static uint64_t sys_madvise(uint64_t addr, uint64_t len, uint64_t advice) {
    if (advice == 4) {   // MADV_DONTNEED: Linux gives zero pages back; Darwin keeps the data.
        int p = region_prot(addr);
        void *m = mmap((void *)(uintptr_t)addr, len, p, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        return m == MAP_FAILED ? ERR(lx_errno(errno)) : 0;
    }
    return 0;
}

static uint64_t sys_mremap(uint64_t old, uint64_t oldsz, uint64_t newsz, uint64_t flags, uint64_t newaddr) {
    if (newsz <= oldsz) {
        if (newsz < oldsz) munmap((void *)(uintptr_t)(old + newsz), oldsz - newsz);
        return old;
    }
    if (!(flags & 1)) return ERR(12);
    int p = region_prot(old);
    void *m = mmap(NULL, newsz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) return ERR(12);
    memcpy(m, (void *)(uintptr_t)old, oldsz);
    mprotect(m, newsz, p);
    munmap((void *)(uintptr_t)old, oldsz);
    return (uint64_t)(uintptr_t)m;
}

// ---- signals ----
static int host_sig(uint64_t s) {
    static const int map[32] = {0, 1, 2, 3, 4, 5, 6, SIGBUS, 8, 9, SIGUSR1, 11, SIGUSR2, 13, 14, 15, 0,
                                SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU, SIGURG, SIGXCPU,
                                SIGXFSZ, SIGVTALRM, SIGPROF, SIGWINCH, SIGIO, 0, SIGSYS};
    return s < 32 ? map[s] : 0;
}
static int lx_sig(int s) {
    for (int i = 1; i < 32; i++) if (host_sig(i) == s) return i;
    return s;
}
static int lx_wstatus(int st) {
    if (WIFEXITED(st)) return (WEXITSTATUS(st) & 0xff) << 8;
    if (WIFSIGNALED(st)) return lx_sig(WTERMSIG(st)) | (WCOREDUMP(st) ? 0x80 : 0);
    if (WIFSTOPPED(st)) return (lx_sig(WSTOPSIG(st)) << 8) | 0x7f;
    return st;
}

// Guest signal delivery. Host handlers only mark signals pending; the dispatcher delivers them
// between translated blocks by building a Linux arm64 rt_sigframe on the guest stack.
static volatile uint64_t g_pending;   // bit (n-1) = Linux signal n
static uint64_t g_sigtramp;            // "mov x8, #rt_sigreturn; svc #0", like the vDSO's
static void host_sig_handler(int hs) {
    int ls = lx_sig(hs);
    if (ls > 0 && ls < 65) __atomic_or_fetch(&g_pending, 1ull << (ls - 1), __ATOMIC_SEQ_CST);
}
static void update_host_action(int lsig) {
    int hs = host_sig((uint64_t)lsig);
    if (!hs || hs == SIGKILL || hs == SIGSTOP || hs == SIGSEGV || hs == SIGBUS || hs == SIGILL ||
        hs == SIGTRAP || hs == SIGFPE) return;
    uint64_t h = g_sigactions[lsig][0], fl = g_sigactions[lsig][1];
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h == 0 ? SIG_DFL : h == 1 ? SIG_IGN : host_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = (fl & 0x10000000) ? SA_RESTART : 0;
    sigaction(hs, &sa, NULL);
}

#define FRAME_SIZE 4688   // siginfo (128) + arm64 ucontext (4560)
static void push_signal(GuestCtx *c, int sig, uint64_t saved_mask) {
    uint64_t handler = g_sigactions[sig][0], flags = g_sigactions[sig][1];
    uint64_t restorer = g_sigactions[sig][2], mask = g_sigactions[sig][3];
    LOGV("[%d] push signal %d: pc %llx sp %llx x19 %llx [sp+8] %llx\n", getpid(), sig,
         (unsigned long long)c->pc, (unsigned long long)c->sp, (unsigned long long)c->x[19],
         (unsigned long long)((uint64_t *)gptr(c->sp))[1]);
    uint64_t sp = (c->sp - FRAME_SIZE - 32) & ~15ull;
    uint8_t *f = gptr(sp);
    memset(f, 0, FRAME_SIZE);
    *(int32_t *)f = sig;                                   // siginfo.si_signo
    *(int32_t *)(f + 8) = sig == 17 ? 1 : 0;               // si_code (CLD_EXITED / SI_USER)
    uint8_t *uc = f + 128;
    *(uint64_t *)(uc + 40) = saved_mask;                   // uc_sigmask
    uint8_t *mc = uc + 176;                                // uc_mcontext
    memcpy(mc + 8, c->x, 31 * 8);
    *(uint64_t *)(mc + 256) = c->sp;
    *(uint64_t *)(mc + 264) = c->pc;
    *(uint64_t *)(mc + 272) = c->nzcv;
    uint8_t *rec = mc + 288;                               // __reserved: fpsimd_context
    *(uint32_t *)rec = 0x46508001;
    *(uint32_t *)(rec + 4) = 528;
    *(uint32_t *)(rec + 8) = (uint32_t)c->fpsr;
    *(uint32_t *)(rec + 12) = (uint32_t)c->fpcr;
    memcpy(rec + 16, c->q, 512);
    c->x[0] = (uint64_t)sig;
    c->x[1] = sp;
    c->x[2] = sp + 128;
    // Return path: sa_restorer if given, otherwise the vDSO-style trampoline (arm64 bionic
    // relies on the kernel's vDSO sigreturn and doesn't set SA_RESTORER).
    c->x[30] = (flags & 0x04000000) ? restorer : g_sigtramp;
    c->sp = sp;
    c->pc = handler;
    c->sigmask |= mask | ((flags & 0x40000000) ? 0 : 1ull << (sig - 1));   // SA_NODEFER
    if (flags & 0x80000000) { g_sigactions[sig][0] = 0; update_host_action(sig); }   // SA_RESETHAND
}

static void sigreturn(GuestCtx *c) {
    uint8_t *f = gptr(c->sp), *uc = f + 128, *mc = uc + 176;
    memcpy(c->x, mc + 8, 31 * 8);
    c->sp = *(uint64_t *)(mc + 256);
    c->pc = *(uint64_t *)(mc + 264);
    c->nzcv = *(uint64_t *)(mc + 272) & 0xf0000000ull;
    for (uint8_t *rec = mc + 288; rec < mc + 288 + 4096;) {
        uint32_t magic = *(uint32_t *)rec, size = *(uint32_t *)(rec + 4);
        if (!magic || size < 8) break;
        if (magic == 0x46508001) {
            c->fpsr = *(uint32_t *)(rec + 8);
            c->fpcr = *(uint32_t *)(rec + 12);
            memcpy(c->q, rec + 16, 512);
        }
        rec += size;
    }
    c->sigmask = *(uint64_t *)(uc + 40) & ~((1ull << 8) | (1ull << 18));   // never block KILL/STOP
    LOGV("[%d] sigreturn: pc %llx sp %llx x19 %llx [sp+8] %llx\n", getpid(), (unsigned long long)c->pc,
         (unsigned long long)c->sp, (unsigned long long)c->x[19], (unsigned long long)((uint64_t *)gptr(c->sp))[1]);
}

// Takes one deliverable signal (thread-directed first, then process-wide), or returns 0.
static int take_signal(GuestCtx *c) {
    for (;;) {
        uint64_t mine = c->pending & ~c->sigmask;
        if (mine) {
            int sig = __builtin_ctzll(mine) + 1;
            uint64_t bit = 1ull << (sig - 1);
            if (__atomic_fetch_and(&c->pending, ~bit, __ATOMIC_SEQ_CST) & bit) return sig;
            continue;
        }
        uint64_t pend = g_pending & ~c->sigmask;
        if (!pend) return 0;
        int sig = __builtin_ctzll(pend) + 1;
        uint64_t bit = 1ull << (sig - 1);
        if (__atomic_fetch_and(&g_pending, ~bit, __ATOMIC_SEQ_CST) & bit) return sig;
    }
}

void sys_deliver_signals(GuestCtx *c) {
    if (!((c->pending | g_pending) & ~c->sigmask)) return;
    int sig = take_signal(c);
    if (!sig) return;
    LOGV("[%d] deliver signal %d at pc %llx sp %llx\n", getpid(), sig, (unsigned long long)c->pc, (unsigned long long)c->sp);
    if (g_sigactions[sig][0] > 1) push_signal(c, sig, c->sigmask);
}

// ---- threads ----
static GuestCtx *g_threads;
static int g_nthreads;
static uint32_t g_next_tid = 0x40000000;   // guest thread ids, distinct from host pids

void sys_register_main_thread(GuestCtx *c) {
    c->host_thread = (void *)pthread_self();
    LOCKED({ c->next_thread = g_threads; g_threads = c; g_nthreads++; });
}

static GuestCtx *find_thread(uint64_t tid) {
    GuestCtx *t;
    pthread_mutex_lock(&g_lock);
    for (t = g_threads; t && t->tid != tid; t = t->next_thread) {}
    pthread_mutex_unlock(&g_lock);
    return t;
}

static void *guest_thread_main(void *arg) {
    GuestCtx *c = arg;
    stack_t ss = { .ss_sp = malloc(1 << 16), .ss_size = 1 << 16 };
    sigaltstack(&ss, NULL);
    c->host_thread = (void *)pthread_self();
    tr_run(c);
    return NULL;
}

// ---- futex ----
static uint64_t futex_wake(uint64_t addr, uint64_t n, bool shared) {
    os_sync_wake_by_address_flags_t f = shared ? OS_SYNC_WAKE_BY_ADDRESS_SHARED : OS_SYNC_WAKE_BY_ADDRESS_NONE;
    if (n >= 0x7fffffff) return os_sync_wake_by_address_all(gptr(addr), 4, f) == 0 ? 1 : 0;
    uint64_t woken = 0;
    while (woken < n && os_sync_wake_by_address_any(gptr(addr), 4, f) == 0) woken++;
    return woken;
}

static uint64_t sys_futex(uint64_t addr, uint64_t op_full, uint64_t val, uint64_t ts, uint64_t addr2, uint64_t val3) {
    int op = (int)(op_full & 0x7f);
    bool shared = !(op_full & 128), realtime = op_full & 256;
    switch (op) {
    case 0: case 9: {          // FUTEX_WAIT(_BITSET)
        os_sync_wait_on_address_flags_t f = shared ? OS_SYNC_WAIT_ON_ADDRESS_SHARED : OS_SYNC_WAIT_ON_ADDRESS_NONE;
        if (__atomic_load_n((uint32_t *)gptr(addr), __ATOMIC_SEQ_CST) != (uint32_t)val) return ERR(11);
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
            rc = os_sync_wait_on_address_with_timeout(gptr(addr), (uint32_t)val, 4, f, OS_CLOCK_MACH_ABSOLUTE_TIME, (uint64_t)ns);
        } else {
            rc = os_sync_wait_on_address(gptr(addr), (uint32_t)val, 4, f);
        }
        if (rc < 0) return errno == ETIMEDOUT ? ERR(110) : errno == EINTR ? ERR(4) : 0;
        return 0;
    }
    case 1: case 10:           // FUTEX_WAKE(_BITSET)
        return futex_wake(addr, val, shared);
    case 3: case 4: {          // FUTEX_(CMP_)REQUEUE: wake everyone on both (spurious wakeups are legal)
        if (op == 4 && __atomic_load_n((uint32_t *)gptr(addr), __ATOMIC_SEQ_CST) != (uint32_t)val3) return ERR(11);
        uint64_t n = futex_wake(addr, 0x7fffffff, shared);
        futex_wake(addr2, 0x7fffffff, shared);
        return n;
    }
    case 5:                    // FUTEX_WAKE_OP: approximate as wake both
        return futex_wake(addr, val, shared) + futex_wake(addr2, ts, shared);
    default:
        return ERR(38);
    }
}

// ---- system call table ----

void sys_dispatch(GuestCtx *c) {
    uint64_t nr = c->x[8], a0 = c->x[0], a1 = c->x[1], a2 = c->x[2], a3 = c->x[3], a4 = c->x[4], a5 = c->x[5];
    uint64_t r = ERR(38);
    char path[4096];
    int hfd, e;
    switch (nr) {
    case 56: {   // openat
        if ((e = resolve_at((int)a0, gstr(a1), path, sizeof path, !(a2 & 0100000), &hfd))) { r = ERR(-e); break; }
        int fd = openat(hfd, path, open_flags(a2), (int)a3);
        r = HOSTRET(fd);
        if (fd >= 0) fd_set_path(fd, hfd == AT_FDCWD ? (path[strlen(g_rootfs)] ? path + strlen(g_rootfs) : "/") : NULL);
        LOGV("  openat(%s) -> %lld\n", gstr(a1), (long long)r);
        break;
    }
    case 57: dir_forget((int)a0); fd_set_path((int)a0, NULL); r = HOSTRET(close((int)a0)); break;
    case 63: r = HOSTRET(read((int)a0, gptr(a1), a2)); break;
    case 64: r = HOSTRET(write((int)a0, gptr(a1), a2)); break;
    case 65: r = HOSTRET(readv((int)a0, gptr(a1), (int)a2)); break;
    case 66: r = HOSTRET(writev((int)a0, gptr(a1), (int)a2)); break;
    case 67: r = HOSTRET(pread((int)a0, gptr(a1), a2, (off_t)a3)); break;
    case 68: r = HOSTRET(pwrite((int)a0, gptr(a1), a2, (off_t)a3)); break;
    case 62: r = HOSTRET(lseek((int)a0, (off_t)a1, (int)a2)); break;
    case 79: {   // newfstatat
        struct stat st;
        int rc;
        if ((a3 & L_AT_EMPTY_PATH) && !*gstr(a1)) rc = fstat((int)a0, &st);
        else {
            bool follow = !(a3 & L_AT_SYMLINK_NOFOLLOW);
            if ((e = resolve_at((int)a0, gstr(a1), path, sizeof path, follow, &hfd))) { r = ERR(-e); break; }
            rc = fstatat(hfd, path, &st, follow ? 0 : AT_SYMLINK_NOFOLLOW);
        }
        if (rc == 0) put_stat(a2, &st);
        r = HOSTRET(rc);
        break;
    }
    case 80: { struct stat st; int rc = fstat((int)a0, &st); if (!rc) put_stat(a1, &st); r = HOSTRET(rc); break; }
    case 78: {   // readlinkat
        if (!strcmp(gstr(a1), "/proc/self/exe")) {
            size_t n = strlen(g_exe_path); if (n > a3) n = a3;
            memcpy(gptr(a2), g_exe_path, n); r = n; break;
        }
        if (!strncmp(gstr(a1), "/proc/self/fd/", 14)) {
            int fd = atoi(gstr(a1) + 14);
            const char *gp = (fd >= 0 && fd < MAX_FDS) ? fd_path[fd] : NULL;
            if (!gp) { r = ERR(2); break; }
            size_t n = strlen(gp); if (n > a3) n = a3;
            memcpy(gptr(a2), gp, n); r = n; break;
        }
        if ((e = resolve_at((int)a0, gstr(a1), path, sizeof path, false, &hfd))) { r = ERR(-e); break; }
        r = HOSTRET(readlinkat(hfd, path, gptr(a2), a3));
        break;
    }
    case 48: case 439: {   // faccessat, faccessat2
        if ((e = resolve_at((int)a0, gstr(a1), path, sizeof path, true, &hfd))) { r = ERR(-e); break; }
        r = HOSTRET(faccessat(hfd, path, (int)a2, 0));
        break;
    }
    case 61: LOCKED(r = sys_getdents64((int)a0, gptr(a1), a2)); break;
    case 17: {   // getcwd
        size_t n = strlen(g_cwd) + 1;
        if (n > a1) { r = ERR(34); break; }
        memcpy(gptr(a0), g_cwd, n); r = n; break;
    }
    case 49: {   // chdir
        const char *p = gstr(a0);
        if ((e = host_path(p, L_AT_FDCWD, path, sizeof path, true))) { r = ERR(-e); break; }
        struct stat st;
        if (stat(path, &st)) { r = ERR(lx_errno(errno)); break; }
        if (!S_ISDIR(st.st_mode)) { r = ERR(20); break; }
        snprintf(g_cwd, sizeof g_cwd, "%s", path + strlen(g_rootfs));
        if (!g_cwd[0]) strcpy(g_cwd, "/");
        r = 0; break;
    }
    case 25: {   // fcntl
        switch (a1) {
        case 0: r = HOSTRET(fcntl((int)a0, F_DUPFD, (int)a2)); fd_copy((int)a0, (int)r); break;
        case 1030: r = HOSTRET(fcntl((int)a0, F_DUPFD_CLOEXEC, (int)a2)); fd_copy((int)a0, (int)r); break;
        case 1: r = HOSTRET(fcntl((int)a0, F_GETFD)); break;
        case 2: r = HOSTRET(fcntl((int)a0, F_SETFD, (int)a2)); break;
        case 3: { int f = fcntl((int)a0, F_GETFL); r = f < 0 ? ERR(lx_errno(errno)) : lx_fl(f); break; }
        case 4: r = HOSTRET(fcntl((int)a0, F_SETFL, open_flags(a2) & (O_NONBLOCK | O_APPEND))); break;
        default: r = ERR(22);
        }
        break;
    }
    case 23: r = HOSTRET(dup((int)a0)); fd_copy((int)a0, (int)r); break;
    case 24: {   // dup3
        int fd = dup2((int)a0, (int)a1);
        if (fd >= 0) { fd_copy((int)a0, fd); dir_forget(fd); }
        if (fd >= 0 && (a2 & 02000000)) fcntl(fd, F_SETFD, FD_CLOEXEC);
        r = HOSTRET(fd); break;
    }
    case 29: {   // ioctl
        if (a1 == 0x5413) { struct winsize ws; int rc = ioctl((int)a0, TIOCGWINSZ, &ws); if (!rc) memcpy(gptr(a2), &ws, sizeof ws); r = HOSTRET(rc); }
        else if (a1 == 0x5421) { int on = *(int *)gptr(a2); r = HOSTRET(ioctl((int)a0, FIONBIO, &on)); }
        else if (a1 == 0x5451) r = HOSTRET(fcntl((int)a0, F_SETFD, FD_CLOEXEC));
        else r = ERR(25);   // ENOTTY: TCGETS and the rest
        break;
    }
    case 59: {   // pipe2
        int fds[2];
        int rc = pipe(fds);
        if (!rc) {
            for (int i = 0; i < 2; i++) {
                if (a1 & 02000000) fcntl(fds[i], F_SETFD, FD_CLOEXEC);
                if (a1 & 04000) fcntl(fds[i], F_SETFL, O_NONBLOCK);
            }
            memcpy(gptr(a0), fds, sizeof fds);
        }
        r = HOSTRET(rc); break;
    }
    case 222: r = sys_mmap(a0, a1, a2, a3, (int64_t)a4, a5); break;
    case 215: r = HOSTRET(munmap(gptr(a0), a1)); break;
    case 226: r = HOSTRET(mprotect(gptr(a0), a1, host_prot(a2))); break;
    case 233: r = sys_madvise(a0, a1, a2); break;
    case 216: r = sys_mremap(a0, a1, a2, a3, a4); break;
    case 214: r = 0; break;   // brk: no heap break, bionic uses mmap
    case 96: c->clear_child_tid = a0; r = c->tid; break;
    case 99: r = 0; break;    // set_robust_list
    case 134: {               // rt_sigaction
        if (a0 < 1 || a0 > 64) { r = ERR(22); break; }
        if (a2) memcpy(gptr(a2), g_sigactions[a0], 32);
        if (a1) { memcpy(g_sigactions[a0], gptr(a1), 32); update_host_action((int)a0); }
        r = 0; break;
    }
    case 135: {               // rt_sigprocmask
        uint64_t old = c->sigmask;
        if (a1) {
            uint64_t s = *(uint64_t *)gptr(a1);
            c->sigmask = a0 == 0 ? (old | s) : a0 == 1 ? (old & ~s) : s;
        }
        if (a2) *(uint64_t *)gptr(a2) = old;
        r = 0; break;
    }
    case 139: sigreturn(c); return;   // rt_sigreturn: registers come from the frame
    case 133: {                       // rt_sigsuspend
        uint64_t old = c->sigmask;
        c->sigmask = *(uint64_t *)gptr(a0);
        sigset_t all, prev, none;
        sigfillset(&all);
        sigemptyset(&none);
        sigprocmask(SIG_BLOCK, &all, &prev);
        while (!((g_pending | c->pending) & ~c->sigmask)) sigsuspend(&none);
        sigprocmask(SIG_SETMASK, &prev, NULL);
        c->x[0] = ERR(4);
        int sig = take_signal(c);
        if (sig && g_sigactions[sig][0] > 1) push_signal(c, sig, old);
        else c->sigmask = old;
        return;
    }
    case 132:                 // sigaltstack
        if (a1) { memset(gptr(a1), 0, 24); *(int32_t *)((char *)gptr(a1) + 8) = 2; }
        r = 0; break;
    case 167:                 // prctl
        switch (a0) {
        case 0x53564d41: case 4: case 15: case 38: case 1: r = 0; break;
        case 3: r = 1; break;
        case 16: strcpy(gptr(a1), "onirun"); r = 0; break;
        case 23: r = 0; break;
        default: r = ERR(22);
        }
        break;
    case 278: arc4random_buf(gptr(a0), a1); r = a1; break;   // getrandom
    case 160: {               // uname
        char *u = gptr(a0);
        memset(u, 0, 65 * 6);
        strcpy(u, "Linux"); strcpy(u + 65, "localhost"); strcpy(u + 130, "6.6.0-oni");
        strcpy(u + 195, "#1 SMP"); strcpy(u + 260, "aarch64"); strcpy(u + 325, "(none)");
        r = 0; break;
    }
    case 113: {               // clock_gettime
        int clk = lx_clock(a0);
        if (clk < 0) { r = ERR(22); break; }
        r = HOSTRET(clock_gettime(clk, gptr(a1))); break;
    }
    case 114: {
        int clk = lx_clock(a0);
        if (clk < 0) { r = ERR(22); break; }
        r = a1 ? HOSTRET(clock_getres(clk, gptr(a1))) : 0; break;
    }
    case 169: {               // gettimeofday: Linux timeval has a 64-bit tv_usec
        struct timeval tv;
        int rc = gettimeofday(&tv, NULL);
        if (!rc && a0) { ((int64_t *)gptr(a0))[0] = tv.tv_sec; ((int64_t *)gptr(a0))[1] = tv.tv_usec; }
        r = HOSTRET(rc); break;
    }
    case 101: r = HOSTRET(nanosleep(gptr(a0), gptr(a1))); break;
    case 115: {               // clock_nanosleep
        struct timespec *t = gptr(a2);
        if (a1 & 1) {         // TIMER_ABSTIME
            struct timespec now; clock_gettime(lx_clock(a0), &now);
            struct timespec d = { t->tv_sec - now.tv_sec, t->tv_nsec - now.tv_nsec };
            if (d.tv_nsec < 0) { d.tv_sec--; d.tv_nsec += 1000000000; }
            if (d.tv_sec >= 0) nanosleep(&d, NULL);
            r = 0;
        } else r = HOSTRET(nanosleep(t, gptr(a3)));
        break;
    }
    case 172: r = getpid(); break;
    case 173: r = getppid(); break;
    case 178: r = c->tid; break;
    case 174: case 175: case 176: case 177: r = 2000; break;   // the adb shell user
    case 158: r = 0; break;   // getgroups
    case 165: memset(gptr(a1), 0, 144); r = 0; break;   // getrusage
    case 163: case 261: {     // getrlimit / prlimit64
        uint64_t res = nr == 163 ? a0 : a1, out = nr == 163 ? a1 : a3;
        if (out) {
            uint64_t *rl = gptr(out);
            if (res == 3) { rl[0] = 8 << 20; rl[1] = ~0ull; }
            else if (res == 7) { rl[0] = 4096; rl[1] = 4096; }
            else { rl[0] = ~0ull; rl[1] = ~0ull; }
        }
        r = 0; break;
    }
    case 179: {               // sysinfo
        uint8_t *s = gptr(a0);
        memset(s, 0, 112);
        *(int64_t *)s = (int64_t)(clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000000);
        uint64_t mem = 0; size_t sl = sizeof mem; sysctlbyname("hw.memsize", &mem, &sl, NULL, 0);
        *(uint64_t *)(s + 32) = mem; *(uint64_t *)(s + 40) = mem / 2;
        *(uint16_t *)(s + 80) = 1; *(uint32_t *)(s + 104) = 1;
        r = 0; break;
    }
    case 124: sched_yield(); r = 0; break;
    case 119: r = 0; break;                                   // sched_setscheduler
    case 120: r = 0; break;                                   // sched_getscheduler: SCHED_OTHER
    case 121: *(int32_t *)gptr(a1) = 0; r = 0; break;         // sched_getparam
    case 125: r = (a0 == 1 || a0 == 2) ? 99 : 0; break;       // sched_get_priority_max
    case 126: r = (a0 == 1 || a0 == 2) ? 1 : 0; break;        // sched_get_priority_min
    case 140: r = 0; break;                                   // setpriority
    case 141: r = 20; break;                                  // getpriority (20 - nice)
    case 122: r = 0; break;   // sched_setaffinity
    case 123: {               // sched_getaffinity
        int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
        memset(gptr(a2), 0, a1);
        for (int i = 0; i < n && i < (int)a1 * 8; i++) ((uint8_t *)gptr(a2))[i / 8] |= 1 << (i % 8);
        r = 8; break;
    }
    case 98: r = sys_futex(a0, a1, a2, a3, a4, a5); break;
    case 93: {                        // exit: this thread only
        if (c->clear_child_tid) {
            __atomic_store_n((uint32_t *)gptr(c->clear_child_tid), 0, __ATOMIC_SEQ_CST);
            futex_wake(c->clear_child_tid, 0x7fffffff, true);
            futex_wake(c->clear_child_tid, 0x7fffffff, false);
        }
        bool last;
        pthread_mutex_lock(&g_lock);
        for (GuestCtx **pp = &g_threads; *pp; pp = &(*pp)->next_thread)
            if (*pp == c) { *pp = c->next_thread; break; }
        last = --g_nthreads == 0;
        pthread_mutex_unlock(&g_lock);
        if (last) { fflush(stdout); exit((int)(a0 & 0xff)); }
        pthread_exit(NULL);
    }
    case 94: fflush(stdout); exit((int)(a0 & 0xff));
    case 129: {                       // kill
        if (a1 == 6 && ((int64_t)a0 == getpid() || a0 == 0)) { sys_backtrace(c); die("onirun: guest called abort()"); }
        int hs = a1 ? host_sig(a1) : 0;
        if (a1 && !hs) { r = 0; break; }
        r = HOSTRET(kill((pid_t)a0, hs)); break;
    }
    case 130: case 131: {             // tkill / tgkill
        uint64_t tid = nr == 131 ? a1 : a0, sig = nr == 131 ? a2 : a1;
        if (sig == 6) { sys_backtrace(c); die("onirun: guest called abort()"); }
        GuestCtx *t = find_thread(tid);
        if (!t) { r = ERR(3); break; }
        if (!sig) { r = 0; break; }
        if (sig > 64) { r = ERR(22); break; }
        __atomic_or_fetch(&t->pending, 1ull << (sig - 1), __ATOMIC_SEQ_CST);
        // Interrupt a blocking host call in that thread; the handler itself only marks pending.
        int hs = host_sig(sig);
        if (t != c && hs && g_sigactions[sig][0] > 1) pthread_kill((pthread_t)t->host_thread, hs);
        r = 0; break;
    }
    case 220: {                       // clone
        uint64_t flags = a0;
        if (flags & 0x10000) {        // CLONE_THREAD: a guest thread on a new host thread
            GuestCtx *nc = malloc(sizeof *nc);
            memcpy(nc, c, sizeof *nc);
            nc->x[0] = 0;
            nc->sp = a1;
            nc->pending = 0;
            nc->clear_child_tid = (flags & 0x00200000) ? a4 : 0;
            if (flags & 0x00080000) nc->tpidr = a3;
            pthread_mutex_lock(&g_lock);
            nc->tid = g_next_tid++;
            nc->next_thread = g_threads;
            g_threads = nc;
            g_nthreads++;
            pthread_mutex_unlock(&g_lock);
            if (flags & 0x00100000) *(uint32_t *)gptr(a2) = (uint32_t)nc->tid;   // PARENT_SETTID
            if (flags & 0x01000000) *(uint32_t *)gptr(a4) = (uint32_t)nc->tid;   // CHILD_SETTID
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            pthread_attr_setstacksize(&at, 1 << 20);
            pthread_t th;
            int rc = pthread_create(&th, &at, guest_thread_main, nc);
            pthread_attr_destroy(&at);
            r = rc ? ERR(11) : nc->tid;
            break;
        }
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            tr_after_fork();
            c->tid = (uint64_t)getpid();
            g_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
            g_threads = c; c->next_thread = NULL; g_nthreads = 1;
            c->host_thread = (void *)pthread_self();
            // arm64 argument order (as bionic's __bionic_clone passes it): flags, newsp, parent_tid,
            // tls, child_tid
            if ((flags & 0x01000000) && a4) *(uint32_t *)gptr(a4) = (uint32_t)c->tid;   // CHILD_SETTID
            if (flags & 0x00200000) c->clear_child_tid = a4;                             // CHILD_CLEARTID
            if (flags & 0x00080000) c->tpidr = a3;                                       // SETTLS
            if (a1) c->sp = a1;
            r = 0;
        } else {
            if (pid > 0 && (flags & 0x00100000)) *(uint32_t *)gptr(a2) = (uint32_t)pid;   // PARENT_SETTID
            r = HOSTRET(pid);
        }
        break;
    }
    case 260: {                       // wait4
        int st = 0;
        pid_t p = wait4((pid_t)(int64_t)a0, &st, (int)(a2 & 3), NULL);
        if (p > 0 && a1) *(int32_t *)gptr(a1) = lx_wstatus(st);
        if (p >= 0 && a3) memset(gptr(a3), 0, 144);
        r = HOSTRET(p); break;
    }
    case 166: r = umask((mode_t)a0); break;
    case 155: case 156: r = getpid(); break;
    case 154: case 157: r = 0; break;
    case 43: case 44: {       // statfs / fstatfs
        uint64_t *s = gptr(a1);
        memset(s, 0, 120);
        s[0] = 0xEF53; s[1] = 4096; s[2] = 1 << 20; s[3] = 1 << 19; s[4] = 1 << 19;
        s[5] = 1 << 20; s[6] = 1 << 19; s[8] = 255; s[9] = 4096;
        r = 0; break;
    }
    case 46: r = HOSTRET(ftruncate((int)a0, (off_t)a1)); break;
    case 34: {               // mkdirat
        if ((e = resolve_at((int)a0, gstr(a1), path, sizeof path, true, &hfd))) { r = ERR(-e); break; }
        r = HOSTRET(mkdirat(hfd, path, (mode_t)a2)); break;
    }
    case 35: {               // unlinkat
        if ((e = resolve_at((int)a0, gstr(a1), path, sizeof path, false, &hfd))) { r = ERR(-e); break; }
        r = HOSTRET(unlinkat(hfd, path, (a2 & L_AT_REMOVEDIR) ? AT_REMOVEDIR : 0)); break;
    }
    case 53: case 52: case 54: case 55: case 88: r = 0; break;   // chmod/chown/utimens: accept
    case 73: {               // ppoll
        int timeout = -1;
        if (a2) { struct timespec *t = gptr(a2); timeout = (int)(t->tv_sec * 1000 + t->tv_nsec / 1000000); }
        r = HOSTRET(poll(gptr(a0), (nfds_t)a1, timeout)); break;
    }
    case 283: r = 0; break;         // membarrier
    case 293: case 435: case 291: case 198: r = ERR(38); break;
    case 240: {                     // rt_tgsigqueueinfo (abort() uses it)
        if (a2 == 6) { sys_backtrace(c); die("onirun: guest called abort()"); }
        r = a2 ? HOSTRET(kill(getpid(), host_sig(a2))) : 0; break;
    }
    case 281:                       // execveat(dirfd, path, argv, envp, flags)
        if (gstr(a1)[0] != '/') { r = ERR(38); break; }
        a0 = a1; a1 = a2; a2 = a3;
        /* fall through */
    case 221: {                     // execve
        const char *p = gstr(a0);
        char *argv[256], *envp[256];
        int n = 0;
        for (char **v = gptr(a1); v && *v && n < 255; v++) argv[n++] = *v;
        argv[n] = NULL;
        n = 0;
        for (char **v = gptr(a2); v && *v && n < 255; v++) envp[n++] = *v;
        envp[n] = NULL;
        ExecImage img;
        int rc = load_exec(p, argv, envp, &img);
        if (rc) { r = ERR(-rc); break; }
        memset(c->x, 0, sizeof c->x);
        memset(c->q, 0, sizeof c->q);
        c->pc = img.entry;
        c->sp = img.sp;
        c->tpidr = 0;
        c->nzcv = 0;
        return;
    }
    default:
        break;
    }
    if (r == ERR(38) && nr != 293 && nr != 435 && nr != 291 && nr != 198)
        fprintf(stderr, "onirun: unimplemented syscall %llu\n", (unsigned long long)nr);
    LOGV("[%d] syscall %llu(%llx, %llx, %llx) = %lld\n", getpid(), (unsigned long long)nr, (unsigned long long)a0,
         (unsigned long long)a1, (unsigned long long)a2, (long long)r);
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
        if (t != 31) c->x[t] = v;
        return 0;
    }
    if ((insn & 0xfff00000) == 0xd5100000) return 0;   // msr: ignore
    return -1;
}

void sys_init(const char *rootfs) {
    if (!realpath(rootfs, g_rootfs)) die("onirun: rootfs %s: %s", rootfs, strerror(errno));
    uint32_t *t = mmap(NULL, (size_t)getpagesize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    t[0] = 0xd2801168;   // mov x8, #139 (rt_sigreturn)
    t[1] = 0xd4000001;   // svc #0
    mprotect(t, (size_t)getpagesize(), PROT_READ);
    g_sigtramp = (uint64_t)(uintptr_t)t;
}
