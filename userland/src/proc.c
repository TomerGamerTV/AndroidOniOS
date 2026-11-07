// Processes, threads, file descriptors and signals. Every guest process and thread lives inside
// this one host process: guest threads are host pthreads; a guest process is an address-space
// window plus its own pid, fd table, signal actions and working directory. Nothing here calls the
// host's fork(), which iOS doesn't have.
#include <errno.h>
#include <fcntl.h>
#include <os/os_sync_wait_on_address.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <dispatch/dispatch.h>

#include "sys.h"

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
extern volatile uint64_t g_barrier_gen;   // translate.c
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;    // process and thread state changes
static pthread_cond_t hk_cond = PTHREAD_COND_INITIALIZER;   // wakes the housekeeping thread
static Proc *g_procs;
static int g_next_id = 100;                                  // pids and tids share one space

void post_signal_locked(Proc *p, GuestCtx *t, int sig);

#define ALTSTACK_SIZE (64 << 10)
#define L_CLONE_VM 0x100
#define L_CLONE_FILES 0x400
#define L_CLONE_VFORK 0x4000
#define L_CLONE_THREAD 0x10000
#define L_CLONE_SETTLS 0x80000
#define L_CLONE_PARENT_SETTID 0x100000
#define L_CLONE_CHILD_CLEARTID 0x200000
#define L_CLONE_CHILD_SETTID 0x1000000
#define L_SA_NOCLDWAIT 2
#define L_SA_RESTORER 0x04000000
#define L_SA_NODEFER 0x40000000
#define L_SA_RESETHAND 0x80000000u
#define UNBLOCKABLE ((1ull << 8) | (1ull << 18))   // SIGKILL, SIGSTOP

// ---- host fd metadata (indexed by host fd) ----
#define HOST_FDS 16384
// Guest fd-table entries share host fds the way Linux processes share open files after fork:
// refs counts the entries naming a host fd, which is closed when the last one goes. (Copying them
// with dup() made every app process hold its own copy of everything zygote had open, and Android
// ran into the host's per-process fd limit.)
typedef struct HostFd { char *path; FdObj *obj; int refs; } HostFd;   // guest path (for /proc/self/fd/N), object
static HostFd hfd[HOST_FDS];
uint32_t g_uid = 2000;

static void set_path_locked(int host, const char *p) {
    if (host < 0 || host >= HOST_FDS) return;
    free(hfd[host].path);
    hfd[host].path = p ? strdup(p) : NULL;
}
void fd_set_path(int host, const char *p) { LOCKED(set_path_locked(host, p)); }
bool fd_get_path(int host, char *out, size_t n) {
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    if (host >= 0 && host < HOST_FDS && hfd[host].path) { snprintf(out, n, "%s", hfd[host].path); ok = true; }
    pthread_mutex_unlock(&g_lock);
    return ok;
}
FdObj *fd_obj(int host) { return host >= 0 && host < HOST_FDS ? hfd[host].obj : NULL; }
static void attach_locked(int host, FdObj *o) {
    if (host < 0 || host >= HOST_FDS) { obj_put_deferred(o); return; }
    if (hfd[host].obj) obj_put_deferred(hfd[host].obj);
    hfd[host].obj = o;
}
void fd_attach_obj(int host, FdObj *o) { obj_get(o); LOCKED(attach_locked(host, o)); }

// A dup of host fd `from` as `to`: same path and object.
static void copy_meta_locked(int from, int to) {
    if (from < 0 || from >= HOST_FDS || to < 0 || to >= HOST_FDS) return;
    set_path_locked(to, hfd[from].path);
    if (hfd[from].obj) obj_get(hfd[from].obj);
    attach_locked(to, hfd[from].obj);
}

static void host_ref_locked(int h) { if (h >= 0 && h < HOST_FDS) hfd[h].refs++; }
int fd_host_refs(int h) { int r; LOCKED(r = h >= 0 && h < HOST_FDS ? hfd[h].refs : 0); return r; }
static void host_close_locked(int h) {
    if (h < 0) return;
    if (h < HOST_FDS && hfd[h].refs > 1) { hfd[h].refs--; return; }   // still in another table
    if (h < HOST_FDS) hfd[h].refs = 0;
    dir_forget(h);
    set_path_locked(h, NULL);
    if (h < HOST_FDS && hfd[h].obj) { FdObj *o = hfd[h].obj; hfd[h].obj = NULL; obj_put_deferred(o); }   // g_lock is held
    close(h);
}

static int fd_host_locked(Proc *p, int g) { return (g >= 0 && g < MAX_FDS) ? p->fd[g].host : -1; }
int fd_host(Proc *p, int g) { int h; LOCKED(h = fd_host_locked(p, g)); return h; }

// ONIRUN_FDTRACE=<exe substring>: log every guest fd install and close in matching processes.
static int fdtrace_on(Proc *p) {
    static const char *want = (const char *)1;
    if (want == (const char *)1) want = getenv("ONIRUN_FDTRACE");
    if (!want || !*want) return 0;
    if (atoi(want) == p->pid) return 1;                      // a pid, or a thread name (system_server names its main thread)
    for (GuestCtx *t = p->threads; t; t = t->next_thread) if (!strcmp(t->name, want)) return 1;
    return 0;
}
static void fdtrace(Proc *p, const char *what, int g, int host) {
    if (!fdtrace_on(p)) return;
    const char *path = host >= 0 && host < HOST_FDS && hfd[host].path ? hfd[host].path : "";
    fprintf(stderr, "FDT %d/%llu(%s) %s %d host %d %s\n", p->pid, (unsigned long long)(tr_cur ? tr_cur->tid : 0), tr_cur ? tr_cur->name : "-", what, g, host, path);
}
static int fd_install_locked(Proc *p, int host, int min, bool cloexec) {
    for (int g = min < 0 ? 0 : min; g < MAX_FDS; g++)
        if (p->fd[g].host < 0) { p->fd[g] = (FdEnt){ host, cloexec, 0, 0, 0 }; host_ref_locked(host); fdtrace(p, "open", g, host); return g; }
    fprintf(stderr, "onirun: pid %d: guest fd table full\n", p->pid);
    host_close_locked(host);
    return -EMFILE;
}
int fd_install(Proc *p, int host, int min, bool cloexec) {
    int g;
    LOCKED(g = fd_install_locked(p, host, min, cloexec));
    return g;
}

int fd_install_obj(Proc *p, int host, FdObj *o, bool cloexec) {
    int g;
    pthread_mutex_lock(&g_lock);
    attach_locked(host, o);
    g = fd_install_locked(p, host, 0, cloexec);
    pthread_mutex_unlock(&g_lock);
    return g;
}

static void note_closer(Proc *p, int g, char how) {
    p->fd[g].closed_by = how;
    p->fd[g].closer_pid = tr_cur ? tr_cur->proc->pid : 0;
    p->fd[g].closer_tid = tr_cur ? (int)tr_cur->tid : 0;
}
// Closing a guest fd from onirun itself (not the guest's close()) is marked so a later
// double close can say who freed the slot.
int fd_close_internal(Proc *p, int g, char how) {
    vfs_fd_closed(p, g);
    pthread_mutex_lock(&g_lock);
    int h = fd_host_locked(p, g);
    if (h >= 0) { fdtrace(p, "close", g, h); p->fd[g].host = -1; note_closer(p, g, how); host_close_locked(h); }
    pthread_mutex_unlock(&g_lock);
    return h < 0 ? -EBADF : 0;
}
int fd_close(Proc *p, int g) {
    vfs_fd_closed(p, g);
    pthread_mutex_lock(&g_lock);
    int h = fd_host_locked(p, g);
    if (h >= 0) { fdtrace(p, "close", g, h); p->fd[g].host = -1; note_closer(p, g, 'c'); host_close_locked(h); }
    else if (g >= 0 && g < MAX_FDS && p->fd[g].closed_by && getenv("ONIRUN_FDTRACE"))   // close-all loops make many; opt-in
        fprintf(stderr, "onirun: pid %d tid %llu: close(%d) of a closed fd; last freed by '%c' from pid %d tid %d\n", p->pid,
                (unsigned long long)(tr_cur ? tr_cur->tid : 0), g, p->fd[g].closed_by, p->fd[g].closer_pid, p->fd[g].closer_tid);
    pthread_mutex_unlock(&g_lock);
    return h < 0 ? -EBADF : 0;
}

