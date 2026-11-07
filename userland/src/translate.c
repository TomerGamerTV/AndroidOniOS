// Block translator: guest arm64 code -> host arm64 code in a JIT cache, plus the dispatch loop.
//
// Register use in translated code: x16 = the process window base, x17 = scratch. The guest's own
// x16, x17 and x18 live in the GuestCtx. Instructions that name them are rewritten to load and store
// those values around a substitute register. Every memory access through a general register gets
// the base added (`ldr x0, [x1, #8]` -> `add x17, x16, x1; ldr x0, [x17, #8]`). Accesses through
// sp run unchanged, because the host sp holds the guest stack pointer as a host address.
// Instructions that read sp as a value subtract the base, and instructions that write it add the
// base.
#include <errno.h>
#include <execinfo.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "onx.h"

_Static_assert(offsetof(GuestCtx, sp) == CTX_SP, "ctx layout");
_Static_assert(offsetof(GuestCtx, tpidr) == CTX_TPIDR, "ctx layout");
_Static_assert(offsetof(GuestCtx, exit_arg) == CTX_EXIT_ARG, "ctx layout");
_Static_assert(offsetof(GuestCtx, exit_kind) == CTX_EXIT_KIND, "ctx layout");
_Static_assert(offsetof(GuestCtx, base) == CTX_BASE, "ctx layout");
_Static_assert(offsetof(GuestCtx, q) == 320, "ctx layout");
_Static_assert(offsetof(GuestCtx, host_sp) == 832, "ctx layout");
_Static_assert(offsetof(GuestCtx, host_d) == 936, "ctx layout");
_Static_assert(IB_SIZE == (0x40u << 12), "the inline probe subtracts #0x40, lsl #12");

// Address space only: chunks are committed as they fill and given back when their process exits.
// shortcut: each process translates its own copy of shared code (boot.oat after a zygote fork);
// sharing translations across forks would cut this and the memory a lot (needed for iOS).
#define CACHE_SIZE ((size_t)3 << 30)
#define CHUNK_WORDS ((1u << 20) / 4)
#define NCHUNKS (CACHE_SIZE / (CHUNK_WORDS * 4))
#define TRAMP_WORDS 12            // two trampolines: exit stubs (x0/x1 in ctx) and the branch probe miss (x0/x1 at [sp-32])
#define MAX_BLOCK_INSNS 256
#define BLOCK_RESERVE (MAX_BLOCK_INSNS * 32 + 64)

static uint32_t *cache;
static bool chunk_used[NCHUNKS];
static pthread_key_t ctx_key;
static pthread_mutex_t tr_lock = PTHREAD_MUTEX_INITIALIZER;
__thread GuestCtx *tr_cur;

// ---- translation spaces (one per address space) ----
// For precise faults: where each guest instruction's host code starts, and which guest registers
// were borrowed (spilled below sp) while a memory instruction naming x16/x17/x18 ran.
typedef struct Borrow { uint16_t start, end; uint8_t nt, t[4]; } Borrow;
typedef struct Block {
    uint64_t pc;
    uint32_t *host;
    uint32_t *host_end;
    uint16_t *imap;           // host word offset of each guest instruction
    int ninsn;
    Borrow *borrows;
    int nborrows;
    bool dead;                // retired by tr_invalidate; its code now jumps to a re-lookup stub
    struct Block *next;
} Block;
static uint16_t cur_imap[MAX_BLOCK_INSNS + 1];
static Borrow cur_borrows[MAX_BLOCK_INSNS];
static int cur_nborrows;
static uint32_t *cur_start;
// Exit records give the dispatcher the target of a direct branch and the stub to patch.
typedef struct ExitRec { uint64_t target; uint32_t *patch; struct ExitRec *next; } ExitRec;
typedef struct EmuRec { uint64_t pc; uint32_t insn; struct EmuRec *next; } EmuRec;
typedef struct { uint64_t pc; uint32_t *host; } IbEntry;   // the window's inline lookup table

#define HBITS 15
typedef struct TrSpace {
    uint8_t *base;
    Block *htab[1 << HBITS];
    ExitRec *exits;
    EmuRec *emus;
    int *chunks, nchunks;
    uint32_t *ptr, *end, *tramp, *tramp_spilled;   // current chunk
    uint64_t inv_lo, inv_hi;  // guest range with pending invalidations (IC IVAU lines), applied at ISB
    struct TrSpace *next;
} TrSpace;
static TrSpace *spaces;
static TrSpace *ts;               // the space being translated into (tr_lock held)

static inline unsigned hidx(uint64_t pc) { return (unsigned)((pc >> 2) * 0x9E3779B1u) >> (32 - HBITS); }

static Block *lookup(TrSpace *s, uint64_t pc) {
    for (Block *b = s->htab[hidx(pc)]; b; b = b->next)
        if (b->pc == pc && !b->dead) return b;
    return NULL;
}

TrSpace *tr_space_new(uint8_t *base) {
    TrSpace *s = calloc(1, sizeof *s);
    s->base = base;
    // Entry 0 must not match guest pc 0 (a call through a null pointer).
    ((IbEntry *)(void *)(base - IB_SIZE))[0].pc = 1;
    pthread_mutex_lock(&tr_lock);
    s->next = spaces;
    spaces = s;
    pthread_mutex_unlock(&tr_lock);
    return s;
}

void tr_space_free(TrSpace *s) {
    pthread_mutex_lock(&tr_lock);
    for (unsigned i = 0; i < (1u << HBITS); i++)
        for (Block *b = s->htab[i]; b;) { Block *n = b->next; free(b->imap); free(b->borrows); free(b); b = n; }
    while (s->exits) { ExitRec *n = s->exits->next; free(s->exits); s->exits = n; }
    while (s->emus) { EmuRec *n = s->emus->next; free(s->emus); s->emus = n; }
    for (int i = 0; i < s->nchunks; i++) {
        chunk_used[s->chunks[i]] = false;
        madvise(cache + (size_t)s->chunks[i] * CHUNK_WORDS, CHUNK_WORDS * 4, MADV_FREE);
    }
    free(s->chunks);
    for (TrSpace **pp = &spaces; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }
    pthread_mutex_unlock(&tr_lock);
    free(s);
}

