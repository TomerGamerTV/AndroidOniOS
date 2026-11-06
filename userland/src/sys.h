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
int64_t mm_map_host(Mm *m, uint64_t addr, uint64_t len, int prot, void *host, const char *path);
uint64_t mm_exec_end(Mm *m, uint64_t pc);
void mm_symbolize(Mm *m, uint64_t addr, char *out, size_t n);
bool mm_readable(Mm *m, uint64_t addr, uint64_t len);

// ---- file-descriptor objects (vfs.c) ----
// Things that aren't plain host files: eventfds, epoll sets, sockets, ashmem, binder. Each is
// attached to a host fd (usually a pipe's read end, so poll() works on it; obj_set_ready() puts a
// byte in the pipe or drains it) and the host fd's dups share the object.
typedef struct FdObj FdObj;
typedef struct FdOps {
    const char *name;
    int64_t (*read)(FdObj *o, GuestCtx *c, void *buf, size_t n, bool nonblock);
    int64_t (*write)(FdObj *o, GuestCtx *c, const void *buf, size_t n, bool nonblock);
    int64_t (*ioctl)(FdObj *o, GuestCtx *c, uint64_t req, uint64_t arg);
    void (*mapped)(FdObj *o, GuestCtx *c, uint64_t gaddr, uint64_t len, int prot, uint64_t off);
    void (*release)(FdObj *o);
    // Optional: the object maps itself (GPU buffers). Returns the guest address or -errno.
    int64_t (*mmap)(FdObj *o, GuestCtx *c, uint64_t addr, uint64_t len, int prot, int lflags, uint64_t off);
} FdOps;
struct FdObj {
    const FdOps *ops;
    int refs;
    int rd, wr;               // readiness pipe (-1 if the host fd is something else)
    int mmap_fd;              // host fd to mmap for this object, -1 = not mappable
    bool map_shared;          // map it MAP_SHARED even if the guest asked for private
    uint64_t size;            // reported by fstat/lseek(SEEK_END) when nonzero (dma-bufs)
    bool ready;
    pthread_mutex_t lock;
    void *data;
    uint64_t ino;             // the readiness pipe's inode: identifies the object behind any dup of rd
    struct FdObj *next_reg;   // registry of live piped objects (obj_by_fd)
};
FdObj *obj_new(const FdOps *ops, void *data, bool piped);   // refs = 1; piped: creates rd/wr
void obj_get(FdObj *o);
FdObj *obj_by_fd(int host);                                // the object a passed/dup'd host fd refers to, or NULL (borrowed)
void obj_put(FdObj *o);
void obj_set_ready(FdObj *o, bool ready);                  // o->lock held by the caller
int  obj_wait(int host, int events, bool nonblock);        // 0, -EAGAIN or -EINTR

int  vfs_open_dev(struct Proc *p, const char *guest_path, int lflags);   // guest fd, -errno, or VFS_PASS
#define VFS_PASS INT64_MIN                                 // "not an object: use the host fd"
int64_t vfs_read(GuestCtx *c, int host, void *buf, size_t n);
int64_t vfs_write(GuestCtx *c, int host, const void *buf, size_t n);
int64_t vfs_ioctl(GuestCtx *c, int host, uint64_t req, uint64_t arg);
int64_t vfs_eventfd(GuestCtx *c, uint64_t count, int lflags);
int64_t vfs_memfd(GuestCtx *c, const char *name, int lflags);
int64_t vfs_inotify_init(GuestCtx *c, int lflags);
int64_t vfs_memfd_seals(int host, int cmd, uint64_t arg);
int64_t vfs_timerfd_create(GuestCtx *c, int clockid, int lflags);
int64_t vfs_timerfd_settime(GuestCtx *c, int gfd, int lflags, uint64_t newv, uint64_t oldv);
int64_t vfs_timerfd_gettime(GuestCtx *c, int gfd, uint64_t curv);
int64_t vfs_epoll_create(GuestCtx *c, int lflags);
int64_t vfs_epoll_ctl(GuestCtx *c, int epfd, int op, int fd, uint64_t event);
int64_t vfs_epoll_wait(GuestCtx *c, int epfd, uint64_t events, int maxevents, int64_t timeout_ms, uint64_t sigmask);
void vfs_fd_closed(struct Proc *p, int gfd);               // drops the fd from the process's epoll sets
int  host_tmpfile(void);                                   // unlinked temporary file