// dup/F_DUPFD (lowest free fd >= target) or dup3 (exactly target).
int fd_dup(Proc *p, int old, int target, bool exact, bool cloexec) {
    pthread_mutex_lock(&g_lock);
    int r, h = fd_host_locked(p, old);
    if (h < 0 || (exact && (target < 0 || target >= MAX_FDS))) { r = -EBADF; goto out; }
    if (target < 0 || target >= MAX_FDS) { r = -EINVAL; goto out; }
    int nh = dup(h);
    if (nh < 0) { r = -lx_errno(errno); goto out; }
    copy_meta_locked(h, nh);
    if (exact) {
        if (p->fd[target].host >= 0) { fdtrace(p, "dup-over", target, p->fd[target].host); note_closer(p, target, 'd'); host_close_locked(p->fd[target].host); }
        p->fd[target] = (FdEnt){ nh, cloexec, 0, 0, 0 };
        host_ref_locked(nh);
        fdtrace(p, "dup-to", target, nh);
        r = target;
    } else {
        r = fd_install_locked(p, nh, target, cloexec);
    }
out:
    pthread_mutex_unlock(&g_lock);
    return r;
}

int fd_cloexec(Proc *p, int g, int set) {   // set < 0: query
    int r;
    pthread_mutex_lock(&g_lock);
    if (fd_host_locked(p, g) < 0) r = -EBADF;
    else if (set < 0) r = p->fd[g].cloexec ? 1 : 0;
    else { p->fd[g].cloexec = set; r = 0; }
    pthread_mutex_unlock(&g_lock);
    return r;
}

// ---- process table ----
Proc *proc_list(void) { return g_procs; }
Proc *proc_find(int pid) {
    for (Proc *q = g_procs; q; q = q->next) if (q->pid == pid) return q;
    return NULL;
}

// The SELinux domain a process would have on a real device, by program name.
const char *proc_secontext(Proc *p) {
    const char *b = strrchr(p->exe, '/');
    b = b ? b + 1 : p->exe;
    if (!strcmp(b, "servicemanager")) return "u:r:servicemanager:s0";
    if (!strcmp(b, "hwservicemanager")) return "u:r:hwservicemanager:s0";
    if (!strcmp(b, "system_server")) return "u:r:system_server:s0";
    if (!strcmp(b, "surfaceflinger")) return "u:r:surfaceflinger:s0";
    if (!strcmp(b, "app_process64") || !strcmp(b, "zygote64")) return "u:r:zygote:s0";
    return "u:r:shell:s0";
}

static void set_cmdline(Proc *p, char **argv) {
    p->cmdline_len = 0;
    for (int i = 0; argv && argv[i]; i++) {
        size_t n = strlen(argv[i]) + 1;
        if (p->cmdline_len + n > sizeof p->cmdline) break;
        memcpy(p->cmdline + p->cmdline_len, argv[i], n);
        p->cmdline_len += n;
    }
}

static Proc *proc_alloc_locked(void) {
    Proc *p = calloc(1, sizeof *p);
    for (int i = 0; i < MAX_FDS; i++) p->fd[i].host = -1;
    p->pid = p->pgid = p->sid = g_next_id++;
    p->uid = p->gid = p->euid = p->egid = 0;      // init's children start as root and drop privileges themselves
    memset(p->caps, 0xff, sizeof p->caps);        // ... with every capability until they drop some
    strcpy(p->cwd, "/");
    p->umask = 022;
    p->exit_signal = 17;
    p->next = g_procs;
    g_procs = p;
    return p;
}

static void proc_free_locked(Proc *p) {
    for (Proc **pp = &g_procs; *pp; pp = &(*pp)->next)
        if (*pp == p) { *pp = p->next; break; }
    free(p);
}

static GuestCtx *find_thread_locked(int64_t tid) {
    for (Proc *p = g_procs; p; p = p->next)
        for (GuestCtx *t = p->threads; t; t = t->next_thread)
            if ((int64_t)t->tid == tid) return t;
    return NULL;
}

// Interrupts a thread's blocking host call; see kick_handler in translate.c.
static void kick(GuestCtx *t) {
    if (t->host_thread && t != tr_cur) pthread_kill((pthread_t)t->host_thread, SIGUSR1);
}

// Marks every thread of p to leave; the last one out finishes the process.
static void kill_proc_locked(Proc *p, int wstatus) {
    if (p->state != P_RUNNING) return;
    p->state = P_EXITING;
    p->wstatus = wstatus;
    for (GuestCtx *t = p->threads; t; t = t->next_thread) { t->kill = 1; kick(t); }
    pthread_cond_broadcast(&g_cond);
    pthread_cond_signal(&hk_cond);
}

// ---- threads ----
static void *thread_main(void *arg) {
    GuestCtx *c = arg;
    stack_t ss = { .ss_sp = c->altstack, .ss_size = ALTSTACK_SIZE };
    sigaltstack(&ss, NULL);
    tr_run(c);
}

static int thread_spawn_locked(GuestCtx *c) {
    c->magic = CTX_MAGIC;
    c->altstack = malloc(ALTSTACK_SIZE);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 1 << 20);
    pthread_t th;
    int rc = pthread_create(&th, &at, thread_main, c);
    pthread_attr_destroy(&at);
    if (!rc) c->host_thread = (void *)th;
    return rc;
}