static void jit_write(bool on) { pthread_jit_write_protect_np(!on); }

// Each 1 MiB chunk starts with a trampoline to onx_exit_common, so exit stubs always reach one.
static void space_grow(TrSpace *s) {
    int i = 0;
    while (i < (int)NCHUNKS && chunk_used[i]) i++;
    if (i == (int)NCHUNKS) die("onirun: code cache full");
    chunk_used[i] = true;
    static int high;
    if (i + 1 > high) { high = i + 1; if (high % 256 == 0 && getenv("ONIRUN_FAULTS")) fprintf(stderr, "onirun: code cache: %d MiB in use\n", high); }
    s->chunks = realloc(s->chunks, (size_t)(s->nchunks + 1) * sizeof(int));
    s->chunks[s->nchunks++] = i;
    uint32_t *c = cache + (size_t)i * CHUNK_WORDS;
    c[0] = 0x58000040;                                 // ldr x0, c+8
    c[1] = 0xd61f0000;                                 // br x0
    c[2] = (uint32_t)(uintptr_t)onx_exit_common;
    c[3] = (uint32_t)((uintptr_t)onx_exit_common >> 32);
    c[4] = 0x58000040;                                 // ldr x0, c+24
    c[5] = 0xd61f0000;                                 // br x0
    c[6] = (uint32_t)(uintptr_t)onx_exit_spilled;
    c[7] = (uint32_t)((uintptr_t)onx_exit_spilled >> 32);
    sys_icache_invalidate(c, TRAMP_WORDS * 4);
    s->tramp = c;
    s->tramp_spilled = c + 4;
    s->ptr = c + TRAMP_WORDS;
    s->end = c + CHUNK_WORDS;
}

// ---- instruction encoders ----
static inline void emit(uint32_t w) { *ts->ptr++ = w; }
static inline uint32_t movz(int rd, int hw, unsigned imm) { return 0xd2800000u | (hw << 21) | ((imm & 0xffff) << 5) | rd; }
static inline uint32_t movk(int rd, int hw, unsigned imm) { return 0xf2800000u | (hw << 21) | ((imm & 0xffff) << 5) | rd; }
static inline uint32_t b_to(uint32_t *from, uint32_t *to) { return 0x14000000u | ((uint32_t)(to - from) & 0x3ffffff); }
static inline uint32_t mov_reg(int rd, int rm) { return 0xaa0003e0u | (rm << 16) | rd; }
static inline uint32_t mrs_tpidrro(int rt) { return 0xd53bd060u | rt; }
static inline uint32_t and_not7(int rd, int rn) { return 0x927df000u | (rn << 5) | rd; }
static inline uint32_t ldr_imm(int rt, int rn, unsigned off) { return 0xf9400000u | ((off / 8) << 10) | (rn << 5) | rt; }
static inline uint32_t str_imm(int rt, int rn, unsigned off) { return 0xf9000000u | ((off / 8) << 10) | (rn << 5) | rt; }
static inline uint32_t stp_x(int a, int b, int n, int off) { return 0xa9000000u | (((off / 8) & 0x7f) << 15) | (b << 10) | (n << 5) | a; }
static inline uint32_t ldp_x(int a, int b, int n, int off) { return 0xa9400000u | (((off / 8) & 0x7f) << 15) | (b << 10) | (n << 5) | a; }
static inline uint32_t add_x(int rd, int rn, int rm) { return 0x8b000000u | (rm << 16) | (rn << 5) | rd; }
static inline uint32_t sub_x(int rd, int rn, int rm) { return 0xcb000000u | (rm << 16) | (rn << 5) | rd; }
static inline uint32_t sub_imm(int rd, int rn, unsigned imm) { return 0xd1000000u | (imm << 10) | (rn << 5) | rd; }
#define SUB_X17_SP_X16 0xcb3063f1u    // sub x17, sp, x16       (guest value of sp)
#define ADD_SP_X16_X17 0x8b31621fu    // add sp, x16, x17       (sp from a guest value)

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

// Guest x0/x1 go straight into the context (never through the guest stack, which may be gone).
static void emit_exit(uint64_t arg, int kind) {
    emit_get_ctx(17);
    emit(stp_x(0, 1, 17, 0));
    emit_mov64(16, arg);
    emit(movz(17, 0, kind));
    uint32_t *at = ts->ptr;
    emit(b_to(at, ts->tramp));
}

static void emit_direct_exit(uint64_t target) {
    ExitRec *r = calloc(1, sizeof(*r));
    r->target = target;
    r->patch = ts->ptr;
    r->next = ts->exits;
    ts->exits = r;
    emit_exit((uint64_t)(uintptr_t)r, EXIT_DIRECT);
}

static void emit_emulate(uint64_t pc, uint32_t insn) {
    EmuRec *r = calloc(1, sizeof(*r));
    r->pc = pc;
    r->insn = insn;
    r->next = ts->emus;
    ts->emus = r;
    emit_exit((uint64_t)(uintptr_t)r, EXIT_EMULATE);
}

static inline int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }
static inline bool stolen_reg(int r) { return r >= 16 && r <= 18; }

// ---- operand decoding ----
// The general-purpose register fields of an instruction, which of them hold the memory base, and
// where register 31 means sp rather than the zero register.
enum { K_OK, K_DROP, K_BAD };
typedef struct Ops {
    int n;
    uint8_t pos[5];
    bool sp[5];
    int base, spsrc, spdst;       // field indexes, -1 = none
    bool wb, wb_known;
    int64_t wb_delta;
    bool casp;
    int kind;
} Ops;
static void opf(Ops *o, int pos, bool sp) { o->pos[o->n] = (uint8_t)pos; o->sp[o->n] = sp; o->n++; }
static inline int fld(uint32_t i, int pos) { return (i >> pos) & 31; }
static inline uint32_t fset(uint32_t i, int pos, int r) { return (i & ~(31u << pos)) | ((uint32_t)r << pos); }
static void opbase(Ops *o) { o->base = o->n; opf(o, 5, true); }

