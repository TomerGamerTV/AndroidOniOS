// /proc and /sys/fs/selinux. Static parts live in the rootfs (the SELinux class tree copied from a
// device); the rest is generated when opened. SELinux is reported as present but permissive, and
// every access query is answered "allowed", so Android's own permission checks pass.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include "sys.h"

// ---- generated files ----
static int serve_text(Proc *p, const char *text, size_t len, const char *path, int lflags) {
    int fd = host_tmpfile();
    if (fd < 0) return -errno;
    if (len) write(fd, text, len);
    lseek(fd, 0, SEEK_SET);
    fd_set_path(fd, path);
    return fd_install(p, fd, 0, lflags & 02000000);
}

static int maps_text(Proc *p, char **out) {
    Mm *m = p->mm;
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    pthread_mutex_lock(&m->lock);
    for (int i = 0; i < m->n; i++) {
        Vma *v = &m->v[i];
        if (n + 400 > cap) { cap *= 2; buf = realloc(buf, cap); }
        n += (size_t)snprintf(buf + n, cap - n, "%012llx-%012llx %c%c%c%c %08llx 00:00 0 %s%s\n",
                              (unsigned long long)v->start, (unsigned long long)v->end,
                              (v->prot & PROT_READ) ? 'r' : '-', (v->prot & PROT_WRITE) ? 'w' : '-',
                              (v->prot & PROT_EXEC) ? 'x' : '-', v->shared ? 's' : 'p',
                              (unsigned long long)v->off, v->path ? (v->path[0] == '[' ? "" : "") : "",
                              v->path ? v->path : "");
    }
    pthread_mutex_unlock(&m->lock);
    *out = buf;
    return (int)n;
}

// ---- directories: a temporary host directory with the right entry names ----
// Programs list /proc/self/fd (zygote), /proc/self/task (ART) and /proc (ps). The entries are
// dummies named right; the files behind them are served by proc_open when opened by full path.
typedef struct { char dir[256]; } ProcDir;
static const char *proc_target(Proc *p, const char *path, Proc **out);
static void rm_tree(const char *dir) {
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char path[600];
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            struct stat st;
            if (!lstat(path, &st) && S_ISDIR(st.st_mode)) rm_tree(path); else unlink(path);
        }
        closedir(d);
    }
    rmdir(dir);
}
static void procdir_release(FdObj *o) { ProcDir *pd = o->data; rm_tree(pd->dir); free(pd); }
static const FdOps procdir_ops = { "procdir", NULL, NULL, NULL, NULL, procdir_release };

static void touch(const char *dir, const char *name, bool isdir) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (isdir) mkdir(path, 0755);
    else close(open(path, O_WRONLY | O_CREAT, 0644));
}

