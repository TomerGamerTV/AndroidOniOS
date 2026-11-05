// Block translator: guest arm64 code -> host arm64 code in a JIT cache, plus the dispatch loop.
#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "onx.h"

_Static_assert(offsetof(GuestCtx, sp) == 248, "ctx layout");
_Static_assert(offsetof(GuestCtx, tpidr) == 288, "ctx layout");
_Static_assert(offsetof(GuestCtx, exit_arg) == 296, "ctx layout");
_Static_assert(offsetof(GuestCtx, q) == 320, "ctx layout");
_Static_assert(offsetof(GuestCtx, host_sp) == 832, "ctx layout");
_Static_assert(offsetof(GuestCtx, host_d) == 936, "ctx layout");

#define CACHE_SIZE (256u << 20)
#define MAX_BLOCK_INSNS 256
#define TRAMP_WORDS 6

static uint32_t *cache, *cache_ptr, *cache_end, *cache_tramp;
static pthread_key_t ctx_key;
static pthread_mutex_t tr_lock = PTHREAD_MUTEX_INITIALIZER;

// ---- block map ----
typedef struct Block {
    uint64_t pc;
    uint32_t *host;
    uint32_t *host_end;
    struct Block *next;
} Block;
#define HBITS 16
static Block *htab[1 << HBITS];
static inline unsigned hidx(uint64_t pc) { return (unsigned)((pc >> 2) * 0x9E3779B1u) >> (32 - HBITS); }

// Exit records give the dispatcher the target of a direct branch and the stub to patch.
typedef struct ExitRec { uint64_t target; uint32_t *patch; struct ExitRec *next; } ExitRec;
typedef struct EmuRec { uint64_t pc; uint32_t insn; struct EmuRec *next; } EmuRec;
static ExitRec *exit_recs;

// Indirect-branch lookup table, probed inline by translated code: {guest pc, host block entry}.
#define IB_BITS 14
typedef struct { uint64_t pc; uint32_t *host; } IbEntry;
static IbEntry ib_table[1 << IB_BITS] __attribute__((aligned(16)));
static EmuRec *emu_recs;

static Block *lookup(uint64_t pc) {
    for (Block *b = htab[hidx(pc)]; b; b = b->next)
        if (b->pc == pc) return b;
    return NULL;
}

static int g_flushes;
static void cache_flush_all(void) {
    if (++g_flushes > 1) fprintf(stderr, "onirun: warning: code cache flush #%d\n", g_flushes);
    for (unsigned i = 0; i < (1u << HBITS); i++) {
        Block *b = htab[i];
        while (b) { Block *n = b->next; free(b); b = n; }
        htab[i] = NULL;
    }
    while (exit_recs) { ExitRec *n = exit_recs->next; free(exit_recs); exit_recs = n; }
    while (emu_recs) { EmuRec *n = emu_recs->next; free(emu_recs); emu_recs = n; }
    cache_ptr = cache + TRAMP_WORDS;
    memset(ib_table, 0, sizeof ib_table);
    LOGV("onirun: code cache flushed\n");
}

static void jit_write(bool on) { pthread_jit_write_protect_np(!on); }

// ---- instruction encoders ----
static inline void emit(uint32_t w) { *cache_ptr++ = w; }
#define STP_X16_X17_SPM16 0xa93f47f0u
#define LDP_X16_X17_SPM16 0xa97f47f0u
static inline uint32_t movz(int rd, int hw, unsigned imm) { return 0xd2800000u | (hw << 21) | ((imm & 0xffff) << 5) | rd; }
static inline uint32_t movk(int rd, int hw, unsigned imm) { return 0xf2800000u | (hw << 21) | ((imm & 0xffff) << 5) | rd; }
static inline uint32_t b_to(uint32_t *from, uint32_t *to) { return 0x14000000u | ((uint32_t)(to - from) & 0x3ffffff); }
static inline uint32_t mov_reg(int rd, int rm) { return 0xaa0003e0u | (rm << 16) | rd; }
static inline uint32_t mrs_tpidrro(int rt) { return 0xd53bd060u | rt; }
static inline uint32_t and_not7(int rd, int rn) { return 0x927df000u | (rn << 5) | rd; }
static inline uint32_t ldr_imm(int rt, int rn, unsigned off) { return 0xf9400000u | ((off / 8) << 10) | (rn << 5) | rt; }
static inline uint32_t str_imm(int rt, int rn, unsigned off) { return 0xf9000000u | ((off / 8) << 10) | (rn << 5) | rt; }
static inline uint32_t stp_sp(int a, int b, int off) { return 0xa9000000u | (((off / 8) & 0x7f) << 15) | (b << 10) | (31 << 5) | a; }
static inline uint32_t ldp_sp(int a, int b, int off) { return 0xa9400000u | (((off / 8) & 0x7f) << 15) | (b << 10) | (31 << 5) | a; }