static void decode_ldst(uint32_t i, Ops *o) {
    bool v = i & (1u << 26);
    if ((i & 0xbfbf0000) == 0x0c000000 || (i & 0xbf9f0000) == 0x0d000000) { opbase(o); return; }  // SIMD struct
    if ((i & 0xbfa00000) == 0x0c800000 || (i & 0xbf800000) == 0x0d800000) {                        // ... post-index
        opbase(o);
        opf(o, 16, false);
        o->wb = true;
        return;
    }
    if ((i & 0x3f000000) == 0x08000000) {             // exclusive, acquire/release, CAS
        opf(o, 0, false); opf(o, 10, false); opbase(o); opf(o, 16, false);
        o->casp = (i & 0xbfa07c00) == 0x08207c00;
        return;
    }
    if ((i & 0x3f200c00) == 0x19000000) { opf(o, 0, false); opbase(o); return; }   // LDAPUR/STLUR
    if ((i & 0x3a000000) == 0x28000000) {             // load/store pair
        if (!v) { opf(o, 0, false); opf(o, 10, false); }
        opbase(o);
        int idx = (i >> 23) & 3;
        if (idx == 1 || idx == 3) {
            int opc = i >> 30;
            int scale = v ? (4 << opc) : (opc == 2 ? 8 : 4);
            o->wb = o->wb_known = true;
            o->wb_delta = sext((i >> 15) & 0x7f, 7) * scale;
        }
        return;
    }
    int size = i >> 30, opc = (i >> 22) & 3;
    bool prfm = !v && size == 3 && opc == 2;
    if ((i & 0x3b200000) == 0x38000000) {             // imm9: unscaled, post, unprivileged, pre
        int op4 = (i >> 10) & 3;
        if (prfm && op4 == 0) { o->kind = K_DROP; return; }
        if (!v) opf(o, 0, false);
        opbase(o);
        if (op4 == 1 || op4 == 3) {
            o->wb = o->wb_known = true;
            o->wb_delta = sext((i >> 12) & 0x1ff, 9);
        }
        return;
    }
    if ((i & 0x3b200c00) == 0x38200000) {             // atomics, LDAPR
        if (v) { o->kind = K_BAD; return; }
        opf(o, 0, false); opbase(o); opf(o, 16, false);
        return;
    }
    if ((i & 0x3b200c00) == 0x38200800) {             // register offset
        if (prfm) { o->kind = K_DROP; return; }
        if (!v) opf(o, 0, false);
        opbase(o); opf(o, 16, false);
        return;
    }
    if ((i & 0x3b000000) == 0x39000000) {             // unsigned immediate
        if (prfm) { o->kind = K_DROP; return; }
        if (!v) opf(o, 0, false);
        opbase(o);
        return;
    }
    o->kind = K_BAD;                                  // pointer-auth loads, MTE, MOPS
}

static Ops decode(uint32_t i) {
    Ops o = { .base = -1, .spsrc = -1, .spdst = -1 };
    if ((i & 0x0a000000) == 0x08000000) { decode_ldst(i, &o); return o; }
    if ((i & 0x1f800000) == 0x11000000 || (i & 0x1f200000) == 0x0b200000) {   // add/sub imm, extended
        bool s = i & (1u << 29);
        o.spdst = s ? -1 : 0;
        opf(&o, 0, !s);
        o.spsrc = 1;
        opf(&o, 5, true);
        if (i & (1u << 27)) opf(&o, 16, false);
        return o;
    }
    if ((i & 0x1f800000) == 0x12000000) {             // logical immediate
        bool ands = ((i >> 29) & 3) == 3;
        o.spdst = ands ? -1 : 0;
        opf(&o, 0, !ands);
        opf(&o, 5, false);
        return o;
    }
    if ((i & 0x1f800000) == 0x12800000) { opf(&o, 0, false); return o; }                        // move wide
    if ((i & 0x1f800000) == 0x13000000) { opf(&o, 0, false); opf(&o, 5, false); return o; }     // bitfield
    if ((i & 0x1f800000) == 0x13800000 || (i & 0x1f000000) == 0x0a000000 ||                    // extr, logical,
        (i & 0x1f200000) == 0x0b000000 || (i & 0x1fe00000) == 0x1a800000 ||                    // add/sub shifted,
        (i & 0x5fe00000) == 0x1ac00000) {                                                      // csel, 2-source
        opf(&o, 0, false); opf(&o, 5, false); opf(&o, 16, false);
        return o;
    }
    if ((i & 0x1fe00000) == 0x1a000000) {             // adc/sbc (rmif/setf: Rn only)
        if ((i >> 10) & 0x3f) opf(&o, 5, false);
        else { opf(&o, 0, false); opf(&o, 5, false); opf(&o, 16, false); }
        return o;
    }
    if ((i & 0x1fe00000) == 0x1a400000) {             // conditional compare
        opf(&o, 5, false);
        if (!(i & (1u << 11))) opf(&o, 16, false);
        return o;
    }
    if ((i & 0x5fe00000) == 0x5ac00000) { opf(&o, 0, false); opf(&o, 5, false); return o; }     // 1-source
    if ((i & 0x1f000000) == 0x1b000000) {             // 3-source
        opf(&o, 0, false); opf(&o, 5, false); opf(&o, 16, false); opf(&o, 10, false);
        return o;
    }
    if ((i & 0x0e000000) == 0x0e000000) {             // SIMD and floating point
        if ((i & 0x5f20fc00) == 0x1e200000) {         // FP <-> integer
            int op = (i >> 16) & 7;
            opf(&o, (op == 2 || op == 3 || op == 7) ? 5 : 0, false);
        } else if ((i & 0x5f200000) == 0x1e000000) {  // FP <-> fixed point
            int op = (i >> 16) & 7;
            opf(&o, (op == 2 || op == 3) ? 5 : 0, false);
        } else if ((i & 0x9fe08400) == 0x0e000400 && !(i & (1u << 29))) {   // DUP/INS/SMOV/UMOV
            int imm4 = (i >> 11) & 15;
            if (imm4 == 1 || imm4 == 3) opf(&o, 5, false);
            else if (imm4 == 5 || imm4 == 7) opf(&o, 0, false);
        }
        return o;
    }
    o.kind = K_BAD;
    return o;
}

