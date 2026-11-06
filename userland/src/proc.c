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
typedef struct HostFd { char *path; FdObj *obj; } HostFd;   // guest path (for /proc/self/fd/N), object
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
    if (host < 0 || host >= HOST_FDS) { obj_put(o); return; }
    if (hfd[host].obj) obj_put(hfd[host].obj);
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

static void host_close_locked(int h) {
    if (h < 0) return;
    dir_forget(h);
    set_path_locked(h, NULL);
    if (h < HOST_FDS && hfd[h].obj) { FdObj *o = hfd[h].obj; hfd[h].obj = NULL; obj_put(o); }
    close(h);
}

static int fd_host_locked(Proc *p, int g) { return (g >= 0 && g < MAX_FDS) ? p->fd[g].host : -1; }
int fd_host(Proc *p, int g) { int h; LOCKED(h = fd_host_locked(p, g)); return h; }

static int fd_install_locked(Proc *p, int host, int min, bool cloexec) {
    for (int g = min < 0 ? 0 : min; g < MAX_FDS; g++)
        if (p->fd[g].host < 0) { p->fd[g] = (FdEnt){ host, cloexec }; return g; }
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

int fd_close(Proc *p, int g) {
    vfs_fd_closed(p, g);
    pthread_mutex_lock(&g_lock);
    int h = fd_host_locked(p, g);
    if (h >= 0) { p->fd[g].host = -1; host_close_locked(h); }
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
        if (p->fd[target].host >= 0) host_close_locked(p->fd[target].host);
        p->fd[target] = (FdEnt){ nh, cloexec };
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
    p->uid = p->gid = p->euid = p->egid = 0;      // init's children start as root; they drop privileges themselves
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
    free(c->altstack);
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
    if (sig == 11 || sig == 7 || sig == 4 || sig == 8 || sig == 5) *(uint64_t *)(f + 16) = addr;   // si_addr
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
                *(int32_t *)(si + 8) = 0;                      // SI_USER
                *(int32_t *)(si + 16) = p->pid;
                *(uint32_t *)(si + 20) = p->uid;
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
    if (q != c->proc || q->sigact[sig][0] != 0 || sig_default(sig) != D_CORE) return;
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
    if (flags & L_CLONE_SETTLS) nc->tpidr = tls;
    if (newsp) nc->sp = newsp;

    if (flags & L_CLONE_THREAD) {   // a guest thread on a new host thread
        pthread_mutex_lock(&g_lock);
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
    memcpy(q->groups, p->groups, sizeof q->groups);
    q->ngroups = p->ngroups;
    q->nepoll = p->nepoll;
    memcpy(q->sigact, p->sigact, sizeof q->sigact);
    q->exit_signal = (int)(flags & 0xff);
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fd[i].host < 0) continue;
        int h = dup(p->fd[i].host);
        if (h < 0) continue;
        copy_meta_locked(p->fd[i].host, h);
        q->fd[i] = (FdEnt){ h, p->fd[i].cloexec };
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
        if (p->fd[i].host >= 0 && p->fd[i].cloexec) { host_close_locked(p->fd[i].host); p->fd[i].host = -1; }
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
uint64_t proc_wait4(GuestCtx *c, int64_t pid, uint64_t status, int options, uint64_t rusage) {
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
            int id = q->pid, ws = q->wstatus;
            proc_free_locked(q);
            pthread_mutex_unlock(&g_lock);
            if (status) *(int32_t *)gptr(status) = ws;
            if (rusage) memset(gptr(rusage), 0, 144);
            return (uint64_t)id;
        }
        if (!any) { pthread_mutex_unlock(&g_lock); return ERR(10); }   // ECHILD
        if (options & 1) { pthread_mutex_unlock(&g_lock); return 0; } // WNOHANG
        if (c->kill || proc_deliverable(c)) { pthread_mutex_unlock(&g_lock); return ERR(4); }
        pthread_cond_wait(&g_cond, &g_lock);
    }
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
    for (int i = 0; i < 3; i++) fd_set_path(p->fd[i].host, "/dev/null");
    // Sockets init would have created for a service (zygote's, for example): listening, bound in
    // the guest namespace, and announced as ANDROID_SOCKET_<name>=<fd>.
    char *env2[256];
    int ne = 0;
    for (; envp[ne] && ne < 200; ne++) env2[ne] = envp[ne];
    for (int i = 0; sockets && sockets[i] && ne < 250; i++) {
        int g = sock_listen_for(p, sockets[i]);
        if (g < 0) die("onirun: cannot create socket %s", sockets[i]);
        char *kv = malloc(strlen(sockets[i]) + 40);
        sprintf(kv, "ANDROID_SOCKET_%s=%d", sockets[i], g);
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