static void emit_mov64(int rd, uint64_t v) {
    emit(movz(rd, 0, v & 0xffff));
    for (int hw = 1; hw < 4; hw++) {
        unsigned part = (v >> (16 * hw)) & 0xffff;
        if (part) emit(movk(rd, hw, part));
    }
}

// Loads this thread's GuestCtx pointer into rd (no other register used).
static void emit_get_ctx(int rd) {
    emit(mrs_tpidrro(rd));
    emit(and_not7(rd, rd));
    emit(ldr_imm(rd, rd, (unsigned)onx_tsd_off));
}

static void emit_exit(uint64_t arg, int kind) {
    emit(STP_X16_X17_SPM16);
    emit_mov64(16, arg);
    emit(movz(17, 0, kind));
    uint32_t *at = cache_ptr;
    emit(b_to(at, cache_tramp));
}

static void emit_direct_exit(uint64_t target) {
    ExitRec *r = calloc(1, sizeof(*r));
    r->target = target;
    r->patch = cache_ptr;
    r->next = exit_recs;
    exit_recs = r;
    emit_exit((uint64_t)(uintptr_t)r, EXIT_DIRECT);
}

static void emit_emulate(uint64_t pc, uint32_t insn) {
    EmuRec *r = calloc(1, sizeof(*r));
    r->pc = pc;
    r->insn = insn;
    r->next = emu_recs;
    emu_recs = r;
    emit_exit((uint64_t)(uintptr_t)r, EXIT_EMULATE);
}

static inline int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

// ---- x18 virtualization ----
// Register-field masks for instruction classes that may name x18; 0 = unknown class.
enum { F_RD = 1, F_RN = 2, F_RM = 4, F_RT2 = 8 };
static int gpr_fields(uint32_t i) {
    if ((i & 0x1f000000) == 0x11000000) return F_RD | F_RN;                  // add/sub imm
    if ((i & 0x1f800000) == 0x12000000) return F_RD | F_RN;                  // logical imm
    if ((i & 0x1f800000) == 0x12800000) return F_RD;                         // move wide
    if ((i & 0x1f800000) == 0x13000000) return F_RD | F_RN;                  // bitfield
    if ((i & 0x1f000000) == 0x0a000000) return F_RD | F_RN | F_RM;           // logical shifted reg
    if ((i & 0x1f000000) == 0x0b000000) return F_RD | F_RN | F_RM;           // add/sub shifted/ext
    if ((i & 0x3a000000) == 0x28000000 && !(i & (1u << 26)))                // load/store pair (GPR)
        return F_RD | F_RN | F_RT2;
    if ((i & 0x3b000000) == 0x39000000 && !(i & (1u << 26)))                // ld/st unsigned imm
        return F_RD | F_RN;
    if ((i & 0x3b200000) == 0x38000000 && !(i & (1u << 26)))                // ld/st imm9 forms
        return F_RD | F_RN;
    if ((i & 0x3b200c00) == 0x38200800 && !(i & (1u << 26)))                // ld/st register offset
        return F_RD | F_RN | F_RM;
    return 0;
}
static bool field_is(uint32_t i, int f, int reg) {
    switch (f) {
    case F_RD: return (i & 31) == (unsigned)reg;
    case F_RN: return ((i >> 5) & 31) == (unsigned)reg;
    case F_RM: return ((i >> 16) & 31) == (unsigned)reg;
    case F_RT2: return ((i >> 10) & 31) == (unsigned)reg;
    }
    return false;
}
static uint32_t field_set(uint32_t i, int f, int reg) {
    switch (f) {
    case F_RD: return (i & ~31u) | reg;
    case F_RN: return (i & ~(31u << 5)) | (reg << 5);
    case F_RM: return (i & ~(31u << 16)) | (reg << 16);
    case F_RT2: return (i & ~(31u << 10)) | (reg << 10);
    }
    return i;
}