// ---- rewriting ----
static const int8_t POOL[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                              19, 20, 21, 22, 23, 24, 25, 26, 27, 28};

// Emits the host form of a non-branch instruction. Returns false if the block ends here.
static bool rewrite(uint32_t insn, const Ops *o, uint64_t pc) {
    if (o->kind == K_DROP) return true;
    if (o->kind == K_BAD) { emit_emulate(pc, insn); return false; }
    uint32_t used = 0, stolen = 0;
    for (int k = 0; k < o->n; k++) {
        int v = fld(insn, o->pos[k]);
        if (v == 31) continue;
        used |= 1u << v;
        if (stolen_reg(v)) stolen |= 1u << v;
    }
    if (o->casp) {                                    // register pairs: Rs, Rs+1 and Rt, Rt+1
        int rs = fld(insn, 16), rt = fld(insn, 0);
        used |= (1u << (rs + 1)) | (1u << (rt + 1));
        if (stolen_reg(rs) || stolen_reg(rs + 1) || stolen_reg(rt) || stolen_reg(rt + 1)) {
            emit_emulate(pc, insn);
            return false;
        }
    }
    bool mem = o->base >= 0 && !(fld(insn, o->pos[o->base]) == 31 && o->sp[o->base]);
    bool spsrc = o->spsrc >= 0 && fld(insn, o->pos[o->spsrc]) == 31;
    bool spdst = o->spdst >= 0 && fld(insn, o->pos[o->spdst]) == 31;
    if (spsrc && spdst) spsrc = spdst = false;        // sp-to-sp arithmetic is the same either way
    bool spwb = o->base >= 0 && !mem && o->wb;        // sp-based access that moves sp
    if (!stolen && !mem && !spsrc && !spdst) { emit(insn); return true; }

    if (!stolen) {
        if (mem) {
            int b = fld(insn, o->pos[o->base]);
            emit(add_x(17, 16, b));
            emit(fset(insn, o->pos[o->base], 17));
            if (o->wb) emit(sub_x(b, 17, 16));
        } else if (spsrc) {
            emit(SUB_X17_SP_X16);
            emit(fset(insn, o->pos[o->spsrc], 17));
        } else {
            emit(fset(insn, o->pos[o->spdst], 17));
            emit(ADD_SP_X16_X17);
        }
        return true;
    }

    int nst = __builtin_popcount(stolen);
    if (!mem && !spsrc && !spdst && !spwb && nst == 1) {
        // One stolen register, no memory: use host x16 for it and reload the base afterwards.
        int s = __builtin_ctz(stolen);
        uint32_t out = insn;
        for (int k = 0; k < o->n; k++) if (fld(insn, o->pos[k]) == s) out = fset(out, o->pos[k], 16);
        emit_get_ctx(17);
        emit(ldr_imm(16, 17, CTX_X(s)));
        emit(out);
        emit(str_imm(16, 17, CTX_X(s)));
        emit(ldr_imm(16, 17, CTX_BASE));
        return true;
    }

    // General case: borrow guest registers the instruction doesn't name, spilled far enough below
    // sp that a push by the instruction itself can't reach them.
    if (spwb && (!o->wb_known || o->wb_delta <= -480 || o->wb_delta > 496)) { emit_emulate(pc, insn); return false; }
    int t[4], nt = 0;
    for (unsigned k = 0; k < sizeof POOL && nt < nst + 1; k++)
        if (!(used & (1u << POOL[k]))) t[nt++] = POOL[k];
    int map[19] = {0}, ctx = t[nst];
    for (int r = 16, j = 0; r <= 18; r++) if (stolen & (1u << r)) map[r] = t[j++];
    Borrow *bw = &cur_borrows[cur_nborrows++];
    bw->start = (uint16_t)(ts->ptr - cur_start);
    bw->nt = (uint8_t)nt;
    for (int k = 0; k < 4; k++) bw->t[k] = k < nt ? (uint8_t)t[k] : 0xff;
    emit(stp_x(t[0], t[1], 31, -512));
    if (nt > 2) emit(stp_x(t[2], nt > 3 ? t[3] : 31, 31, -496));
    emit_get_ctx(ctx);
    for (int r = 16; r <= 18; r++) if (stolen & (1u << r)) emit(ldr_imm(map[r], ctx, CTX_X(r)));
    uint32_t out = insn;
    for (int k = 0; k < o->n; k++) {
        int v = fld(insn, o->pos[k]);
        if (v != 31 && stolen_reg(v)) out = fset(out, o->pos[k], map[v]);
    }
    int b = -1;
    if (mem) {
        b = fld(out, o->pos[o->base]);
        emit(add_x(17, 16, b));
        out = fset(out, o->pos[o->base], 17);
    }
    if (spsrc) { emit(SUB_X17_SP_X16); out = fset(out, o->pos[o->spsrc], 17); }
    if (spdst) out = fset(out, o->pos[o->spdst], 17);
    emit(out);
    if (mem && o->wb) emit(sub_x(b, 17, 16));
    for (int r = 16; r <= 18; r++) if (stolen & (1u << r)) emit(str_imm(map[r], ctx, CTX_X(r)));
    if (spwb) {
        emit(sub_imm(17, 31, (unsigned)(512 + o->wb_delta)));
        emit(ldp_x(t[0], t[1], 17, 0));
        if (nt > 2) emit(ldp_x(t[2], nt > 3 ? t[3] : 31, 17, 16));
    } else {
        emit(ldp_x(t[0], t[1], 31, -512));
        if (nt > 2) emit(ldp_x(t[2], nt > 3 ? t[3] : 31, 31, -496));
    }
    if (spdst) emit(ADD_SP_X16_X17);
    bw->end = (uint16_t)(ts->ptr - cur_start);
    return true;
}