static void wake_tid_word(GuestCtx *c, uint64_t addr) {
    uint32_t *w = (uint32_t *)(uintptr_t)(c->base + UNTAG(addr));
    __atomic_store_n(w, 0, __ATOMIC_SEQ_CST);
    os_sync_wake_by_address_all(w, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    os_sync_wake_by_address_all(w, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

static void finish_proc_locked(Proc *p);

// ---- pidfds: an fd per process that becomes readable when it exits ----
typedef struct PidFd { int pid; FdObj *o; struct PidFd *next; } PidFd;
static PidFd *pidfds;                     // g_lock
static void pidfd_release(FdObj *o) {
    pthread_mutex_lock(&g_lock);
    for (PidFd **pp = &pidfds; *pp; pp = &(*pp)->next)
        if ((*pp)->o == o) { PidFd *x = *pp; *pp = x->next; free(x); break; }
    pthread_mutex_unlock(&g_lock);
}
static const FdOps pidfd_ops = { "pidfd", NULL, NULL, NULL, NULL, pidfd_release };
static void pidfd_exited_locked(int pid) {
    for (PidFd *x = pidfds; x; x = x->next)
        if (x->pid == pid) { pthread_mutex_lock(&x->o->lock); obj_set_ready(x->o, true); pthread_mutex_unlock(&x->o->lock); }
}
Proc *proc_find(int pid);
uint64_t proc_pidfd_open(GuestCtx *c, int64_t pid, int flags) {
    pthread_mutex_lock(&g_lock);
    Proc *q = proc_find((int)pid);
    if (!q) { pthread_mutex_unlock(&g_lock); return ERR(3); }
    bool dead = q->state == P_ZOMBIE;
    PidFd *x = calloc(1, sizeof *x);
    x->pid = (int)pid;
    x->o = obj_new(&pidfd_ops, (void *)(intptr_t)pid, true);
    x->next = pidfds;
    pidfds = x;
    if (dead) { pthread_mutex_lock(&x->o->lock); obj_set_ready(x->o, true); pthread_mutex_unlock(&x->o->lock); }
    pthread_mutex_unlock(&g_lock);
    fd_set_path(x->o->rd, "anon_inode:[pidfd]");
    int g = fd_install_obj(c->proc, x->o->rd, x->o, true);
    (void)flags;
    return g < 0 ? ERR(-g) : (uint64_t)g;
}
int proc_pidfd_pid(Proc *p, int gfd) {
    int h = fd_host(p, gfd);
    FdObj *o = h >= 0 ? fd_obj(h) : NULL;
    return o && o->ops == &pidfd_ops ? (int)(intptr_t)o->data : -1;
}

// The calling thread leaves its process; the last thread out finishes the process.
__attribute__((noreturn)) static void thread_leave(GuestCtx *c) {
    Proc *p = c->proc;
    pthread_mutex_lock(&g_lock);
    for (GuestCtx **pp = &p->threads; *pp; pp = &(*pp)->next_thread)
        if (*pp == c) { *pp = c->next_thread; break; }
    if (--p->nthreads == 0) finish_proc_locked(p);
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    tr_leave();
    stack_t ss = { .ss_flags = SS_DISABLE };
    sigaltstack(&ss, NULL);
    if (c->magic != CTX_MAGIC || (c->host_thread && (pthread_t)c->host_thread != pthread_self())) {
        fprintf(stderr, "onirun: thread_leave: bad context %p (magic %llx, tid %llu, owner %p, self %p); not freeing it\n",
                (void *)c, (unsigned long long)c->magic, (unsigned long long)c->tid, c->host_thread, (void *)pthread_self());
        pthread_exit(NULL);
    }
    c->magic = 0;
    free(c->altstack);
    c->altstack = NULL;
    free(c);
    pthread_exit(NULL);
}

void proc_exit_thread(GuestCtx *c, int status) {
    if (c->clear_child_tid && !c->kill) wake_tid_word(c, c->clear_child_tid);
    pthread_mutex_lock(&g_lock);
    Proc *p = c->proc;
    if (p->nthreads == 1 && p->state == P_RUNNING) { p->state = P_EXITING; p->wstatus = (status & 0xff) << 8; }
    pthread_mutex_unlock(&g_lock);
    thread_leave(c);
}

void proc_exit_group(GuestCtx *c, int status) {
    LOCKED(kill_proc_locked(c->proc, (status & 0xff) << 8));
    thread_leave(c);
}

// ---- process exit ----
static void finish_proc_locked(Proc *p) {
    if (p->shadow_of) {                    // a CLONE_FILES-less thread's private table: close it, share nothing else
        for (int i = 0; i < MAX_FDS; i++)
            if (p->fd[i].host >= 0) { host_close_locked(p->fd[i].host); p->fd[i].host = -1; }
        for (Proc *q = g_procs, *n; q; q = n) {     // its children (crash_dump's helpers) lose their parent
            n = q->next;
            if (q->parent == p) { q->parent = NULL; q->ppid = 1; if (q->state == P_ZOMBIE) proc_free_locked(q); }
        }
        mm_put(p->mm);
        free(p);
        return;
    }
    if (p->state == P_RUNNING) { p->state = P_EXITING; p->wstatus = 0; }
    for (int i = 0; i < MAX_FDS; i++)
        if (p->fd[i].host >= 0) { host_close_locked(p->fd[i].host); p->fd[i].host = -1; }
    Mm *mm = p->mm;
    p->mm = NULL;
    p->vfork_done = true;
    // Orphans lose their parent; orphaned zombies are freed.
    for (Proc *q = g_procs; q;) {
        Proc *n = q->next;
        if (q->parent == p) {
            q->parent = NULL;
            q->ppid = 1;
            if (q->state == P_ZOMBIE) proc_free_locked(q);
        }
        q = n;
    }
    p->state = P_ZOMBIE;
    pidfd_exited_locked(p->pid);
    Proc *par = p->parent;
    bool autoreap = !par || par->sigact[17][0] == 1 || (par->sigact[17][1] & L_SA_NOCLDWAIT);
    if (par && p->exit_signal) post_signal_locked(par, NULL, p->exit_signal);
    bool initial = p->initial;
    int ws = p->wstatus;
    if (autoreap) proc_free_locked(p);
    pthread_cond_broadcast(&g_cond);
    if (mm) mm_put(mm);
    if (initial) {
        fflush(stderr);
        _exit((ws & 0x7f) ? 128 + (ws & 0x7f) : (ws >> 8) & 0xff);
    }
}

// ---- signals ----
enum { D_IGN, D_TERM, D_CORE, D_STOP };
static int sig_default(int s) {
    switch (s) {
    case 17: case 18: case 23: case 28: return D_IGN;              // CHLD, CONT, URG, WINCH
    case 19: case 20: case 21: case 22: return D_STOP;             // STOP, TSTP, TTIN, TTOU
    case 3: case 4: case 5: case 6: case 7: case 8: case 11: case 24: case 25: case 31: return D_CORE;
    default: return D_TERM;
    }
}

// Makes a signal pending for process p (t == NULL) or one of its threads. A signal whose default
// action kills the process does so right away, even if the process never makes another system call.
void post_signal_locked(Proc *p, GuestCtx *t, int sig) {
    if (sig < 1 || sig > 64 || p->state != P_RUNNING) return;
    uint64_t bit = 1ull << (sig - 1), h = p->sigact[sig][0];
    if (sig == 9) { kill_proc_locked(p, 9); return; }
    if (sig == 19 || h == 1) return;                               // SIGSTOP (no job control), SIG_IGN
    if (h == 0) {
        int d = sig_default(sig);
        if (d == D_IGN || d == D_STOP) return;
        bool can = false;
        if (t) can = !(t->sigmask & bit);
        else for (GuestCtx *x = p->threads; x && !can; x = x->next_thread) can = !(x->sigmask & bit);
        if (can) { kill_proc_locked(p, sig); return; }
    }
    if (t) {
        __atomic_or_fetch(&t->pending, bit, __ATOMIC_SEQ_CST);
        kick(t);
    } else {
        __atomic_or_fetch(&p->pending, bit, __ATOMIC_SEQ_CST);
        for (GuestCtx *x = p->threads; x; x = x->next_thread)
            if (!(x->sigmask & bit) || (x->waitset & bit)) kick(x);
    }
    pthread_cond_broadcast(&g_cond);
    pthread_cond_signal(&hk_cond);
}

void proc_raise(GuestCtx *c, int sig) { LOCKED(post_signal_locked(c->proc, c, sig)); }

void proc_forward_host_signal(int lsig) {
    if (!lsig) return;
    pthread_mutex_lock(&g_lock);
    for (Proc *p = g_procs; p; p = p->next) post_signal_locked(p, NULL, lsig);
    pthread_mutex_unlock(&g_lock);
}

// Takes one deliverable signal (thread-directed first, then process-wide), or returns 0.
static int take_signal(GuestCtx *c) {
    Proc *p = c->proc;
    for (;;) {
        uint64_t mine = c->pending & ~c->sigmask;
        if (mine) {
            int sig = __builtin_ctzll(mine) + 1;
            uint64_t bit = 1ull << (sig - 1);
            if (__atomic_fetch_and(&c->pending, ~bit, __ATOMIC_SEQ_CST) & bit) return sig;
            continue;
        }
        uint64_t pend = p->pending & ~c->sigmask;
        if (!pend) return 0;
        int sig = __builtin_ctzll(pend) + 1;
        uint64_t bit = 1ull << (sig - 1);
        if (__atomic_fetch_and(&p->pending, ~bit, __ATOMIC_SEQ_CST) & bit) return sig;
    }
}

int proc_deliverable(GuestCtx *c) {
    uint64_t pend = (c->pending | c->proc->pending) & ~c->sigmask;
    for (int sig = 1; pend; sig++, pend >>= 1)
        if ((pend & 1) && c->proc->sigact[sig][0] > 1) return sig;
    return 0;
}

#define FRAME_SIZE 4688   // siginfo (128) + arm64 ucontext (4560)
static void push_signal(GuestCtx *c, int sig, uint64_t saved_mask, int code, uint64_t addr) {
    Proc *p = c->proc;
    uint64_t handler = p->sigact[sig][0], flags = p->sigact[sig][1];
    uint64_t restorer = p->sigact[sig][2], mask = p->sigact[sig][3];
    uint64_t base = c->sp;
    bool on_alt = c->altstack_size && c->sp > c->altstack_sp && c->sp <= c->altstack_sp + c->altstack_size;
    if ((flags & 0x08000000) && c->altstack_size && !on_alt) base = c->altstack_sp + c->altstack_size;   // SA_ONSTACK
    uint64_t sp = (base - FRAME_SIZE - 32) & ~15ull;
    LOGV("[%llu] push signal %d: sp %llx -> frame %llx (alt %llx+%llx, on_alt %d, flags %llx)\n", (unsigned long long)c->tid, sig,
         (unsigned long long)c->sp, (unsigned long long)sp, (unsigned long long)c->altstack_sp, (unsigned long long)c->altstack_size, on_alt, (unsigned long long)flags);
    uint8_t *f = gptr(sp);
    memset(f, 0, FRAME_SIZE);
    *(int32_t *)f = sig;                                   // siginfo.si_signo
    *(int32_t *)(f + 8) = code;                            // si_code
    struct SigExtra *x = c->sigx[sig].code ? &c->sigx[sig] : p->sigx[sig].code ? &p->sigx[sig] : NULL;
    if (x && code == 0) {                                  // a POSIX timer: SI_TIMER, timer id, overrun, sigev_value
        *(int32_t *)(f + 8) = x->code; *(int32_t *)(f + 16) = x->timerid; *(int32_t *)(f + 20) = 0; *(uint64_t *)(f + 24) = x->value;
        x->code = 0;
    }
    else if (sig == 11 || sig == 7 || sig == 4 || sig == 8 || sig == 5) *(uint64_t *)(f + 16) = addr;   // si_addr
    else { *(int32_t *)(f + 16) = p->pid; *(uint32_t *)(f + 20) = p->uid; }                          // si_pid, si_uid
    uint8_t *uc = f + 128;
    *(uint64_t *)(uc + 40) = saved_mask;                   // uc_sigmask
    uint8_t *mc = uc + 176;                                // uc_mcontext
    *(uint64_t *)mc = addr;                                // fault_address
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
    c->x[30] = (flags & L_SA_RESTORER) ? restorer : VDSO_ADDR;
    c->sp = sp;
    c->pc = handler;
    c->sigmask |= (mask | ((flags & L_SA_NODEFER) ? 0 : 1ull << (sig - 1))) & ~UNBLOCKABLE;
    if (flags & L_SA_RESETHAND) p->sigact[sig][0] = 0;
}

void proc_sigreturn(GuestCtx *c) {
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
    c->sigmask = *(uint64_t *)(uc + 40) & ~UNBLOCKABLE;
    LOGV("[%llu] sigreturn: pc %llx sp %llx\n", (unsigned long long)c->tid, (unsigned long long)c->pc, (unsigned long long)c->sp);
}

__attribute__((noreturn)) static void die_by_signal(GuestCtx *c, int sig) {
    LOCKED(kill_proc_locked(c->proc, sig));
    thread_leave(c);
}

void sys_deliver_signals(GuestCtx *c) {
    if (c->kill) thread_leave(c);
    Proc *p = c->proc;
    if (!((c->pending | p->pending) & ~c->sigmask)) return;
    for (;;) {
        int sig = take_signal(c);
        if (!sig) return;
        uint64_t h = p->sigact[sig][0];
        if (h == 1) continue;
        if (h == 0) {
            int d = sig_default(sig);
            if (d == D_IGN || d == D_STOP) continue;
            die_by_signal(c, sig);
        }
        LOGV("[%llu] deliver signal %d at pc %llx\n", (unsigned long long)c->tid, sig, (unsigned long long)c->pc);
        push_signal(c, sig, c->sigmask, sig == 17 ? 1 : 0, 0);
        return;
    }
}

uint64_t proc_sigsuspend(GuestCtx *c, uint64_t mask) {
    uint64_t old = c->sigmask;
    c->sigmask = mask & ~UNBLOCKABLE;
    sigset_t usr1, prev;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &usr1, &prev);
    sigset_t wait = prev;
    sigdelset(&wait, SIGUSR1);
    while (!c->kill && !((c->pending | c->proc->pending) & ~c->sigmask)) sigsuspend(&wait);
    pthread_sigmask(SIG_SETMASK, &prev, NULL);
    c->x[0] = ERR(4);
    int sig = c->kill ? 0 : take_signal(c);
    if (sig && c->proc->sigact[sig][0] > 1) {
        push_signal(c, sig, old, sig == 17 ? 1 : 0, 0);   // the handler returns to the caller's mask
    } else {
        c->sigmask = old;
        if (sig && c->proc->sigact[sig][0] == 0 && sig_default(sig) != D_IGN && sig_default(sig) != D_STOP)
            die_by_signal(c, sig);
    }
    return 0;
}

// rt_sigtimedwait: take a signal from set, waiting up to timeout (NULL = forever). Fills a Linux
// siginfo. Returns the signal number or -errno.
int64_t proc_sigtimedwait(GuestCtx *c, uint64_t set, uint64_t info, const struct timespec *timeout) {
    Proc *p = c->proc;
    set &= ~UNBLOCKABLE;
    int64_t deadline = -1;
    if (timeout) deadline = (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC) + timeout->tv_sec * 1000000000 + timeout->tv_nsec;
    c->waitset = set;
    int64_t r;
    for (;;) {
        uint64_t avail = (c->pending | p->pending) & set;
        if (avail) {
            int sig = __builtin_ctzll(avail) + 1;
            uint64_t bit = 1ull << (sig - 1);
            if (!(__atomic_fetch_and(&c->pending, ~bit, __ATOMIC_SEQ_CST) & bit) &&
                !(__atomic_fetch_and(&p->pending, ~bit, __ATOMIC_SEQ_CST) & bit)) continue;
            if (info) {
                uint8_t *si = gptr(info);
                memset(si, 0, 128);
                *(int32_t *)si = sig;
                struct SigExtra *x = c->sigx[sig].code ? &c->sigx[sig] : p->sigx[sig].code ? &p->sigx[sig] : NULL;
                if (x) {                                       // SI_TIMER: bionic's SIGEV_THREAD helper checks it
                    *(int32_t *)(si + 8) = x->code; *(int32_t *)(si + 16) = x->timerid; *(uint64_t *)(si + 24) = x->value;
                    x->code = 0;
                } else {
                    *(int32_t *)(si + 8) = 0;                  // SI_USER
                    *(int32_t *)(si + 16) = p->pid;
                    *(uint32_t *)(si + 20) = p->uid;
                }
            }
            r = sig;
            break;
        }
        if (c->kill) { r = -EINTR; break; }
        int ms = -1;
        if (deadline >= 0) {
            int64_t left = deadline - (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC);
            if (left <= 0) { r = -EAGAIN; break; }
            ms = (int)((left + 999999) / 1000000);
        }
        int rc = poll(NULL, 0, ms);                           // a kick (SIGUSR1) interrupts this
        if (rc < 0 && errno != EINTR) { r = -errno; break; }
        if (proc_deliverable(c)) { r = -EINTR; break; }
    }
    c->waitset = 0;
    return r;
}

// membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED): like the kernel's IPIs, every other thread of the
// process runs a full barrier (its kick handler) before we return. ART's collector depends on it.
uint64_t proc_membarrier(GuestCtx *c, int cmd) {
    if (cmd == 0) return 1 | 8 | 16 | 32 | 64;            // QUERY: GLOBAL, PRIVATE_EXPEDITED (+REGISTER), SYNC_CORE variants
    if (cmd == 16 || cmd == 64 || cmd == 2) return 0;    // REGISTER_* and GLOBAL_EXPEDITED: nothing to set up
    Proc *p = c->proc;
    uint64_t gen = __atomic_add_fetch(&g_barrier_gen, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&g_lock);
    for (GuestCtx *t = p->threads; t; t = t->next_thread) if (t != c) kick(t);
    pthread_mutex_unlock(&g_lock);
    for (int spins = 0; spins < 2000; spins++) {           // up to ~100 ms
        bool all = true;
        pthread_mutex_lock(&g_lock);
        for (GuestCtx *t = p->threads; t && all; t = t->next_thread) if (t != c && t->barrier_ack < gen) all = false;
        pthread_mutex_unlock(&g_lock);
        if (all) break;
        usleep(50);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}

// ---- diagnostics ----
void proc_backtrace(GuestCtx *c) {
    Mm *m = c->proc->mm;
    if (!m) return;
    char b[1200];
    mm_symbolize(m, c->pc, b, sizeof b);
    fprintf(stderr, "  pc %s\n", b);
    mm_symbolize(m, c->x[30], b, sizeof b);
    fprintf(stderr, "  lr %s\n", b);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 16 && fp && !(fp & 7) && mm_readable(m, fp, 16); i++) {
        uint64_t *fr = (uint64_t *)(uintptr_t)(c->base + fp);
        mm_symbolize(m, fr[1] & 0x0000ffffffffffffull, b, sizeof b);
        fprintf(stderr, "  #%d %s\n", i, b);
        fp = fr[0];
    }
}

static const char *sig_name(int s) {
    static const char *n[] = {"?", "SIGHUP", "SIGINT", "SIGQUIT", "SIGILL", "SIGTRAP", "SIGABRT", "SIGBUS",
                              "SIGFPE", "SIGKILL", "SIGUSR1", "SIGSEGV", "SIGUSR2", "SIGPIPE", "SIGALRM", "SIGTERM"};
    return s > 0 && s < 16 ? n[s] : "signal";
}

// A fault in guest code (SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE) at c->pc. Delivered to the
// guest's handler if it has one and the signal isn't blocked, as the kernel would; otherwise the
// process dies. ART relies on this: its interpreter and compiled code use implicit null and
// stack-overflow checks and turn the resulting SIGSEGV into Java exceptions.
void sys_fault(GuestCtx *c, int sig, int code, uint64_t addr) {
    Proc *p = c->proc;
    uint64_t bit = 1ull << (sig - 1);
    if (sig == 11 && p->mm && mm_readable(p->mm, addr & ~(PG - 1), 1)) code = 2;   // SEGV_ACCERR: mapped but not that way
    static int show_faults = -1;
    if (show_faults < 0) show_faults = getenv("ONIRUN_FAULTS") != NULL;
    if (p->sigact[sig][0] > 1 && !(c->sigmask & bit) && p->state == P_RUNNING) {
        if (show_faults && sig != 11) {                   // SIGSEGV is ART's null check; the others are news
            fprintf(stderr, "onirun: [%d] %s: signal %d at pc %llx\n", p->pid, p->exe, sig, (unsigned long long)c->pc);
            proc_backtrace(c);
        } else if (show_faults && sig == 11 && addr > 0x10000) {   // a real bad pointer, not a null check
            char where[1200];
            mm_symbolize(p->mm, c->pc, where, sizeof where);
            fprintf(stderr, "onirun: [%d] %s: SIGSEGV at %s addr %llx\n", p->pid, p->exe, where, (unsigned long long)addr);
            proc_backtrace(c);
        }
        if (g_verbose) {
            char where[1200];
            mm_symbolize(p->mm, c->pc, where, sizeof where);
            uint32_t insn = mm_readable(p->mm, c->pc, 4) ? *(uint32_t *)(uintptr_t)(c->base + c->pc) : 0;
            LOGV("[%llu] fault signal %d code %d at pc %llx (%s, insn %08x) addr %llx -> handler\n", (unsigned long long)c->tid,
                 sig, code, (unsigned long long)c->pc, where, insn, (unsigned long long)addr);
        }
        push_signal(c, sig, c->sigmask, code, addr);
        return;
    }
    fprintf(stderr, "onirun: [%d] %s: %s at guest address %llx\n", p->pid, p->exe, sig_name(sig), (unsigned long long)addr);
    proc_backtrace(c);
    die_by_signal(c, sig);
}

void sys_abandoned(GuestCtx *c) {
    if (c->fault_sig) {
        int sig = c->fault_sig, code = c->fault_code;
        uint64_t addr = c->fault_addr;
        c->fault_sig = 0;
        sys_fault(c, sig, code, addr);
        return;
    }
    thread_leave(c);
}

// Notes an abort() or similar so the reason isn't lost when the process dies.
static void note_self_fatal(GuestCtx *c, Proc *q, int sig) {
    // SIGABRT to oneself is always reported: abort() callers often log nothing, and debuggerd's
    // handler (installed in every process) would otherwise hide where it came from.
    if (q != c->proc || (q->sigact[sig][0] != 0 && sig != 6) || sig_default(sig) != D_CORE) return;
    bool can = false;                                 // only fatal if some thread can take it
    for (GuestCtx *x = q->threads; x && !can; x = x->next_thread) can = !(x->sigmask & (1ull << (sig - 1)));
    if (!can) return;
    fprintf(stderr, "onirun: [%d] %s: %s\n", q->pid, q->exe, sig_name(sig));
    proc_backtrace(c);
}

uint64_t proc_kill(GuestCtx *c, int64_t pid, int sig) {
    if (sig < 0 || sig > 64) return ERR(22);
    int found = 0;
    pthread_mutex_lock(&g_lock);
    for (Proc *q = g_procs; q; q = q->next) {
        bool match = pid > 0 ? q->pid == pid : pid == 0 ? q->pgid == c->proc->pgid
                   : pid == -1 ? q != c->proc : q->pgid == -pid;
        if (!match) continue;
        found++;
        if (!sig) continue;
        note_self_fatal(c, q, sig);
        post_signal_locked(q, NULL, sig);
    }
    pthread_mutex_unlock(&g_lock);
    return found ? 0 : ERR(3);
}

uint64_t proc_tgkill(GuestCtx *c, int64_t tgid, int64_t tid, int sig) {
    if (sig < 0 || sig > 64) return ERR(22);
    pthread_mutex_lock(&g_lock);
    GuestCtx *t = find_thread_locked(tid);
    uint64_t r = 0;
    if (!t || (tgid > 0 && t->proc->pid != tgid)) r = ERR(3);
    else if (sig) {
        note_self_fatal(c, t->proc, sig);
        post_signal_locked(t->proc, t, sig);
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

// ---- clone: threads and processes ----
uint64_t proc_clone(GuestCtx *c, uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t tls, uint64_t ctid) {
    Proc *p = c->proc;
    LOCKED(if (fdtrace_on(p)) fprintf(stderr, "FDT %d/%llu(%s) clone flags %#llx sp %#llx\n", p->pid, (unsigned long long)c->tid, c->name, (unsigned long long)flags, (unsigned long long)newsp));
    GuestCtx *nc = malloc(sizeof *nc);
    memcpy(nc, c, sizeof *nc);
    nc->x[0] = 0;
    nc->pending = 0;
    nc->kill = 0;
    nc->fault_sig = 0;
    nc->host_thread = NULL;
    nc->next_thread = NULL;
    nc->clear_child_tid = (flags & L_CLONE_CHILD_CLEARTID) ? ctid : 0;
    nc->altstack_sp = nc->altstack_size = 0;
    nc->waitset = 0;
    memset(nc->sigx, 0, sizeof nc->sigx);
    if (flags & L_CLONE_SETTLS) nc->tpidr = tls;
    if (newsp) nc->sp = newsp;

    if (flags & L_CLONE_THREAD) {   // a guest thread on a new host thread
        pthread_mutex_lock(&g_lock);
        if (!(flags & L_CLONE_FILES)) {
            // Linux gives such a thread a private copy of the fd table. debuggerd's pseudothread
            // relies on it: it raw-closes fds 0..1023, which must not touch the process's own.
            // It runs in a shadow Proc: same pid and address space, its own fd copies, not on
            // the process list (pid lookups, signals and wait still reach the real process).
            Proc *sh = malloc(sizeof *sh);
            memcpy(sh, p, sizeof *sh);
            sh->shadow_of = p;
            sh->next = NULL;
            sh->threads = NULL;
            sh->nthreads = 0;
            sh->exit_signal = 0;
            mm_get(sh->mm);
            for (int i = 0; i < MAX_FDS; i++) {
                if (p->fd[i].host < 0) continue;
                sh->fd[i].host = p->fd[i].host;     // shared, as after fork
                host_ref_locked(p->fd[i].host);
            }
            nc->proc = sh;
            p = sh;
        }
        nc->tid = (uint64_t)g_next_id++;
        nc->next_thread = p->threads;
        p->threads = nc;
        p->nthreads++;
        if (flags & L_CLONE_PARENT_SETTID) *(uint32_t *)gptr(ptid) = (uint32_t)nc->tid;
        if (flags & L_CLONE_CHILD_SETTID) *(uint32_t *)gptr(ctid) = (uint32_t)nc->tid;
        uint64_t r = nc->tid;
        if (p->state != P_RUNNING || thread_spawn_locked(nc)) {
            p->threads = nc->next_thread;
            p->nthreads--;
            free(nc->altstack);
            free(nc);
            r = ERR(11);
        }
        pthread_mutex_unlock(&g_lock);
        return r;
    }

    // A new process: copy the address space (fork) or share it (CLONE_VM, as vfork does).
    Mm *mm;
    if (flags & L_CLONE_VM) { mm = p->mm; mm_get(mm); }
    else if (!(mm = mm_fork(p->mm))) { free(nc); return ERR(12); }
    nc->base = (uint64_t)(uintptr_t)mm->base;
    nc->space = mm->tr;
    pthread_mutex_lock(&g_lock);
    Proc *q = proc_alloc_locked();
    q->ppid = p->pid;
    q->pgid = p->pgid;
    q->sid = p->sid;
    q->parent = p;
    q->mm = mm;
    memcpy(q->cwd, p->cwd, sizeof q->cwd);
    memcpy(q->exe, p->exe, sizeof q->exe);
    q->umask = p->umask;
    q->uid = p->uid;
    q->gid = p->gid;
    q->euid = p->euid;
    q->egid = p->egid;
    memcpy(q->caps, p->caps, sizeof q->caps);
    memcpy(q->groups, p->groups, sizeof q->groups);
    q->ngroups = p->ngroups;
    q->nepoll = p->nepoll;
    memcpy(q->sigact, p->sigact, sizeof q->sigact);
    q->exit_signal = (int)(flags & 0xff);
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fd[i].host < 0) continue;
        q->fd[i] = (FdEnt){ p->fd[i].host, p->fd[i].cloexec };   // the child shares the open file
        host_ref_locked(p->fd[i].host);
    }
    nc->proc = q;
    nc->tid = (uint64_t)q->pid;
    q->threads = nc;
    q->nthreads = 1;
    if (flags & L_CLONE_PARENT_SETTID) *(uint32_t *)gptr(ptid) = (uint32_t)q->pid;
    if (flags & L_CLONE_CHILD_SETTID) *(uint32_t *)(void *)(mm->base + UNTAG(ctid)) = (uint32_t)q->pid;
    if (thread_spawn_locked(nc)) die("onirun: cannot create a host thread");
    uint64_t r = (uint64_t)q->pid;
    if (flags & L_CLONE_VFORK)   // the parent waits until the child execs or exits
        while (!q->vfork_done && !c->kill) pthread_cond_wait(&g_cond, &g_lock);
    pthread_mutex_unlock(&g_lock);
    return r;
}

// ---- exec ----
static char **copy_strv(uint64_t a) {
    int n = 0;
    const uint64_t *v = gptr(a);
    while (v && v[n] && n < 4096) n++;
    char **out = calloc((size_t)n + 1, sizeof(char *));
    for (int i = 0; i < n; i++) out[i] = strdup(gstr(v[i]));
    return out;
}
static void free_strv(char **v) {
    for (int i = 0; v && v[i]; i++) free(v[i]);
    free(v);
}

uint64_t proc_execve(GuestCtx *c, const char *gpath, uint64_t argv_g, uint64_t envp_g) {
    Proc *p = c->proc;
    char *path = strdup(gpath);
    char **argv = copy_strv(argv_g), **envp = copy_strv(envp_g);
    int e = load_check(p, path);
    if (e) goto fail;
    // The other threads go first, as on Linux.
    pthread_mutex_lock(&g_lock);
    for (GuestCtx *t = p->threads; t; t = t->next_thread)
        if (t != c) { t->kill = 1; kick(t); }
    pthread_cond_signal(&hk_cond);
    while (p->nthreads > 1 && !c->kill) pthread_cond_wait(&g_cond, &g_lock);
    pthread_mutex_unlock(&g_lock);
    if (c->kill) { e = -EINTR; goto fail; }

    Mm *nm = mm_new();
    ExecImage img;
    e = nm ? load_exec(p, nm, path, argv, envp, &img) : -ENOMEM;
    if (e) {
        if (nm) mm_put(nm);
        fprintf(stderr, "onirun: [%d] exec %s failed after the point of no return (errno %d)\n", p->pid, path, -e);
        LOCKED(kill_proc_locked(p, 11));
        goto fail;
    }
    // A vfork child's clear_child_tid points into the parent's memory: release it like Linux.
    if (c->clear_child_tid && p->mm->refs > 1) wake_tid_word(c, c->clear_child_tid);
    c->clear_child_tid = 0;
    Mm *old = p->mm;
    pthread_mutex_lock(&g_lock);
    p->mm = nm;
    c->base = (uint64_t)(uintptr_t)nm->base;
    c->space = nm->tr;
    c->tid = (uint64_t)p->pid;
    for (int i = 0; i < MAX_FDS; i++)
        if (p->fd[i].host >= 0 && p->fd[i].cloexec) { host_close_locked(p->fd[i].host); p->fd[i].host = -1; note_closer(p, i, 'e'); }
    for (int s = 1; s <= 64; s++)
        if (p->sigact[s][0] > 1) memset(p->sigact[s], 0, sizeof p->sigact[s]);
    snprintf(p->exe, sizeof p->exe, "%s", path);
    set_cmdline(p, argv);
    p->stack_start = img.sp;
    p->vfork_done = true;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    mm_put(old);
    memset(c->x, 0, sizeof c->x);
    memset(c->q, 0, sizeof c->q);
    c->pc = img.entry;
    c->sp = img.sp;
    c->tpidr = 0;
    c->nzcv = 0;
    c->fpsr = 0;
    c->fpcr = 0;
    free(path);
    free_strv(argv);
    free_strv(envp);
    return 0;
fail:
    free(path);
    free_strv(argv);
    free_strv(envp);
    return ERR(-e);
}

// ---- wait ----
// Waits for a child matching pid (wait4 conventions). Returns its id and status, 0 for WNOHANG
// with nothing ready, or -errno. nowait leaves the zombie in place (waitid WNOWAIT).
static int64_t wait_core(GuestCtx *c, int64_t pid, bool nohang, bool nowait, int *wstatus, uint32_t *uid) {
    Proc *p = c->proc;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        bool any = false;
        for (Proc *q = g_procs; q; q = q->next) {
            if (q->parent != p) continue;
            bool match = pid > 0 ? q->pid == pid : pid == 0 ? q->pgid == p->pgid : pid == -1 ? true : q->pgid == -pid;
            if (!match) continue;
            any = true;
            if (q->state != P_ZOMBIE) continue;
            int id = q->pid;
            *wstatus = q->wstatus;
            if (uid) *uid = q->uid;
            if (!nowait) proc_free_locked(q);
            pthread_mutex_unlock(&g_lock);
            return id;
        }
        if (!any) { pthread_mutex_unlock(&g_lock); return -10; }        // ECHILD
        if (nohang) { pthread_mutex_unlock(&g_lock); return 0; }
        if (c->kill || proc_deliverable(c)) { pthread_mutex_unlock(&g_lock); return -4; }
        pthread_cond_wait(&g_cond, &g_lock);
    }
}

// waitid(idtype, id, infop, options): P_ALL 0, P_PID 1, P_PGID 2, P_PIDFD 3; WNOHANG 1, WNOWAIT 0x01000000.
uint64_t proc_waitid(GuestCtx *c, int idtype, int64_t id, uint64_t infop, int options) {
    int64_t sel;
    if (idtype == 0) sel = -1;
    else if (idtype == 1) sel = id;
    else if (idtype == 2) sel = id ? -id : 0;
    else if (idtype == 3) { int pid = proc_pidfd_pid(c->proc, (int)id); if (pid < 0) return ERR(9); sel = pid; }
    else return ERR(22);
    int ws = 0;
    uint32_t uid = 0;
    int64_t r = wait_core(c, sel, options & 1, options & 0x01000000, &ws, &uid);
    if (r < 0) return ERR(-r);
    if (infop) {
        uint8_t *si = gptr(infop);
        memset(si, 0, 128);
        if (r > 0) {
            int32_t signo = 17, code, st;
            if (ws & 0x7f) { code = (ws & 0x80) ? 3 : 2; st = ws & 0x7f; }   // CLD_DUMPED / CLD_KILLED
            else { code = 1; st = (ws >> 8) & 0xff; }                          // CLD_EXITED
            memcpy(si, &signo, 4); memcpy(si + 8, &code, 4);
            int32_t pid32 = (int32_t)r; memcpy(si + 16, &pid32, 4); memcpy(si + 20, &uid, 4); memcpy(si + 24, &st, 4);
        }
    }
    return 0;
}

uint64_t proc_wait4(GuestCtx *c, int64_t pid, uint64_t status, int options, uint64_t rusage) {
    int ws = 0;
    int64_t r = wait_core(c, pid, options & 1, false, &ws, NULL);
    if (r <= 0) return r < 0 ? ERR(-r) : 0;
    if (status) *(int32_t *)gptr(status) = ws;
    if (rusage) memset(gptr(rusage), 0, 144);
    return (uint64_t)r;
}
// ---- process groups ----
uint64_t proc_setpgid(GuestCtx *c, int64_t pid, int64_t pgid) {
    uint64_t r = ERR(3);
    pthread_mutex_lock(&g_lock);
    for (Proc *q = g_procs; q; q = q->next)
        if ((pid == 0 && q == c->proc) || q->pid == pid) { q->pgid = pgid ? (int)pgid : q->pid; r = 0; break; }
    pthread_mutex_unlock(&g_lock);
    return r;
}
uint64_t proc_getpgid(GuestCtx *c, int64_t pid) {
    uint64_t r = ERR(3);
    pthread_mutex_lock(&g_lock);
    for (Proc *q = g_procs; q; q = q->next)
        if ((pid == 0 && q == c->proc) || q->pid == pid) { r = (uint64_t)q->pgid; break; }
    pthread_mutex_unlock(&g_lock);
    return r;
}
uint64_t proc_setsid(GuestCtx *c) {
    Proc *p = c->proc;
    LOCKED({ p->sid = p->pgid = p->pid; });
    return (uint64_t)p->pid;
}

// ---- startup ----
// Re-kicks threads that must leave or have a signal to take, in case a kick landed just before
// they blocked.
static void *housekeeping(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        bool busy = false;
        for (Proc *p = g_procs; p; p = p->next)
            for (GuestCtx *t = p->threads; t; t = t->next_thread)
                if (t->kill || ((t->pending | p->pending) & ~t->sigmask)) { kick(t); busy = true; }
        if (busy) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20 * 1000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&hk_cond, &g_lock, &ts);
        } else {
            pthread_cond_wait(&hk_cond, &g_lock);
        }
    }
    return NULL;
}