// Returns true if it emitted a rewritten instruction.
static bool rewrite_x18(uint32_t insn, uint64_t pc) {
    int fields = gpr_fields(insn);
    if (!fields) return false;
    bool uses = false;
    for (int f = 1; f <= F_RT2; f <<= 1)
        if ((fields & f) && field_is(insn, f, 18)) uses = true;
    if (!uses) return false;
    // Pick two scratch registers the instruction doesn't name.
    int s = -1, c = -1;
    for (int r = 9; r <= 17 && (s < 0 || c < 0); r++) {
        bool used = false;
        for (int f = 1; f <= F_RT2; f <<= 1)
            if ((fields & f) && field_is(insn, f, r)) used = true;
        if (used) continue;
        if (s < 0) s = r; else c = r;
    }
    if (((insn >> 5) & 31) == 31 && (fields & F_RN)) {
        // sp-based with possible writeback would break our sp-relative spill; not seen yet.
        bool writeback = ((insn & 0x3a000000) == 0x28000000 && ((insn >> 23) & 3) != 2) ||
                         ((insn & 0x3b200000) == 0x38000000 && ((insn >> 10) & 1));
        if (writeback) die("onirun: unsupported x18 instruction %08x at %llx", insn, (unsigned long long)pc);
    }
    uint32_t out = insn;
    for (int f = 1; f <= F_RT2; f <<= 1)
        if ((fields & f) && field_is(insn, f, 18)) out = field_set(out, f, s);
    emit(stp_sp(s, c, -32));
    emit_get_ctx(c);
    emit(ldr_imm(s, c, CTX_X(18)));
    emit(out);
    emit(str_imm(s, c, CTX_X(18)));
    emit(ldp_sp(s, c, -32));
    return true;
}

// ---- system registers ----
enum { SYS_NATIVE, SYS_DONE, SYS_EXIT };
static int rewrite_sys(uint32_t insn, uint64_t pc) {
    if ((insn & 0xffffffe0) == 0xd53bd040) {          // mrs xT, tpidr_el0
        int t = insn & 31;
        if (t != 31) {
            emit_get_ctx(t);
            emit(ldr_imm(t, t, CTX_TPIDR));
        }
        return SYS_DONE;
    }
    if ((insn & 0xffffffe0) == 0xd51bd040) {          // msr tpidr_el0, xT
        int t = insn & 31;
        int s = (t == 16) ? 17 : 16;
        emit(STP_X16_X17_SPM16);
        emit_get_ctx(s);
        emit(str_imm(t, s, CTX_TPIDR));               // t == 31 stores xzr
        emit(LDP_X16_X17_SPM16);
        return SYS_DONE;
    }
    unsigned op1 = (insn >> 16) & 7, crn = (insn >> 12) & 15;
    if ((insn & 0xfff00000) == 0xd5300000) {          // other mrs
        // EL0-readable registers run natively: CTR, DCZID, NZCV, FPCR, FPSR, counters.
        if (op1 == 3 && (crn == 0 || crn == 4 || crn == 14)) return SYS_NATIVE;
        emit_emulate(pc, insn);                       // MIDR/ID registers and the like
        return SYS_EXIT;
    }
    if ((insn & 0xfff00000) == 0xd5100000) {          // msr (register)
        if (op1 == 3 && crn == 4) return SYS_NATIVE;  // NZCV/FPCR/FPSR
        emit_emulate(pc, insn);
        return SYS_EXIT;
    }
    return SYS_NATIVE;                                // barriers, hints, DC/IC
}