// Loads guest register r (x16/x17/x18 come from the context) into host x17.
static void emit_reg_to_x17(int r) {
    if (stolen_reg(r)) {
        emit_get_ctx(17);
        emit(ldr_imm(17, 17, CTX_X(r)));
    } else {
        emit(mov_reg(17, r));
    }
}

// ---- system instructions ----
enum { SYS_NATIVE, SYS_DONE, SYS_EXIT, SYS_REWRITE };
static int rewrite_sys(uint32_t insn, uint64_t pc, Ops *o) {
    int t = insn & 31;
    bool st = stolen_reg(t);
    if ((insn & 0xffffffe0) == 0xd53bd040) {          // mrs xT, tpidr_el0
        if (t == 31) return SYS_DONE;
        if (st) {
            emit_get_ctx(17);
            emit(ldr_imm(16, 17, CTX_TPIDR));
            emit(str_imm(16, 17, CTX_X(t)));
            emit(ldr_imm(16, 17, CTX_BASE));
        } else {
            emit_get_ctx(t);
            emit(ldr_imm(t, t, CTX_TPIDR));
        }
        return SYS_DONE;
    }
    if ((insn & 0xffffffe0) == 0xd51bd040) {          // msr tpidr_el0, xT
        emit_get_ctx(17);
        if (st) {
            emit(ldr_imm(16, 17, CTX_X(t)));
            emit(str_imm(16, 17, CTX_TPIDR));
            emit(ldr_imm(16, 17, CTX_BASE));
        } else {
            emit(str_imm(t, 17, CTX_TPIDR));          // t == 31 stores xzr
        }
        return SYS_DONE;
    }
    unsigned op1 = (insn >> 16) & 7, crn = (insn >> 12) & 15, crm = (insn >> 8) & 15, op2 = (insn >> 5) & 7;
    *o = (Ops){ .base = -1, .spsrc = -1, .spdst = -1 };
    if ((insn & 0xfff00000) == 0xd5300000) {          // other mrs
        // EL0-readable registers run natively: DCZID, NZCV, FPCR, FPSR, counters. Not CTR_EL0:
        // Darwin traps it (bionic and ART read it for cache line sizes).
        bool ctr = op1 == 3 && crn == 0 && crm == 0 && op2 == 1;
        if (!ctr && op1 == 3 && (crn == 0 || crn == 4 || crn == 14)) { opf(o, 0, false); return SYS_REWRITE; }
        emit_emulate(pc, insn);                       // MIDR/ID registers and the like
        return SYS_EXIT;
    }
    if ((insn & 0xfff00000) == 0xd5100000) {          // msr (register)
        if (op1 == 3 && crn == 4) { opf(o, 0, false); return SYS_REWRITE; }   // NZCV/FPCR/FPSR
        emit_emulate(pc, insn);
        return SYS_EXIT;
    }
    if ((insn & 0xfffff000) == 0xd50b7000) {          // DC/IC by address
        if (crm == 4 && op2 == 1) {                   // DC ZVA: the address needs the base
            o->base = 0;
            opf(o, 0, false);
            return SYS_REWRITE;
        }
        if (crm == 5 && op2 == 1) {                   // IC IVAU: the line's translations go stale
            emit_emulate(pc, insn);
            return SYS_EXIT;
        }
        // DC ops (CVAU, CIVAC...): the host keeps its caches coherent itself, and Darwin traps them at EL0.
        return SYS_DONE;
    }
    if (insn == 0xd5033fdf) {                         // ISB: the point where new code must be visible
        emit_emulate(pc, insn);
        return SYS_EXIT;
    }
    // PACIA1716 and friends work on x16/x17, which aren't the guest's in translated code.
    if (insn == 0xd503211f || insn == 0xd503215f || insn == 0xd503219f || insn == 0xd50321df) return SYS_DONE;
    return SYS_NATIVE;                                // barriers, hints, CLREX
}