static Proc *spawn(const char *path, char **argv, char **envp, const char **sockets, bool initial);
Proc *proc_start(const char *path, char **argv, char **envp, const char **sockets) { return spawn(path, argv, envp, sockets, true); }
Proc *proc_spawn(const char *path, char **argv, char **envp, const char **sockets) { return spawn(path, argv, envp, sockets, false); }
void proc_kill_locked(Proc *p, int sig) { post_signal_locked(p, NULL, sig); }

static Proc *spawn(const char *path, char **argv, char **envp, const char **sockets, bool initial) {
    pthread_mutex_lock(&g_lock);
    Proc *p = proc_alloc_locked();
    p->initial = initial;
    p->ppid = 1;
    pthread_mutex_unlock(&g_lock);
    // Signals ignored when onirun started (nohup, background jobs) stay ignored, as across exec.
    for (int hs = 1; hs < 32; hs++) {
        struct sigaction cur;
        int ls = lx_sig(hs);
        if (ls && !sigaction(hs, NULL, &cur) && cur.sa_handler == SIG_IGN) p->sigact[ls][0] = 1;
    }
    if (!(p->mm = mm_new())) die("onirun: cannot reserve an address space");
    // Standard fds: stdin is /dev/null (Android services never read it, and the host's stdin may
    // be a socket, which would confuse zygote's fd checks); stdout/stderr are the host's but
    // claim to be /dev/null, which is what zygote reopens them as in its children.
    p->fd[0].host = open("/dev/null", O_RDONLY | O_CLOEXEC);
    p->fd[1].host = dup(1);
    p->fd[2].host = dup(2);
    for (int i = 0; i < 3; i++) host_ref_locked(p->fd[i].host);
    for (int i = 0; i < 3; i++) fd_set_path(p->fd[i].host, "/dev/null");
    // Sockets init would have created for a service (zygote's, for example): listening, bound in
    // the guest namespace, and announced as ANDROID_SOCKET_<name>=<fd>.
    char *env2[256];
    int ne = 0;
    for (; envp[ne] && ne < 200; ne++) env2[ne] = envp[ne];
    for (int i = 0; sockets && sockets[i] && ne < 250; i++) {
        char name[64];
        snprintf(name, sizeof name, "%s", sockets[i]);
        char *colon = strchr(name, ':');                 // "name:dgram" from init's "socket name dgram"
        bool dgram = colon && !strcmp(colon + 1, "dgram");
        if (colon) *colon = 0;
        int g = dgram ? sock_dgram_for(p, name) : sock_listen_for(p, name);
        if (g < 0) die("onirun: cannot create socket %s", name);
        char *kv = malloc(strlen(name) + 40);
        sprintf(kv, "ANDROID_SOCKET_%s=%d", name, g);
        env2[ne++] = kv;
    }
    env2[ne] = NULL;
    envp = env2;
    ExecImage img;
    int e = load_exec(p, p->mm, path, argv, envp, &img);
    if (e) {
        if (initial) die("onirun: cannot load %s (errno %d)", path, -e);
        fprintf(stderr, "onirun: cannot load %s (errno %d)\n", path, -e);
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < MAX_FDS; i++) if (p->fd[i].host >= 0) host_close_locked(p->fd[i].host);
        proc_free_locked(p);
        pthread_mutex_unlock(&g_lock);
        mm_put(p->mm);
        return NULL;
    }
    snprintf(p->exe, sizeof p->exe, "%s", path);
    set_cmdline(p, argv);
    p->stack_start = img.sp;
    GuestCtx *c = calloc(1, sizeof *c);
    c->proc = p;
    c->base = (uint64_t)(uintptr_t)p->mm->base;
    c->space = p->mm->tr;
    c->tid = (uint64_t)p->pid;
    c->pc = img.entry;
    c->sp = img.sp;
    pthread_mutex_lock(&g_lock);
    p->threads = c;
    p->nthreads = 1;
    if (thread_spawn_locked(c)) die("onirun: cannot create a host thread");
    pthread_mutex_unlock(&g_lock);
    return p;
}