// ---- block translation ----
static Block *translate(uint64_t pc) {
    if ((size_t)(cache_end - cache_ptr) < MAX_BLOCK_INSNS * 24 + 64) cache_flush_all();
    uint32_t *start = cache_ptr;
    uint64_t gpc = pc;
    emit(LDP_X16_X17_SPM16);                           // block entry from the dispatcher
    for (int n = 0;; n++, gpc += 4) {
        if (n == MAX_BLOCK_INSNS) { emit_direct_exit(gpc); break; }
        uint32_t insn = *(const uint32_t *)(uintptr_t)gpc;

        if ((insn & 0x7c000000) == 0x14000000) {       // B / BL
            uint64_t target = gpc + (sext(insn & 0x3ffffff, 26) << 2);
            if (insn & 0x80000000) emit_mov64(30, gpc + 4);
            emit_direct_exit(target);
            break;
        }
        if ((insn & 0xff000010) == 0x54000000 ||        // B.cond
            (insn & 0x7e000000) == 0x34000000 ||        // CBZ/CBNZ
            (insn & 0x7e000000) == 0x36000000) {        // TBZ/TBNZ
            bool tb = (insn & 0x7e000000) == 0x36000000;
            int64_t off = tb ? sext((insn >> 5) & 0x3fff, 14) : sext((insn >> 5) & 0x7ffff, 19);
            uint64_t target = gpc + (off << 2);
            uint32_t *br = cache_ptr;
            emit(0);                                    // patched below
            emit_direct_exit(gpc + 4);
            int64_t rel = cache_ptr - br;
            *br = tb ? (insn & ~(0x3fffu << 5)) | ((uint32_t)(rel & 0x3fff) << 5)
                     : (insn & ~(0x7ffffu << 5)) | ((uint32_t)(rel & 0x7ffff) << 5);
            emit_direct_exit(target);
            break;
        }
        if ((insn & 0xff9ffc1f) == 0xd61f0000 ||        // BR / BLR / RET
            (insn & 0xfffffc1f) == 0xd65f0000 ||
            insn == 0xd65f0bff || insn == 0xd65f0fff) { // RETAA / RETAB
            int op = (insn >> 21) & 3;                  // 0 BR, 1 BLR, 2 RET
            int n = (insn >> 5) & 31;
            if (insn == 0xd65f0bff) { emit(0xd50323bf); n = 30; op = 2; }  // autiasp
            if (insn == 0xd65f0fff) { emit(0xd50323ff); n = 30; op = 2; }  // autibsp
            // Inline probe of ib_table; flags are untouched (no compares). On a hit, jump to the
            // block's entry, which reloads guest x16/x17 from below sp.
            emit(STP_X16_X17_SPM16);
            emit(mov_reg(16, n));
            if (op == 1) emit_mov64(30, gpc + 4);
            emit(0xa93e07e0);                               // stp x0, x1, [sp, #-32]
            emit_mov64(17, (uint64_t)(uintptr_t)ib_table);
            emit(0xd3400000u | (2u << 16) | ((2u + IB_BITS - 1) << 10) | (16u << 5)); // ubfx x0, x16, #2, #IB_BITS
            emit(0x8b001231);                               // add x17, x17, x0, lsl #4
            emit(0xa9400620);                               // ldp x0, x1, [x17]
            emit(0xca100000);                               // eor x0, x0, x16
            emit(0xb5000000u | (4u << 5));                  // cbnz x0, miss (+4 insns)
            emit(0xaa0103f0);                               // mov x16, x1
            emit(0xa97e07e0);                               // ldp x0, x1, [sp, #-32]
            emit(0xd61f0200);                               // br x16
            emit(0xa97e07e0);                               // miss: ldp x0, x1, [sp, #-32]
            emit(movz(17, 0, EXIT_INDIRECT));
            uint32_t *at = cache_ptr;
            emit(b_to(at, cache_tramp));
            break;
        }
        if ((insn & 0xfe1f0000) == 0xd61f0000) {        // pointer-auth branch forms (BRAA...)
            emit_emulate(gpc, insn);
            break;
        }
        if ((insn & 0xffe0001f) == 0xd4000001) {        // SVC #0
            emit_exit(gpc + 4, EXIT_SYSCALL);
            break;
        }
        if ((insn & 0xffe0001f) == 0xd4200000) {        // BRK
            emit_exit(gpc, EXIT_BRK);
            break;
        }
        if ((insn & 0x1f000000) == 0x10000000) {        // ADR / ADRP
            int rd = insn & 31;
            uint64_t imm = ((insn >> 5) & 0x7ffff) << 2 | ((insn >> 29) & 3);
            uint64_t v = (insn & 0x80000000) ? (gpc & ~0xfffull) + (sext(imm, 21) << 12)
                                             : gpc + sext(imm, 21);
            if (rd != 31) emit_mov64(rd, v);
            continue;
        }
        if ((insn & 0x3b000000) == 0x18000000) {        // LDR (literal)
            int t = insn & 31, opc = insn >> 30;
            uint64_t addr = gpc + (sext((insn >> 5) & 0x7ffff, 19) << 2);
            if (!(insn & (1u << 26))) {
                if (opc == 3) continue;                 // PRFM
                if (t == 31) continue;
                emit_mov64(t, addr);
                emit(opc == 0 ? 0xb9400000u | (t << 5) | t
                   : opc == 1 ? 0xf9400000u | (t << 5) | t
                              : 0xb9800000u | (t << 5) | t);
            } else {
                emit(STP_X16_X17_SPM16);
                emit_mov64(16, addr);
                emit((opc == 0 ? 0xbd400000u : opc == 1 ? 0xfd400000u : 0x3dc00000u) | (16 << 5) | t);
                emit(LDP_X16_X17_SPM16);
            }
            continue;
        }
        if ((insn & 0xffc00000) == 0xd5000000) {        // system instructions
            int r = rewrite_sys(insn, gpc);
            if (r == SYS_EXIT) break;
            if (r == SYS_NATIVE) emit(insn);
            continue;
        }
        if (rewrite_x18(insn, gpc)) continue;
        emit(insn);
    }
    Block *b = calloc(1, sizeof(*b));
    b->pc = pc;
    b->host = start;
    b->host_end = cache_ptr;
    unsigned h = hidx(pc);
    b->next = htab[h];
    htab[h] = b;
    sys_icache_invalidate(start, (size_t)(cache_ptr - start) * 4);
    return b;
}