// ---- block translation ----
static Block *translate(TrSpace *s, uint64_t pc, uint64_t limit) {
    ts = s;
    if (!s->ptr || (size_t)(s->end - s->ptr) < BLOCK_RESERVE) space_grow(s);
    uint32_t *start = s->ptr;
    cur_start = start;
    cur_nborrows = 0;
    int ninsn = 0;
    uint64_t gpc = pc;
    for (int n = 0;; n++, gpc += 4) {
        cur_imap[n] = (uint16_t)(s->ptr - start);
        if (n == MAX_BLOCK_INSNS || gpc >= limit) { emit_direct_exit(gpc); break; }
        ninsn = n + 1;
        uint32_t insn = *(const uint32_t *)(s->base + gpc);

        if ((insn & 0x7c000000) == 0x14000000) {       // B / BL
            uint64_t target = gpc + (sext(insn & 0x3ffffff, 26) << 2);
            if (insn & 0x80000000) emit_mov64(30, gpc + 4);
            emit_direct_exit(target);
            break;
        }
        bool cb = (insn & 0x7e000000) == 0x34000000, tb = (insn & 0x7e000000) == 0x36000000;
        if ((insn & 0xff000010) == 0x54000000 || cb || tb) {   // B.cond, CBZ/CBNZ, TBZ/TBNZ
            int64_t off = tb ? sext((insn >> 5) & 0x3fff, 14) : sext((insn >> 5) & 0x7ffff, 19);
            uint64_t target = gpc + (off << 2);
            if ((cb || tb) && stolen_reg(insn & 31)) {
                emit_reg_to_x17(insn & 31);
                insn = (insn & ~31u) | 17;
            }
            uint32_t *br = s->ptr;
            emit(0);                                    // patched below
            emit_direct_exit(gpc + 4);
            int64_t rel = s->ptr - br;
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
            // Inline probe of the window's lookup table (just below x16); flags are untouched.
            emit_reg_to_x17(n);
            if (op == 1) emit_mov64(30, gpc + 4);
            emit(0xa93e07e0);                           // stp x0, x1, [sp, #-32]
            emit(0xd1410200);                           // sub x0, x16, #IB_SIZE
            emit(0xd3400000u | (2u << 16) | ((2u + IB_BITS - 1) << 10) | (17u << 5) | 1); // ubfx x1, x17, #2, #IB_BITS
            emit(0x8b011000);                           // add x0, x0, x1, lsl #4
            emit(0xa9400400);                           // ldp x0, x1, [x0]
            emit(0xca110000);                           // eor x0, x0, x17
            emit(0xb5000080);                           // cbnz x0, miss
            emit(0xaa0103f1);                           // mov x17, x1
            emit(0xa97e07e0);                           // ldp x0, x1, [sp, #-32]
            emit(0xd61f0220);                           // br x17
            emit(0xaa1103f0);                           // miss: mov x16, x17 (x0/x1 still at [sp-32])
            emit(movz(17, 0, EXIT_INDIRECT));
            uint32_t *at = s->ptr;
            emit(b_to(at, s->tramp_spilled));
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
            if (rd == 31) continue;
            if (stolen_reg(rd)) {
                emit_get_ctx(17);
                emit_mov64(16, v);
                emit(str_imm(16, 17, CTX_X(rd)));
                emit(ldr_imm(16, 17, CTX_BASE));
            } else {
                emit_mov64(rd, v);
            }
            continue;
        }
        if ((insn & 0x3b000000) == 0x18000000) {        // LDR (literal)
            int t = insn & 31, opc = insn >> 30;
            uint64_t addr = gpc + (sext((insn >> 5) & 0x7ffff, 19) << 2);
            if (!(insn & (1u << 26))) {
                if (opc == 3 || t == 31) continue;      // PRFM, xzr
                uint32_t ld = opc == 0 ? 0xb8606800u : opc == 1 ? 0xf8606800u : 0xb8a06800u;  // [x16, xM]
                if (stolen_reg(t)) {
                    emit_mov64(17, addr);
                    emit(ld | (17u << 16) | (16u << 5) | 17);
                    emit_get_ctx(16);
                    emit(str_imm(17, 16, CTX_X(t)));
                    emit(ldr_imm(16, 16, CTX_BASE));
                } else {
                    emit_mov64(t, addr);
                    emit(ld | ((uint32_t)t << 16) | (16u << 5) | t);
                }
            } else {
                uint32_t ld = opc == 0 ? 0xbc606800u : opc == 1 ? 0xfc606800u : 0x3ce06800u;
                emit_mov64(17, addr);
                emit(ld | (17u << 16) | (16u << 5) | t);
            }
            continue;
        }
        if ((insn & 0xffc00000) == 0xd5000000) {        // system instructions
            Ops o;
            int r = rewrite_sys(insn, gpc, &o);
            if (r == SYS_EXIT) break;
            if (r == SYS_NATIVE) emit(insn);
            if (r == SYS_REWRITE && !rewrite(insn, &o, gpc)) break;
            continue;
        }
        if ((insn & 0x1c000000) == 0x14000000) {        // other branch/exception encodings
            emit_emulate(gpc, insn);
            break;
        }
        Ops o = decode(insn);
        if (!rewrite(insn, &o, gpc)) break;
    }
    Block *b = calloc(1, sizeof(*b));
    b->pc = pc;
    b->host = start;
    b->host_end = s->ptr;
    b->ninsn = ninsn;
    b->imap = malloc((size_t)ninsn * sizeof(uint16_t));
    memcpy(b->imap, cur_imap, (size_t)ninsn * sizeof(uint16_t));
    if (cur_nborrows) {
        b->borrows = malloc((size_t)cur_nborrows * sizeof(Borrow));
        memcpy(b->borrows, cur_borrows, (size_t)cur_nborrows * sizeof(Borrow));
        b->nborrows = cur_nborrows;
    }
    unsigned h = hidx(pc);
    b->next = s->htab[h];
    s->htab[h] = b;
    sys_icache_invalidate(start, (size_t)(s->ptr - start) * 4);
    return b;
}

// Guest code in [start, start + len) changed. Blocks that contain any of it are retired: their first
// words become a far jump to an exit stub that asks the dispatcher for the current translation, so
// chained branches and inline-table hits into them keep working (and get re-chained). tr_lock held.
static void retire_blocks(TrSpace *s, uint64_t start, uint64_t len) {
    bool writing = false;
    for (unsigned i = 0; i < (1u << HBITS); i++)
        for (Block *b = s->htab[i]; b; b = b->next) {
            if (b->dead || b->pc >= start + len || b->pc + (uint64_t)b->ninsn * 4 <= start) continue;
            if (!writing) { ts = s; jit_write(true); writing = true; }
            b->dead = true;
            if (!s->ptr || (size_t)(s->end - s->ptr) < BLOCK_RESERVE) space_grow(s);
            ExitRec *r = calloc(1, sizeof *r);
            r->target = b->pc;
            r->patch = b->host;
            r->next = s->exits;
            s->exits = r;
            uint32_t *stub = s->ptr;
            emit_exit((uint64_t)(uintptr_t)r, EXIT_DIRECT);
            sys_icache_invalidate(stub, (size_t)(s->ptr - stub) * 4);
            // ldr x17, [pc, #8]; br x17; .quad stub  -- every block is at least one exit stub long
            b->host[2] = (uint32_t)(uintptr_t)stub;
            b->host[3] = (uint32_t)((uintptr_t)stub >> 32);
            b->host[1] = 0xd61f0220;
            __atomic_store_n(&b->host[0], 0x58000051u, __ATOMIC_RELEASE);
            sys_icache_invalidate(b->host, 16);
            IbEntry *e = (IbEntry *)(uintptr_t)(s->base - IB_SIZE) + ((b->pc >> 2) & ((1u << IB_BITS) - 1));
            if (e->host == b->host) { uint64_t one = 1, zero = 0; __asm__ volatile("stp %0, %1, [%2]" :: "r"(one), "r"(zero), "r"(e) : "memory"); }
        }
    if (writing) jit_write(false);
}