// Returns a guest fd for a directory listing, 1 if path isn't a directory we know, or -errno.
static int proc_dir(Proc *p, const char *path, int lflags) {
    Proc *t = NULL;
    const char *rest = "";
    bool root = !strcmp(path, "/proc") || !strcmp(path, "/proc/");
    if (!root) {
        rest = proc_target(p, path, &t);
        if (!rest || !t) return 1;
        while (*rest == '/' && rest[1] == '/') rest++;
    }
    int kind;                          // what to list
    long tid = 0;
    if (root) kind = 0;
    else if (!*rest || !strcmp(rest, "/")) kind = 1;
    else if (!strcmp(rest, "/fd") || !strcmp(rest, "/fdinfo")) kind = 2;
    else if (!strcmp(rest, "/task")) kind = 3;
    else if (!strcmp(rest, "/attr")) kind = 4;
    else if (!strncmp(rest, "/task/", 6)) {
        char *end;
        tid = strtol(rest + 6, &end, 10);
        if (*end && strcmp(end, "/")) return 1;
        kind = 5;
    } else return 1;
    const char *tmp = getenv("TMPDIR");
    ProcDir *pd = calloc(1, sizeof *pd);
    snprintf(pd->dir, sizeof pd->dir, "%s/onirun-proc.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(pd->dir)) { free(pd); return -errno; }
    char name[64];
    switch (kind) {
    case 0:
        for (const char *f = "self\0thread-self\0filesystems\0cpuinfo\0meminfo\0stat\0mounts\0version\0uptime\0loadavg\0"; *f; f += strlen(f) + 1) touch(pd->dir, f, false);
        touch(pd->dir, "sys", true);
        pthread_mutex_lock(&g_lock);
        for (Proc *q = proc_list(); q; q = q->next) { snprintf(name, sizeof name, "%d", q->pid); touch(pd->dir, name, true); }
        pthread_mutex_unlock(&g_lock);
        break;
    case 1:
        for (const char *f = "cmdline\0comm\0stat\0status\0maps\0smaps\0mounts\0mountinfo\0oom_score_adj\0oom_adj\0timerslack_ns\0"; *f; f += strlen(f) + 1) touch(pd->dir, f, false);
        touch(pd->dir, "fd", true); touch(pd->dir, "fdinfo", true); touch(pd->dir, "task", true); touch(pd->dir, "attr", true);
        { char lp[600]; snprintf(lp, sizeof lp, "%s/exe", pd->dir); symlink(t->exe, lp); snprintf(lp, sizeof lp, "%s/cwd", pd->dir); symlink(t->cwd, lp); }
        break;
    case 2:
        for (int i = 0; i < MAX_FDS; i++) {              // racy by design: a snapshot, like procfs
            int h = t->fd[i].host;
            if (h < 0) continue;
            char lp[600], target[1100];
            snprintf(name, sizeof name, "%d", i);
            snprintf(lp, sizeof lp, "%s/%s", pd->dir, name);
            if (!fd_get_path(h, target, sizeof target)) snprintf(target, sizeof target, "anon_inode:[unknown]");
            symlink(target, lp);
        }
        break;
    case 3:
        pthread_mutex_lock(&g_lock);
        for (GuestCtx *x = t->threads; x; x = x->next_thread) { snprintf(name, sizeof name, "%llu", (unsigned long long)x->tid); touch(pd->dir, name, true); }
        pthread_mutex_unlock(&g_lock);
        break;
    case 4:
        for (const char *f = "current\0exec\0fscreate\0keycreate\0prev\0sockcreate\0"; *f; f += strlen(f) + 1) touch(pd->dir, f, false);
        break;
    case 5:
        for (const char *f = "stat\0status\0comm\0cmdline\0maps\0"; *f; f += strlen(f) + 1) touch(pd->dir, f, false);
        (void)tid;
        break;
    }
    int fd = open(pd->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { rm_tree(pd->dir); free(pd); return -errno; }
    FdObj *o = obj_new(&procdir_ops, pd, false);
    fd_set_path(fd, path);
    return fd_install_obj(p, fd, o, lflags & 02000000);
}

// Resolves "/proc/self" and "/proc/<pid>" to a process; returns the rest of the path.
static const char *proc_target(Proc *p, const char *path, Proc **out) {
    const char *rest = path + 6;
    *out = NULL;
    if (!strncmp(rest, "self/", 5) || !strcmp(rest, "self")) { *out = p; return rest + 4; }
    if (!strncmp(rest, "thread-self/", 12)) { *out = p; return rest + 11; }
    char *end;
    long pid = strtol(rest, &end, 10);
    if (end != rest && (*end == '/' || !*end)) {
        LOCKED(*out = proc_find((int)pid));
        return end;
    }
    return NULL;
}

static int proc_open(Proc *p, const char *path, int lflags) {
    char buf[4096];
    int n = -1;
    if (!strcmp(path, "/proc/filesystems")) n = snprintf(buf, sizeof buf, "nodev\tsysfs\nnodev\ttmpfs\nnodev\tproc\nnodev\tselinuxfs\nnodev\tdevpts\n\text4\n\tfuse\n");
    else if (!strcmp(path, "/proc/version")) n = snprintf(buf, sizeof buf, "Linux version 6.6.0-oni (onirun) #1 SMP PREEMPT\n");
    else if (!strcmp(path, "/proc/sys/kernel/pid_max")) n = snprintf(buf, sizeof buf, "32768\n");
    else if (!strcmp(path, "/proc/sys/kernel/random/boot_id")) n = snprintf(buf, sizeof buf, "0f3a6d9e-7c1b-4b2a-9d6e-0a1b2c3d4e5f\n");
    else if (!strcmp(path, "/proc/sys/vm/overcommit_memory")) n = snprintf(buf, sizeof buf, "0\n");
    else if (!strcmp(path, "/proc/sys/vm/mmap_min_addr")) n = snprintf(buf, sizeof buf, "32768\n");
    else if (!strcmp(path, "/proc/sys/fs/pipe-max-size")) n = snprintf(buf, sizeof buf, "1048576\n");
    else if (!strcmp(path, "/proc/uptime")) {
        double up = clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1e9;
        n = snprintf(buf, sizeof buf, "%.2f %.2f\n", up, up * 2);
    } else if (!strcmp(path, "/proc/loadavg")) n = snprintf(buf, sizeof buf, "0.10 0.10 0.10 1/100 200\n");
    else if (!strcmp(path, "/proc/mounts")) return proc_open(p, "/proc/self/mounts", lflags);
    else if (!strcmp(path, "/proc/stat")) {
        int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
        n = snprintf(buf, sizeof buf, "cpu  100 0 100 10000 0 0 0 0 0 0\n");
        for (int i = 0; i < ncpu && n < (int)sizeof buf - 80; i++) n += snprintf(buf + n, sizeof buf - (size_t)n, "cpu%d 10 0 10 1000 0 0 0 0 0 0\n", i);
        n += snprintf(buf + n, sizeof buf - (size_t)n, "ctxt 1000\nbtime 1700000000\nprocesses 100\nprocs_running 1\nprocs_blocked 0\n");
    }
    else if (!strcmp(path, "/proc/cpuinfo")) {
        int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
        n = 0;
        for (int i = 0; i < ncpu && n < (int)sizeof buf - 400; i++)
            n += snprintf(buf + n, sizeof buf - (size_t)n,
                          "processor\t: %d\nBogoMIPS\t: 48.00\nFeatures\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc dcpop sha3 asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb paca pacg dcpodp flagm2 frint\nCPU implementer\t: 0x61\nCPU architecture: 8\nCPU variant\t: 0x0\nCPU part\t: 0x000\nCPU revision\t: 0\n\n", i);
    } else if (!strcmp(path, "/proc/meminfo")) {
        uint64_t mem = 0;
        size_t sl = sizeof mem;
        sysctlbyname("hw.memsize", &mem, &sl, NULL, 0);
        n = snprintf(buf, sizeof buf, "MemTotal:       %llu kB\nMemFree:        %llu kB\nMemAvailable:   %llu kB\nBuffers:               0 kB\nCached:                0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n",
                     (unsigned long long)(mem / 1024), (unsigned long long)(mem / 2048), (unsigned long long)(mem / 2048));
    } else {
        Proc *t;
        const char *rest = proc_target(p, path, &t);
        if (!rest) return -ENOENT;
        if (!t) return -ENOENT;
        GuestCtx *th = NULL;
        if (!strncmp(rest, "/task/", 6)) {               // /proc/<pid>/task/<tid>/...
            char *end;
            long tid = strtol(rest + 6, &end, 10);
            pthread_mutex_lock(&g_lock);
            for (GuestCtx *x = t->threads; x; x = x->next_thread) if ((long)x->tid == tid) th = x;
            pthread_mutex_unlock(&g_lock);
            if (!th || *end != '/') return -ENOENT;
            rest = end;
        }
        const char *tname = th && th->name[0] ? th->name : (strrchr(t->exe, '/') ? strrchr(t->exe, '/') + 1 : t->exe);
        int tid = th ? (int)th->tid : t->pid;
        if (!strcmp(rest, "/attr/current")) n = snprintf(buf, sizeof buf, "%s", proc_secontext(t));
        else if (!strcmp(rest, "/cmdline")) { memcpy(buf, t->cmdline, t->cmdline_len); n = (int)t->cmdline_len; }
        else if (!strcmp(rest, "/comm")) n = snprintf(buf, sizeof buf, "%.15s\n", tname);
        else if (!strcmp(rest, "/exe")) return 1;        // resolved by readlink/stat elsewhere
        else if (!strcmp(rest, "/status")) {
            const char *b = strrchr(t->exe, '/');
            n = snprintf(buf, sizeof buf, "Name:\t%.15s\nUmask:\t%04o\nState:\tR (running)\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\nUid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nThreads:\t%d\n",
                         b ? b + 1 : t->exe, t->umask, t->pid, t->pid, t->ppid, t->uid, t->uid, t->uid, t->uid, t->gid, t->gid, t->gid, t->gid, t->nthreads);
        } else if (!strcmp(rest, "/stat")) {
            // ... num_threads itrealvalue starttime vsize rss rsslim startcode endcode startstack ...
            n = snprintf(buf, sizeof buf, "%d (%.15s) R %d %d %d 0 -1 4194304 0 0 0 0 0 0 0 0 20 0 %d 0 0 %llu 0 18446744073709551615 0 0 %llu 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                         tid, tname, t->ppid, t->pgid, t->sid, t->nthreads, (unsigned long long)WIN_SIZE,
                         (unsigned long long)t->stack_start);
        } else if (!strcmp(rest, "/oom_score_adj") || !strcmp(rest, "/oom_adj")) n = snprintf(buf, sizeof buf, "0\n");
        else if (!strcmp(rest, "/timerslack_ns")) n = snprintf(buf, sizeof buf, "50000\n");
        else if (!strcmp(rest, "/maps") || !strcmp(rest, "/smaps")) {
            char *m;
            int len = maps_text(t, &m);
            int r = serve_text(p, m, (size_t)len, path, lflags);
            free(m);
            return r;
        } else if (!strcmp(rest, "/mountinfo") || !strcmp(rest, "/mounts")) {
            if (!strcmp(rest, "/mounts"))
                n = snprintf(buf, sizeof buf, "rootfs / rootfs rw 0 0\nproc /proc proc rw 0 0\nsysfs /sys sysfs rw 0 0\nselinuxfs /sys/fs/selinux selinuxfs rw 0 0\ntmpfs /dev tmpfs rw 0 0\n/dev/block/dm-0 /data ext4 rw 0 0\n");
            else
                n = snprintf(buf, sizeof buf, "1 0 0:1 / / rw - rootfs rootfs rw\n2 1 0:2 / /proc rw - proc proc rw\n3 1 0:3 / /sys rw - sysfs sysfs rw\n6 3 0:6 / /sys/fs/selinux rw - selinuxfs selinuxfs rw\n4 1 0:4 / /dev rw - tmpfs tmpfs rw\n5 1 0:5 / /data rw - ext4 /dev/block/dm-0 rw\n");
        } else return -ENOENT;
    }
    if (n < 0) return -ENOENT;
    return serve_text(p, buf, (size_t)n, path, lflags);
}

// ---- /sys/fs/selinux/access, create, context: the policy says yes to everything ----
typedef struct { char query[1024]; bool answered; } SeQuery;
static int64_t se_write(FdObj *o, GuestCtx *c, const void *buf, size_t n, bool nonblock) {
    (void)c; (void)nonblock;
    SeQuery *q = o->data;
    size_t k = n < sizeof q->query - 1 ? n : sizeof q->query - 1;
    memcpy(q->query, buf, k);
    q->query[k] = 0;
    q->answered = false;
    return (int64_t)n;
}
static int64_t se_access_read(FdObj *o, GuestCtx *c, void *buf, size_t n, bool nonblock) {
    (void)c; (void)nonblock;
    SeQuery *q = o->data;
    if (q->answered) return 0;
    q->answered = true;
    const char *ans = "ffffffff ffffffff 0 0 1 0";      // allowed, decided, auditallow, auditdeny, seqno, flags
    size_t k = strlen(ans);
    if (k > n) k = n;
    memcpy(buf, ans, k);
    return (int64_t)k;
}
static int64_t se_create_read(FdObj *o, GuestCtx *c, void *buf, size_t n, bool nonblock) {
    (void)c; (void)nonblock;
    SeQuery *q = o->data;
    if (q->answered) return 0;
    q->answered = true;
    // "scon tcon class [name]": the new object gets the target's label.
    char scon[256] = "", tcon[256] = "";
    sscanf(q->query, "%255s %255s", scon, tcon);
    const char *ans = tcon[0] ? tcon : scon;
    size_t k = strlen(ans) + 1;
    if (k > n) k = n;
    memcpy(buf, ans, k);
    return (int64_t)k;
}
static void se_release(FdObj *o) { free(o->data); }
static const FdOps se_access_ops = { "selinux-access", se_access_read, se_write, NULL, NULL, se_release };
static const FdOps se_create_ops = { "selinux-create", se_create_read, se_write, NULL, NULL, se_release };
static const FdOps se_context_ops = { "selinux-context", NULL, se_write, NULL, NULL, se_release };

static int se_open(Proc *p, const FdOps *ops, const char *path, int lflags) {
    FdObj *o = obj_new(ops, calloc(1, sizeof(SeQuery)), true);
    if (!o) return -ENFILE;
    fd_set_path(o->rd, path);
    return fd_install_obj(p, o->rd, o, lflags & 02000000);
}

// Writes to the ftrace marker and other sinks are accepted and dropped.
static int64_t sink_write(FdObj *o, GuestCtx *c, const void *buf, size_t n, bool nonblock) { (void)o; (void)c; (void)buf; (void)nonblock; return (int64_t)n; }
static const FdOps sink_ops = { "sink", NULL, sink_write, NULL, NULL, NULL };

int procfs_open(Proc *p, const char *path, int lflags) {
    if (!strncmp(path, "/proc", 5) && (path[5] == 0 || path[5] == '/')) {
        int d = proc_dir(p, path, lflags);
        if (d != 1) return d;
        if (path[5] == 0) return -ENOENT;
        return proc_open(p, path, lflags);
    }
    if (!strcmp(path, "/sys/kernel/tracing/trace_marker") || !strcmp(path, "/sys/kernel/debug/tracing/trace_marker")) {
        FdObj *o = obj_new(&sink_ops, NULL, true);
        if (!o) return -ENFILE;
        fd_set_path(o->rd, path);
        return fd_install_obj(p, o->rd, o, lflags & 02000000);
    }
    const char *f = path + 16;
    if (!strcmp(f, "access")) return se_open(p, &se_access_ops, path, lflags);
    if (!strcmp(f, "create") || !strcmp(f, "member") || !strcmp(f, "relabel")) return se_open(p, &se_create_ops, path, lflags);
    if (!strcmp(f, "context") || !strcmp(f, "load") || (!strcmp(f, "enforce") && (lflags & 3))) return se_open(p, &se_context_ops, path, lflags);
    return 1;                                            // status, enforce, class/...: files in the rootfs
}