static uint32_t *get_block(uint64_t pc) {
    Block *b = lookup(pc);
    if (!b) {
        jit_write(true);
        b = translate(pc);
        jit_write(false);
    }
    return b->host;
}

static void chain(ExitRec *r, uint32_t *target_block) {
    uint32_t *dest = target_block + 1;                 // skip the entry reload
    int64_t d = dest - r->patch;
    if (d < -(1 << 25) || d >= (1 << 25)) return;
    jit_write(true);
    *r->patch = b_to(r->patch, dest);
    jit_write(false);
    sys_icache_invalidate(r->patch, 4);
}

GuestCtx *tr_current(void) { return pthread_getspecific(ctx_key); }

void tr_run(GuestCtx *c) {
    pthread_setspecific(ctx_key, c);
    for (;;) {
        sys_deliver_signals(c);
        pthread_mutex_lock(&tr_lock);
        uint32_t *h = get_block(c->pc);
        pthread_mutex_unlock(&tr_lock);
        onx_enter(c, h);
        switch (c->exit_kind) {
        case EXIT_DIRECT: {
            ExitRec *r = (ExitRec *)(uintptr_t)c->exit_arg;
            c->pc = r->target;
            pthread_mutex_lock(&tr_lock);
            uint32_t *t = get_block(r->target);
            chain(r, t);
            pthread_mutex_unlock(&tr_lock);
            break;
        }
        case EXIT_INDIRECT: {
            c->pc = c->exit_arg;
            pthread_mutex_lock(&tr_lock);
            uint32_t *t = get_block(c->pc);
            IbEntry *e = &ib_table[(c->pc >> 2) & ((1 << IB_BITS) - 1)];
            // One 16-byte store (single-copy atomic with FEAT_LSE2) so racing threads never
            // see a pc paired with another block's code.
            __asm__ volatile("stp %0, %1, [%2]" :: "r"(c->pc), "r"(t), "r"(e) : "memory");
            pthread_mutex_unlock(&tr_lock);
            break;
        }
        case EXIT_SYSCALL:
            c->pc = c->exit_arg;
            sys_dispatch(c);
            break;
        case EXIT_EMULATE: {
            EmuRec *r = (EmuRec *)(uintptr_t)c->exit_arg;
            c->pc = r->pc + 4;
            if (sys_emulate(c, r->pc, r->insn))
                die("onirun: cannot emulate instruction %08x at guest pc %llx", r->insn,
                    (unsigned long long)r->pc);
            break;
        }
        case EXIT_BRK:
            die("onirun: guest hit brk at %llx", (unsigned long long)c->exit_arg);
        default:
            die("onirun: bad exit kind %llu", (unsigned long long)c->exit_kind);
        }
    }
}