static void flush_pending_locked(TrSpace *s) {
    if (s->inv_hi > s->inv_lo) { retire_blocks(s, s->inv_lo, s->inv_hi - s->inv_lo); s->inv_lo = s->inv_hi = 0; }
}

// IC IVAU: remember the line; a flush sequence ends with ISB, which applies the batch.
void tr_invalidate_later(TrSpace *s, uint64_t start, uint64_t len) {
    if (!s) return;
    pthread_mutex_lock(&tr_lock);
    if (s->inv_hi <= s->inv_lo) { s->inv_lo = start; s->inv_hi = start + len; }
    else { if (start < s->inv_lo) s->inv_lo = start; if (start + len > s->inv_hi) s->inv_hi = start + len; }
    pthread_mutex_unlock(&tr_lock);
}

// munmap, MAP_FIXED over code, mremap, ISB: apply now.
void tr_invalidate(TrSpace *s, uint64_t start, uint64_t len) {
    if (!s) return;
    pthread_mutex_lock(&tr_lock);
    flush_pending_locked(s);
    if (len) retire_blocks(s, start, len);
    pthread_mutex_unlock(&tr_lock);
}

// Returns the host code for guest pc, translating it if needed, or NULL if pc isn't executable.
static uint32_t *get_block(GuestCtx *c, uint64_t pc) {
    TrSpace *s = c->space;
    flush_pending_locked(s);
    Block *b = lookup(s, pc);
    if (b) return b->host;
    uint64_t limit = (pc & 3) ? 0 : sys_code_end(c, pc);
    if (!limit) return NULL;
    jit_write(true);
    b = translate(s, pc, limit);
    jit_write(false);
    return b->host;
}

static void chain(ExitRec *r, uint32_t *dest) {
    int64_t d = dest - r->patch;
    if (d < -(1 << 25) || d >= (1 << 25)) return;
    jit_write(true);
    *r->patch = b_to(r->patch, dest);
    jit_write(false);
    sys_icache_invalidate(r->patch, 4);
}

void tr_leave(void) {
    pthread_setspecific(ctx_key, NULL);
    tr_cur = NULL;
}

void tr_run(GuestCtx *c) {
    tr_cur = c;
    pthread_setspecific(ctx_key, c);
    for (;;) {
        sys_deliver_signals(c);
        pthread_mutex_lock(&tr_lock);
        uint32_t *h = get_block(c, c->pc);
        pthread_mutex_unlock(&tr_lock);
        if (!h) { sys_fault(c, 11, 1, c->pc); continue; }
        onx_enter(c, h);
        switch (c->exit_kind) {
        case EXIT_DIRECT: {
            ExitRec *r = (ExitRec *)(uintptr_t)c->exit_arg;
            c->pc = r->target;
            pthread_mutex_lock(&tr_lock);
            uint32_t *t = get_block(c, r->target);
            if (t) chain(r, t);
            pthread_mutex_unlock(&tr_lock);
            break;
        }
        case EXIT_INDIRECT: {
            c->pc = c->exit_arg;
            pthread_mutex_lock(&tr_lock);
            uint32_t *t = get_block(c, c->pc);
            if (t) {
                IbEntry *e = (IbEntry *)(uintptr_t)(c->base - IB_SIZE) + ((c->pc >> 2) & ((1u << IB_BITS) - 1));
                // One 16-byte store (single-copy atomic with FEAT_LSE2) so racing threads never
                // see a pc paired with another block's code.
                __asm__ volatile("stp %0, %1, [%2]" :: "r"(c->pc), "r"(t), "r"(e) : "memory");
            }
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
            if (sys_emulate(c, r->pc, r->insn)) {
                fprintf(stderr, "onirun: unsupported instruction %08x at guest pc %llx\n", r->insn,
                        (unsigned long long)r->pc);
                c->pc = r->pc;
                sys_fault(c, 4, 1, r->pc);                // ILL_ILLOPC
            }
            break;
        }
        case EXIT_BRK:
            c->pc = c->exit_arg;
            sys_fault(c, 5, 1, c->exit_arg);              // TRAP_BRKPT
            break;
        case EXIT_KILLED:
            sys_abandoned(c);
            break;
        default:
            die("onirun: bad exit kind %llu", (unsigned long long)c->exit_kind);
        }
    }
}

// ---- host signals ----
static bool in_cache(uint64_t pc) {
    return pc >= (uint64_t)(uintptr_t)cache && pc < (uint64_t)(uintptr_t)cache + CACHE_SIZE;
}

static const Block *find_host(TrSpace *s, uint64_t host_pc) {
    for (unsigned i = 0; s && i < (1u << HBITS); i++)
        for (Block *b = s->htab[i]; b; b = b->next)
            if (host_pc >= (uint64_t)(uintptr_t)b->host && host_pc < (uint64_t)(uintptr_t)b->host_end) return b;
    return NULL;
}

