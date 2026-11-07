// onirun: runs Android (Linux/arm64, bionic) programs on Apple arm64 hosts (macOS now, iOS later)
// without a virtual machine. Android code is translated in blocks into a JIT cache. Every guest
// process lives in its own window of host address space: guest address g is host address
// base + g, translated code keeps base in x16 and adds it to every memory access, so a fork is a
// copy-on-write copy of the window into a new one and many Android processes share one host
// process (iOS has no fork). System calls are served by a Linux personality layer.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Guest CPU state. The assembly in cpu.S uses these offsets.
typedef struct GuestCtx {
    uint64_t x[31];        //   0: x0..x30 (x16, x17 and x18 live only here; the host uses them)
    uint64_t sp;           // 248: guest address
    uint64_t pc;           // 256
    uint64_t nzcv;         // 264
    uint64_t fpcr;         // 272
    uint64_t fpsr;         // 280
    uint64_t tpidr;        // 288: guest TPIDR_EL0
    uint64_t exit_arg;     // 296
    uint64_t exit_kind;    // 304
    uint64_t base;         // 312: host address of guest address 0 (this process's window)
    uint8_t q[32][16];     // 320: v0..v31
    uint64_t host_sp;      // 832
    uint64_t host_x[12];   // 840: x19..x30
    uint64_t host_d[8];    // 936: d8..d15
    // ---- C-only from here ----
    struct Proc *proc;
    struct TrSpace *space;         // translations for this address space
    uint64_t tid;
    uint64_t clear_child_tid;
    uint64_t sigmask;
    uint64_t altstack_sp, altstack_size;   // sigaltstack, per thread
    volatile uint64_t waitset;     // signals this thread is blocked in rt_sigtimedwait for
    volatile uint64_t barrier_ack; // membarrier: generation this thread has acknowledged
    volatile uint64_t pending;     // signals sent to this thread (tgkill)
    volatile int kill;             // set by another thread: leave now (exit_group, exec, fatal signal)
    int fault_sig;                 // Linux signal when translated code faulted
    int fault_code;                // its si_code
    uint64_t fault_addr;           // guest address of that fault
    void *host_thread;             // pthread_t
    void *altstack;
    char name[16];
    struct GuestCtx *next_thread;
    uint64_t magic;                // CTX_MAGIC while the context is live (catches double frees)
    struct SigExtra { int32_t code, timerid; uint64_t value; } sigx[65];   // POSIX timer details of pending signals
} GuestCtx;
#define CTX_MAGIC 0x6f6e697275e7c7c7ull

#define CTX_X(n) ((n) * 8)
#define CTX_SP 248
#define CTX_PC 256
#define CTX_TPIDR 288
#define CTX_EXIT_ARG 296
#define CTX_EXIT_KIND 304
#define CTX_BASE 312

enum { EXIT_DIRECT = 1, EXIT_INDIRECT = 2, EXIT_SYSCALL = 3, EXIT_EMULATE = 4, EXIT_BRK = 5, EXIT_KILLED = 6 };

// A process window is WIN_SIZE bytes of host address space. Just below it sits the indirect-branch
// table that translated code probes through x16.
#define WIN_BITS 36
#define WIN_SIZE (1ull << WIN_BITS)
#define IB_BITS 14
#define IB_SIZE ((1ull << IB_BITS) * 16)

// cpu.S
void onx_enter(GuestCtx *ctx, void *host_code);
void onx_exit_common(void);     // guest x0/x1 already saved in the context
void onx_exit_spilled(void);    // guest x0/x1 at [sp-32]
void onx_abandon(void);        // x0 = ctx: leave translated code without saving guest state
extern uint64_t onx_tsd_off;   // byte offset of our context slot in the Darwin TSD

// translate.c
struct TrSpace;
void tr_init(void);
struct TrSpace *tr_space_new(uint8_t *base);
void tr_invalidate(struct TrSpace *s, uint64_t start, uint64_t len);        // code in the range changed
void tr_invalidate_later(struct TrSpace *s, uint64_t start, uint64_t len);  // ... applied at the next ISB
void tr_space_free(struct TrSpace *s);
__attribute__((noreturn)) void tr_run(GuestCtx *ctx);
void tr_leave(void);                       // the calling thread stops being a guest thread
extern __thread GuestCtx *tr_cur;

// syscall.c / proc.c
void sys_init(const char *rootfs);
void display_main(void (*fn)(void *), void *arg);   // display.m: --window; runs fn on a thread, AppKit on this one
__attribute__((noreturn)) void sys_main(const char *path, char **argv, char **envp, const char **sockets);   // path NULL: init mode
void sys_dispatch(GuestCtx *c);
void sys_deliver_signals(GuestCtx *c);          // may not return (thread leaves)
void sys_abandoned(GuestCtx *c);                // a thread pulled out of translated code
int  sys_emulate(GuestCtx *c, uint64_t pc, uint32_t insn); // 0 = handled
uint64_t sys_code_end(GuestCtx *c, uint64_t pc);           // end of the executable mapping at pc, 0 = none
void sys_fault(GuestCtx *c, int sig, int code, uint64_t addr);   // deliver or die

__attribute__((noreturn, format(printf, 1, 2))) void die(const char *fmt, ...);
extern int g_verbose;
#define LOGV(...) do { if (g_verbose) fprintf(stderr, __VA_ARGS__); } while (0)
