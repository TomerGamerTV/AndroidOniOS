// onirun: runs Android (Linux/arm64, bionic) programs on Apple arm64 hosts (macOS now, iOS later)
// without a virtual machine. Android code is translated in blocks into a JIT cache: most
// instructions are copied unchanged, while branches, PC-relative loads, system calls, the TLS
// register (TPIDR_EL0) and x18 are rewritten, because XNU overwrites TPIDR_EL0 with the CPU number
// and zeroes x18 on every context switch. System calls are served by a Linux personality layer.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Guest CPU state. The assembly in cpu.S uses these offsets.
typedef struct GuestCtx {
    uint64_t x[31];        //   0: x0..x30 (x18 lives only here: it is virtualized)
    uint64_t sp;           // 248
    uint64_t pc;           // 256
    uint64_t nzcv;         // 264
    uint64_t fpcr;         // 272
    uint64_t fpsr;         // 280
    uint64_t tpidr;        // 288: guest TPIDR_EL0
    uint64_t exit_arg;     // 296
    uint64_t exit_kind;    // 304
    uint64_t pad0;         // 312
    uint8_t q[32][16];     // 320: v0..v31
    uint64_t host_sp;      // 832
    uint64_t host_x[12];   // 840: x19..x30
    uint64_t host_d[8];    // 936: d8..d15
    // ---- C-only from here ----
    uint64_t tid;
    uint64_t clear_child_tid;
    uint64_t sigmask;
    volatile uint64_t pending;     // signals sent to this thread (tgkill)
    void *host_thread;             // pthread_t
    struct GuestCtx *next_thread;
} GuestCtx;

#define CTX_X(n) ((n) * 8)
#define CTX_SP 248
#define CTX_PC 256
#define CTX_TPIDR 288
#define CTX_EXIT_ARG 296

enum { EXIT_DIRECT = 1, EXIT_INDIRECT = 2, EXIT_SYSCALL = 3, EXIT_EMULATE = 4, EXIT_BRK = 5 };

// cpu.S
void onx_enter(GuestCtx *ctx, void *host_code);
void onx_exit_common(void);
extern uint64_t onx_tsd_off;   // byte offset of our context slot in the Darwin TSD

// translate.c
void tr_init(void);
void tr_after_fork(void);
void tr_run(GuestCtx *ctx);       // runs the guest thread until it exits
GuestCtx *tr_current(void);

// syscall.c
void sys_init(const char *rootfs);
void sys_register_main_thread(GuestCtx *c);
void sys_dispatch(GuestCtx *c);
void sys_deliver_signals(GuestCtx *c);
void sys_note_mapping(uint64_t start, uint64_t len, uint64_t off, const char *path);
int  sys_emulate(GuestCtx *c, uint64_t pc, uint32_t insn); // 0 = handled
int  host_path(const char *guest, int dirfd, char *out, size_t outlen, bool follow_last);
extern char g_rootfs[1024];

// loader.c
typedef struct ExecImage { uint64_t entry; uint64_t sp; } ExecImage;
int  load_exec(const char *guest_path, char *const argv[], char *const envp[], ExecImage *out);

__attribute__((noreturn, format(printf, 1, 2))) void die(const char *fmt, ...);
extern int g_verbose;
#define LOGV(...) do { if (g_verbose) fprintf(stderr, __VA_ARGS__); } while (0)