// SIGUSR1 interrupts a thread's blocking host call. A thread whose process is going away is pulled
// straight out of translated code.
volatile uint64_t g_barrier_gen;   // membarrier generation (proc.c bumps it, every thread acks it here)
static void kick_handler(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    ucontext_t *uc = uc_;
    GuestCtx *c = pthread_getspecific(ctx_key);
    if (c) { __atomic_thread_fence(__ATOMIC_SEQ_CST); c->barrier_ack = g_barrier_gen; }
    if (c && c->kill && in_cache(uc->uc_mcontext->__ss.__pc)) {
        uc->uc_mcontext->__ss.__x[0] = (uint64_t)(uintptr_t)c;
        uc->uc_mcontext->__ss.__pc = (uint64_t)(uintptr_t)onx_abandon;
    }
}

static void fault_handler(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
    GuestCtx *c = pthread_getspecific(ctx_key);
    char buf[512];
    int n;
    if (c && in_cache(pc)) {
        // Guest code faulted. Rebuild the exact guest state (registers, precise pc) so the fault
        // can be delivered to the guest's handler, or reported.
        const Block *b = find_host(c->space, pc);
        // Unused window space is PROT_NONE, which Darwin reports as SIGBUS (with si_code
        // BUS_ADRALN, so it can't be told from a real alignment fault); Linux says SIGSEGV.
        c->fault_sig = sig == SIGSEGV || sig == SIGBUS ? 11 : sig == SIGILL ? 4 : sig == SIGFPE ? 8 : 5;
        c->fault_addr = (uint64_t)(uintptr_t)si->si_addr - c->base;
        c->fault_code = 1;
        bool pc_fault = sig == SIGILL || sig == SIGTRAP;   // si_addr is the instruction: use the guest pc below
        for (int r = 0; r < 29; r++)
            if (r < 16 || r > 18) c->x[r] = uc->uc_mcontext->__ss.__x[r];
        c->x[29] = uc->uc_mcontext->__ss.__fp;
        c->x[30] = uc->uc_mcontext->__ss.__lr;
        uint64_t hsp = uc->uc_mcontext->__ss.__sp;
        c->sp = hsp - c->base;
        c->nzcv = uc->uc_mcontext->__ss.__cpsr & 0xf0000000u;
        c->fpsr = uc->uc_mcontext->__ns.__fpsr;
        c->fpcr = uc->uc_mcontext->__ns.__fpcr;
        memcpy(c->q, uc->uc_mcontext->__ns.__v, sizeof c->q);
        c->pc = 0;
        if (b) {
            uint32_t hoff = (uint32_t)((pc - (uint64_t)(uintptr_t)b->host) / 4);
            int lo = 0, hi = b->ninsn - 1;
            while (lo < hi) { int mid = (lo + hi + 1) / 2; if (b->imap[mid] <= hoff) lo = mid; else hi = mid - 1; }
            c->pc = b->pc + 4 * (uint64_t)lo;
            if (pc_fault) c->fault_addr = c->pc;
            for (int i = 0; i < b->nborrows; i++) {
                const Borrow *bw = &b->borrows[i];
                if (hoff < bw->start || hoff >= bw->end) continue;
                for (int k = 0; k < bw->nt; k++) c->x[bw->t[k]] = *(uint64_t *)(uintptr_t)(hsp - 512 + 8 * (uint64_t)k);
            }
        } else {
            n = snprintf(buf, sizeof buf, "onirun: [%llu] fault at host pc %llx outside any block\n", (unsigned long long)c->tid, (unsigned long long)pc);
            write(2, buf, (size_t)n);
        }
        uc->uc_mcontext->__ss.__x[0] = (uint64_t)(uintptr_t)c;
        uc->uc_mcontext->__ss.__pc = (uint64_t)(uintptr_t)onx_abandon;
        return;
    }
    n = snprintf(buf, sizeof buf, "onirun: internal fault: signal %d at host pc %llx (address %p)\n", sig,
                 (unsigned long long)pc, si->si_addr);
    write(2, buf, (size_t)n);
    if (c) {
        n = snprintf(buf, sizeof buf, "  guest tid %llu pc %llx sp %llx base %llx exit_kind %llu\n", (unsigned long long)c->tid,
                     (unsigned long long)c->pc, (unsigned long long)c->sp, (unsigned long long)c->base, (unsigned long long)c->exit_kind);
        write(2, buf, (size_t)n);
    }
    void *frames[32];
    frames[0] = (void *)(uintptr_t)pc;
    int nf = backtrace(frames + 1, 31);
    backtrace_symbols_fd(frames, nf + 1, 2);
    for (int r = 0; r < 29; r += 4) {
        n = snprintf(buf, sizeof buf, "  x%-2d %016llx x%-2d %016llx x%-2d %016llx x%-2d %016llx\n",
                     r, uc->uc_mcontext->__ss.__x[r], r + 1, uc->uc_mcontext->__ss.__x[r + 1],
                     r + 2, uc->uc_mcontext->__ss.__x[r + 2], r + 3, r + 3 < 29 ? uc->uc_mcontext->__ss.__x[r + 3] : 0);
        write(2, buf, (size_t)n);
    }
    _exit(128 + sig);
}

void tr_init(void) {
    cache = mmap(NULL, CACHE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (cache == MAP_FAILED) die("onirun: cannot allocate the code cache: %s", strerror(errno));
    if (pthread_key_create(&ctx_key, NULL)) die("onirun: pthread_key_create failed");
    onx_tsd_off = (uint64_t)ctx_key * 8;
    // Check that our TSD slot is where translated code looks for it.
    pthread_setspecific(ctx_key, (void *)0x1234);
    uint64_t base;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
    if (((void **)(uintptr_t)(base & ~7ull))[ctx_key] != (void *)0x1234)
        die("onirun: unexpected Darwin TSD layout");
    pthread_setspecific(ctx_key, NULL);

    struct sigaction sa = { .sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK };
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    struct sigaction ka = { .sa_sigaction = kick_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK };
    sigaction(SIGUSR1, &ka, NULL);
    signal(SIGPIPE, SIG_IGN);
}