// ---- unix sockets (sock.c) ----
void sock_init(void);
int64_t sock_call(GuestCtx *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
// A service implemented by onirun itself, reachable at a guest socket path.
typedef void (*BuiltinAccept)(int host_fd, int peer_pid, void *arg);
int  sock_builtin_stream(const char *guest_path, BuiltinAccept accept, void *arg);
int  sock_builtin_dgram(const char *guest_path);           // returns the host fd that receives the datagrams
int  sock_listen_for(struct Proc *p, const char *name);    // init-style socket for the first process: guest fd

// ---- /proc and /sys (procfs.c) ----
int  procfs_open(struct Proc *p, const char *guest_path, int lflags);   // guest fd, -errno, or 1 = not ours

// ---- init-lite (init.c) ----
bool init_start(const char *table_host_path, char **envp);
int  init_control(const char *what, const char *arg);      // ctl.<what>=<arg>

// ---- system properties (props.c) ----
void props_init(void);
int  props_set(const char *name, const char *value);     // 0 or a PROP_ERROR_* code

// ---- virtio-gpu over rutabaga/gfxstream (virtgpu.c) ----
int  gpu_open(struct Proc *p, bool card, int lflags);     // /dev/dri/renderD128 (render) or card0 (KMS)

// ---- binder (binder.c) ----
int  binder_open(struct Proc *p, const char *context, int lflags);   // guest fd or -errno
int  goldfish_pipe_open(struct Proc *p, const char *path, int lflags);   // goldfish.c: qemud pipes
void bpf_init(void);                                                     // bpf.c: pinned maps and programs
int64_t bpf_syscall(GuestCtx *c, int cmd, uint8_t *attr, uint64_t size);  // host errno

// ---- processes (proc.c) ----
#define MAX_FDS 1024
typedef struct FdEnt { int host; bool cloexec; } FdEnt;

enum { P_RUNNING, P_EXITING, P_ZOMBIE };

typedef struct Proc {
    int pid, ppid, pgid, sid;
    uint32_t uid, gid;        // real ids (getuid); zygote preloads with euid 9999 and ruid 0
    uint32_t euid, egid;      // effective ids: credentials seen by binder, sockets and files
    uint32_t caps[6];         // capget/capset v3 data: effective, permitted, inheritable (low, high)
    uint32_t groups[32];
    int ngroups;
    int nepoll;               // epoll fds open, so close() knows whether to scan them
    Mm *mm;
    FdEnt fd[MAX_FDS];        // guest fd -> host fd (-1 = closed)
    char cwd[1024];           // guest path
    char exe[1024];
    char cmdline[512];        // argv, NUL-separated, for /proc/<pid>/cmdline
    size_t cmdline_len;
    uint64_t stack_start;     // initial sp, reported as startstack in /proc/<pid>/stat
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
int  fd_install_obj(Proc *p, int host, FdObj *o, bool cloexec);   // takes the caller's reference to o
int  fd_close(Proc *p, int gfd);
int  fd_dup(Proc *p, int old, int target, bool exact, bool cloexec);
int  fd_cloexec(Proc *p, int gfd, int set);           // set < 0: query
void fd_set_path(int host, const char *guest_path);
bool fd_get_path(int host, char *out, size_t n);
FdObj *fd_obj(int host);                              // borrowed; NULL for plain files
void fd_attach_obj(int host, FdObj *o);               // takes a reference
void dir_forget(int host);
extern uint32_t g_uid;                                // the guest's uid/gid (the shell user)

uint64_t proc_clone(GuestCtx *c, uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t tls, uint64_t ctid);
uint64_t proc_execve(GuestCtx *c, const char *path, uint64_t argv, uint64_t envp);
__attribute__((noreturn)) void proc_exit_thread(GuestCtx *c, int status);
__attribute__((noreturn)) void proc_exit_group(GuestCtx *c, int status);
uint64_t proc_wait4(GuestCtx *c, int64_t pid, uint64_t status, int options, uint64_t rusage);
uint64_t proc_kill(GuestCtx *c, int64_t pid, int sig);
uint64_t proc_tgkill(GuestCtx *c, int64_t tgid, int64_t tid, int sig);
uint64_t proc_sigsuspend(GuestCtx *c, uint64_t mask);   // sets x0 itself
int64_t proc_sigtimedwait(GuestCtx *c, uint64_t set, uint64_t info, const struct timespec *timeout);
void proc_sigreturn(GuestCtx *c);
int  proc_deliverable(GuestCtx *c);                   // a pending signal with a handler, or 0
void proc_raise(GuestCtx *c, int sig);                // thread-directed, from the syscall layer
uint64_t proc_setpgid(GuestCtx *c, int64_t pid, int64_t pgid);
uint64_t proc_getpgid(GuestCtx *c, int64_t pid);
uint64_t proc_setsid(GuestCtx *c);
void proc_backtrace(GuestCtx *c);
Proc *proc_start(const char *path, char **argv, char **envp, const char **sockets);
Proc *proc_spawn(const char *path, char **argv, char **envp, const char **sockets);   // like init starting a service; NULL on failure
void proc_kill_locked(Proc *p, int sig);
const char *proc_secontext(Proc *p);                 // SELinux context we claim for p
Proc *proc_find(int pid);                            // g_lock held
Proc *proc_list(void);                               // g_lock held
void proc_forward_host_signal(int lsig);
void proc_dump_all(void);
void binder_dump(void);                             // SIGINFO: print every guest thread
uint64_t proc_membarrier(GuestCtx *c, int cmd);

// ---- paths and errors (syscall.c) ----
extern char g_rootfs[1024];
int host_path(Proc *p, const char *guest, char *out, size_t outlen, bool follow_last);
int lx_errno(int e);
int lx_sig(int host_sig);
#define ERR(e) ((uint64_t)(int64_t)-(e))
#define HOSTRET(expr) ({ __typeof__(expr) _r = (expr); _r < 0 ? ERR(lx_errno(errno)) : (uint64_t)_r; })

// Guest pointers of the calling thread's process. The top byte is ignored (TBI), as Linux does for
// addresses passed to system calls.
#define UNTAG(a) ((a) & 0x00ffffffffffffffull)
static inline void *gptr(uint64_t a) { return a ? (void *)(uintptr_t)(tr_cur->base + UNTAG(a)) : NULL; }
static inline const char *gstr(uint64_t a) { return (const char *)gptr(a); }

// ---- loader.c ----
typedef struct ExecImage { uint64_t entry; uint64_t sp; } ExecImage;
int load_check(Proc *p, const char *guest_path);
int load_exec(Proc *p, Mm *m, const char *guest_path, char *const argv[], char *const envp[], ExecImage *out);
