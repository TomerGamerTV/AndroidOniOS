// Internals shared by the Linux personality: mm.c (address spaces), proc.c (processes, threads,
// signals, fds), syscall.c (the system-call table) and loader.c.
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include "onx.h"

#define PG 16384ull
#define PG_UP(x) (((x) + PG - 1) & ~(PG - 1))

// Guest address-space layout. Guest addresses are offsets into the process window.
#define MMAP_MIN 0x10000ull
#define STACK_TOP (WIN_SIZE - (1ull << 20))
#define STACK_SIZE (8ull << 20)
#define VDSO_ADDR STACK_TOP                 // one page holding the signal-return trampoline
#define MMAP_TOP (WIN_SIZE - (1ull << 30))  // mmap(NULL) placement searches down from here

// Linux mmap flags.
#define LX_MAP_SHARED 0x01
#define LX_MAP_FIXED 0x10
#define LX_MAP_ANON 0x20
#define LX_MAP_FIXED_NOREPLACE 0x100000

// ---- address spaces (mm.c) ----
typedef struct Vma {
    uint64_t start, end, off;
    int prot;                 // Linux PROT_* (same bits as Darwin)
    bool shared, file;
    char *path;               // guest path of a file mapping
} Vma;

typedef struct Mm {
    uint8_t *base;            // host address of guest address 0
    uint8_t *resv;            // the whole host reservation (lookup table + window)
    size_t resv_len;
    Vma *v;                   // sorted, non-overlapping
    int n, cap;
    pthread_mutex_t lock;
    struct TrSpace *tr;
    int refs;                 // processes using it (vfork children share their parent's)
} Mm;

Mm *mm_new(void);
Mm *mm_fork(Mm *parent);
void mm_get(Mm *m);
void mm_put(Mm *m);
int64_t mm_map(Mm *m, uint64_t addr, uint64_t len, int prot, int lflags, int hostfd, uint64_t off, const char *path);
int mm_unmap(Mm *m, uint64_t addr, uint64_t len);
int mm_protect(Mm *m, uint64_t addr, uint64_t len, int prot);
int mm_dontneed(Mm *m, uint64_t addr, uint64_t len);
int64_t mm_remap(Mm *m, uint64_t old, uint64_t oldsz, uint64_t newsz, int flags, uint64_t newaddr);
uint64_t mm_exec_end(Mm *m, uint64_t pc);
void mm_symbolize(Mm *m, uint64_t addr, char *out, size_t n);
bool mm_readable(Mm *m, uint64_t addr, uint64_t len);

// ---- processes (proc.c) ----
#define MAX_FDS 1024
typedef struct FdEnt { int host; bool cloexec; } FdEnt;

enum { P_RUNNING, P_EXITING, P_ZOMBIE };

typedef struct Proc {
    int pid, ppid, pgid, sid;
    Mm *mm;
    FdEnt fd[MAX_FDS];        // guest fd -> host fd (-1 = closed)
    char cwd[1024];           // guest path
    char exe[1024];
    mode_t umask;
    uint64_t sigact[65][4];   // Linux struct sigaction: handler, flags, restorer, mask
    volatile uint64_t pending;   // process-directed signals
    GuestCtx *threads;
    int nthreads;
    int state;
    int wstatus;              // Linux wait status once exiting
    int exit_signal;          // sent to the parent on exit (SIGCHLD)
    struct Proc *parent;
    bool vfork_done;          // a vfork child has exec'd or exited
    bool initial;             // the first process: when it exits, onirun exits
    struct Proc *next;
} Proc;

extern pthread_mutex_t g_lock;    // processes, threads, fd tables
#define LOCKED(stmt) do { pthread_mutex_lock(&g_lock); stmt; pthread_mutex_unlock(&g_lock); } while (0)

int  fd_host(Proc *p, int gfd);                       // -1 if not open
int  fd_install(Proc *p, int host, int min, bool cloexec);   // guest fd or -EMFILE (closes host)
int  fd_close(Proc *p, int gfd);
int  fd_dup(Proc *p, int old, int target, bool exact, bool cloexec);
int  fd_cloexec(Proc *p, int gfd, int set);           // set < 0: query
void fd_set_path(int host, const char *guest_path);
bool fd_get_path(int host, char *out, size_t n);
void dir_forget(int host);

uint64_t proc_clone(GuestCtx *c, uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t tls, uint64_t ctid);
uint64_t proc_execve(GuestCtx *c, const char *path, uint64_t argv, uint64_t envp);
__attribute__((noreturn)) void proc_exit_thread(GuestCtx *c, int status);
__attribute__((noreturn)) void proc_exit_group(GuestCtx *c, int status);
uint64_t proc_wait4(GuestCtx *c, int64_t pid, uint64_t status, int options, uint64_t rusage);
uint64_t proc_kill(GuestCtx *c, int64_t pid, int sig);
uint64_t proc_tgkill(GuestCtx *c, int64_t tgid, int64_t tid, int sig);
uint64_t proc_sigsuspend(GuestCtx *c, uint64_t mask);   // sets x0 itself
void proc_sigreturn(GuestCtx *c);
int  proc_deliverable(GuestCtx *c);                   // a pending signal with a handler, or 0
void proc_raise(GuestCtx *c, int sig);                // thread-directed, from the syscall layer
uint64_t proc_setpgid(GuestCtx *c, int64_t pid, int64_t pgid);
uint64_t proc_getpgid(GuestCtx *c, int64_t pid);
uint64_t proc_setsid(GuestCtx *c);
void proc_backtrace(GuestCtx *c);
Proc *proc_start(const char *path, char **argv, char **envp);
void proc_forward_host_signal(int lsig);

// ---- paths and errors (syscall.c) ----
extern char g_rootfs[1024];
int host_path(Proc *p, const char *guest, char *out, size_t outlen, bool follow_last);
int lx_errno(int e);
int lx_sig(int host_sig);
#define ERR(e) ((uint64_t)(int64_t)-(e))
#define HOSTRET(expr) ({ __typeof__(expr) _r = (expr); _r < 0 ? ERR(lx_errno(errno)) : (uint64_t)_r; })

// Guest pointers of the calling thread's process.
static inline void *gptr(uint64_t a) { return a ? (void *)(uintptr_t)(tr_cur->base + a) : NULL; }
static inline const char *gstr(uint64_t a) { return (const char *)gptr(a); }

// ---- loader.c ----
typedef struct ExecImage { uint64_t entry; uint64_t sp; } ExecImage;
int load_check(Proc *p, const char *guest_path);
int load_exec(Proc *p, Mm *m, const char *guest_path, char *const argv[], char *const envp[], ExecImage *out);