// ---- crash diagnostics ----
static const Block *find_host(uint64_t host_pc) {
    for (unsigned i = 0; i < (1u << HBITS); i++)
        for (Block *b = htab[i]; b; b = b->next)
            if (host_pc >= (uint64_t)(uintptr_t)b->host && host_pc < (uint64_t)(uintptr_t)b->host_end) return b;
    return NULL;
}

static void fault_handler(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
    const Block *b = find_host(pc);
    char buf[512];
    int n = snprintf(buf, sizeof buf, "onirun: signal %d at host pc %llx (fault addr %p)", sig,
                     (unsigned long long)pc, si->si_addr);
    if (b) n += snprintf(buf + n, sizeof buf - n, " in translated block of guest %llx (+%lld insns)",
                         (unsigned long long)b->pc, (long long)((pc - (uint64_t)(uintptr_t)b->host) / 4));
    n += snprintf(buf + n, sizeof buf - n, "\n");
    write(2, buf, n);
    for (int r = 0; r < 31; r += 4) {
        n = snprintf(buf, sizeof buf, "  x%-2d %016llx x%-2d %016llx x%-2d %016llx x%-2d %016llx\n",
                     r, uc->uc_mcontext->__ss.__x[r], r + 1, uc->uc_mcontext->__ss.__x[r + 1],
                     r + 2, uc->uc_mcontext->__ss.__x[r + 2], r + 3, r + 3 < 29 ? uc->uc_mcontext->__ss.__x[r + 3] : 0);
        write(2, buf, n);
    }
    _exit(128 + sig);
}

static void cache_alloc(void) {
    cache = mmap(NULL, CACHE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (cache == MAP_FAILED) die("onirun: cannot allocate the code cache: %s", strerror(errno));
    cache_end = cache + CACHE_SIZE / 4;
    jit_write(true);
    cache[0] = stp_sp(0, 1, -32);
    cache[1] = 0x58000060;                             // ldr x0, cache+16
    cache[2] = 0xd61f0000;                             // br x0
    cache[3] = 0xd503201f;                             // nop
    cache[4] = (uint32_t)(uintptr_t)onx_exit_common;
    cache[5] = (uint32_t)((uintptr_t)onx_exit_common >> 32);
    cache_ptr = cache + TRAMP_WORDS;
    jit_write(false);
    sys_icache_invalidate(cache, 64);
    cache_tramp = cache;
}

// A forked child can't run code it newly writes into the parent's MAP_JIT pages (they become
// copy-on-write), so it starts over with a fresh cache.
void tr_after_fork(void) {
    uint32_t *old = cache;
    cache_flush_all();
    cache_alloc();
    munmap(old, CACHE_SIZE);
}

void tr_init(void) {
    cache_alloc();
    if (pthread_key_create(&ctx_key, NULL)) die("onirun: pthread_key_create failed");
    onx_tsd_off = (uint64_t)ctx_key * 8;
    // Check that our TSD slot is where the exit path looks for it.
    pthread_setspecific(ctx_key, (void *)0x1234);
    uint64_t base;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
    if (((void **)(uintptr_t)(base & ~7ull))[ctx_key] != (void *)0x1234)
        die("onirun: unexpected Darwin TSD layout");
    pthread_setspecific(ctx_key, NULL);


    stack_t ss = { .ss_sp = malloc(1 << 16), .ss_size = 1 << 16 };
    sigaltstack(&ss, NULL);
    struct sigaction sa = { .sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK };
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
}