static void proc_noop_handler(int sig) { (void)sig; }

void sys_main(const char *path, char **argv, char **envp, const char **sockets) {
    // Terminal and job signals are taken here with sigwait and forwarded to the guest processes;
    // every other thread inherits the blocked mask. SIGUSR1 stays open: it is the kick.
    sigset_t set;
    sigemptyset(&set);
    int fwd[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGALRM, SIGUSR2, SIGWINCH, SIGTSTP, SIGCONT, SIGTTIN, SIGTTOU,
                 SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF, SIGCHLD, SIGINFO};
    for (unsigned i = 0; i < sizeof fwd / sizeof fwd[0]; i++) sigaddset(&set, fwd[i]);
    // Blocked signals whose disposition is "ignore" are discarded, not queued: give those a handler.
    struct sigaction keep = { .sa_handler = (void (*)(int))1 /* SIG_IGN placeholder replaced below */ };
    keep.sa_handler = proc_noop_handler;
    for (unsigned i = 0; i < sizeof fwd / sizeof fwd[0]; i++) sigaction(fwd[i], &keep, NULL);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    struct rlimit rl;
    if (!getrlimit(RLIMIT_NOFILE, &rl)) {
        rl.rlim_cur = rl.rlim_max < HOST_FDS ? rl.rlim_max : HOST_FDS;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    umask(0);   // guest umasks are applied by the syscall layer
    pthread_t hk;
    pthread_create(&hk, NULL, housekeeping, NULL);
    sock_init();
    props_init();
    bpf_init();
    if (path) proc_start(path, argv, envp, sockets);
    else {
        char table[1200];
        snprintf(table, sizeof table, "%s/oni/init.txt", g_rootfs);
        if (!init_start(table, envp)) die("onirun: cannot read %s", table);
    }
    for (;;) {
        int s;
        if (sigwait(&set, &s) != 0) continue;
        if (s == SIGINFO) { proc_dump_all(); continue; }
        if (s == SIGUSR2) {
            // Debugging aid: SIGUSR2 to onirun sends SIGQUIT to the guest pid in /oni/sigquit (ART then
            // dumps every thread's Java stack). Without that file SIGUSR2 is forwarded like the rest.
            char qp[1200];
            snprintf(qp, sizeof qp, "%s/oni/sigquit", g_rootfs);
            FILE *qf = fopen(qp, "r");
            int qpid = 0;
            if (qf) { if (fscanf(qf, "%d", &qpid) != 1) qpid = 0; fclose(qf); }
            if (qpid > 0) {
                pthread_mutex_lock(&g_lock);
                Proc *t = proc_find(qpid);
                if (t) post_signal_locked(t, NULL, 3);
                pthread_mutex_unlock(&g_lock);
                fprintf(stderr, "onirun: SIGQUIT -> guest pid %d%s\n", qpid, t ? "" : " (no such process)");
                continue;
            }
        }
        if (s != SIGCHLD) proc_forward_host_signal(lx_sig(s));
    }
}

// SIGINFO (Ctrl-T) to onirun: where is every guest thread? The pc is the last one recorded at a
// block exit or system call, so for a thread in a tight loop it points near, not at, the loop.
void proc_dump_all(void) {
    pthread_mutex_lock(&g_lock);
    fprintf(stderr, "onirun: --- guest threads ---\n");
    for (Proc *p = g_procs; p; p = p->next) {
        fprintf(stderr, "[%d] %s uid %u/%u state %d threads %d\n", p->pid, p->exe, p->uid, p->euid, p->state, p->nthreads);
        for (GuestCtx *t = p->threads; t; t = t->next_thread) {
            char where[1200] = "?", lr[1200] = "?";
            if (p->mm) { mm_symbolize(p->mm, t->pc, where, sizeof where); mm_symbolize(p->mm, t->x[30], lr, sizeof lr); }
            fprintf(stderr, "  tid %llu %-16s exit %llu pc %s lr %s x8 %llu x0 %llx x1 %llx x2 %llx\n", (unsigned long long)t->tid,
                    t->name[0] ? t->name : "-", (unsigned long long)t->exit_kind, where, lr, (unsigned long long)t->x[8],
                    (unsigned long long)t->x[0], (unsigned long long)t->x[1], (unsigned long long)t->x[2]);
        }
    }
    pthread_mutex_unlock(&g_lock);
    binder_dump();
}

// ---- POSIX timers (timer_create, timer_settime, timer_gettime, timer_getoverrun, timer_delete) ----
// libdispatch timers that post the chosen signal with SI_TIMER details, to the process
// (SIGEV_SIGNAL) or one thread (SIGEV_THREAD_ID, which bionic's SIGEV_THREAD timers use).
typedef struct PTimer {
    int id, pid, sig, notify;
    uint64_t tid, value;
    int64_t interval_ns, deadline_ns;
    dispatch_source_t src;
    struct PTimer *next;
} PTimer;
static PTimer *ptimers;            // g_lock
static int next_timer_id = 1;
static int64_t mono_ns(void) { return (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC); }

static void ptimer_fire(PTimer *t) {
    pthread_mutex_lock(&g_lock);
    Proc *p = proc_find(t->pid);
    if (p && t->notify != 1) {                                          // SIGEV_NONE: nothing to deliver
        GuestCtx *th = NULL;
        if (t->notify == 4) for (GuestCtx *x = p->threads; x; x = x->next_thread) if (x->tid == t->tid) th = x;
        struct SigExtra *x = th ? &th->sigx[t->sig] : &p->sigx[t->sig];
        x->code = -2; x->timerid = t->id; x->value = t->value;          // SI_TIMER
        post_signal_locked(p, th, t->sig);
    }
    t->deadline_ns = t->interval_ns ? mono_ns() + t->interval_ns : 0;
    pthread_mutex_unlock(&g_lock);
}

static PTimer *ptimer_find_locked(Proc *p, int id) {
    for (PTimer *t = ptimers; t; t = t->next) if (t->pid == p->pid && t->id == id) return t;
    return NULL;
}

static void ptimer_disarm(PTimer *t) {
    if (t->src) { dispatch_source_cancel(t->src); dispatch_release(t->src); t->src = NULL; }
    t->deadline_ns = 0;
}

uint64_t proc_timer_call(GuestCtx *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    Proc *p = c->proc;
    uint64_t r = 0;
    pthread_mutex_lock(&g_lock);
    switch (nr) {
    case 107: {                                                         // timer_create(clock, sigevent *, timer_t *id)
        const uint8_t *ev = a1 ? gptr(a1) : NULL;
        PTimer *t = calloc(1, sizeof *t);
        t->pid = p->pid;
        t->id = next_timer_id++;
        t->sig = 14;                                                    // NULL sigevent: SIGALRM to the process
        t->notify = 0;
        t->value = (uint64_t)t->id;
        if (ev) {                                                       // sigev_value 0, signo 8, notify 12, tid 16
            memcpy(&t->value, ev, 8);
            int32_t signo, notify, tid;
            memcpy(&signo, ev + 8, 4); memcpy(&notify, ev + 12, 4); memcpy(&tid, ev + 16, 4);
            t->sig = signo; t->notify = notify; t->tid = (uint64_t)tid;
            if (notify != 1 && (signo < 1 || signo > 64)) { free(t); r = ERR(22); break; }
        }
        t->next = ptimers;
        ptimers = t;
        *(int32_t *)gptr(a2) = t->id;
        break;
    }
    case 110: {                                                         // timer_settime(id, flags, new, old)
        PTimer *t = ptimer_find_locked(p, (int)a0);
        if (!t) { r = ERR(22); break; }
        const int64_t *it = gptr(a2);                                   // it_interval, it_value
        if (a3) {
            int64_t *o = gptr(a3), rem = t->deadline_ns ? t->deadline_ns - mono_ns() : 0;
            if (rem < 0) rem = 0;
            o[0] = t->interval_ns / 1000000000; o[1] = t->interval_ns % 1000000000; o[2] = rem / 1000000000; o[3] = rem % 1000000000;
        }
        ptimer_disarm(t);
        int64_t interval = it[0] * 1000000000 + it[1], value = it[2] * 1000000000 + it[3];
        t->interval_ns = interval;
        if (value) {
            int64_t delay = value;
            if (a1 & 1) {                                               // TIMER_ABSTIME (against realtime, as timerfd here)
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
                delay = value - ((int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec);
                if (delay < 0) delay = 0;
            }
            t->deadline_ns = mono_ns() + delay;
            dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0));
            dispatch_source_set_timer(src, dispatch_time(DISPATCH_TIME_NOW, delay), interval ? (uint64_t)interval : DISPATCH_TIME_FOREVER, 500000);
            PTimer *tt = t;
            dispatch_source_set_event_handler(src, ^{ ptimer_fire(tt); });
            t->src = src;
            dispatch_resume(src);
        }
        break;
    }
    case 108: {                                                         // timer_gettime(id, cur)
        PTimer *t = ptimer_find_locked(p, (int)a0);
        if (!t) { r = ERR(22); break; }
        int64_t *o = gptr(a1), rem = t->deadline_ns ? t->deadline_ns - mono_ns() : 0;
        if (rem < 0) rem = 0;
        o[0] = t->interval_ns / 1000000000; o[1] = t->interval_ns % 1000000000; o[2] = rem / 1000000000; o[3] = rem % 1000000000;
        break;
    }
    case 109: r = ptimer_find_locked(p, (int)a0) ? 0 : ERR(22); break; // timer_getoverrun: never overrun
    case 111: {                                                         // timer_delete(id)
        for (PTimer **pp = &ptimers; *pp; pp = &(*pp)->next)
            if ((*pp)->pid == p->pid && (*pp)->id == (int)a0) {
                PTimer *t = *pp; *pp = t->next;
                // The handler may be about to run: cancel, and free only after cancellation completes.
                if (t->src) { dispatch_source_t src = t->src; t->src = NULL; dispatch_source_set_cancel_handler(src, ^{ free(t); }); dispatch_source_cancel(src); dispatch_release(src); }
                else free(t);
                goto deleted;
            }
        r = ERR(22);
    deleted:
        break;
    }
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}
