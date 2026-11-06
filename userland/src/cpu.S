// Switching between host C code and translated guest code. Offsets match GuestCtx in onx.h.
//
// Guest code runs with the guest's own values in x0..x15, x19..x30, nzcv and v0..v31. The host keeps
// x16 = the process window base (guest address g is host address x16 + g) and uses x17 as scratch;
// the guest's x16, x17 and x18 (which XNU zeroes on context switches) live in the context. The host
// sp holds the guest stack pointer as a host address (window base + guest sp). Every way out of
// translated code puts the exit argument in x16 and the exit kind in x17 and jumps to its code
// chunk's trampoline, which saves guest x0/x1 just below sp (AArch64 has no red zone) and comes here.

    .text
    .globl _onx_enter
    .globl _onx_exit_common
    .globl _onx_exit_spilled
    .globl _onx_abandon
    .globl _onx_tsd_off

    .p2align 2
// void onx_enter(GuestCtx *ctx, void *host_code)
_onx_enter:
    add x9, x0, #832
    mov x10, sp
    str x10, [x9, #0]
    stp x19, x20, [x9, #8]
    stp x21, x22, [x9, #24]
    stp x23, x24, [x9, #40]
    stp x25, x26, [x9, #56]
    stp x27, x28, [x9, #72]
    stp x29, x30, [x9, #88]
    stp d8, d9, [x9, #104]
    stp d10, d11, [x9, #120]
    stp d12, d13, [x9, #136]
    stp d14, d15, [x9, #152]
    add x9, x0, #320
    ldp q0, q1, [x9, #0]
    ldp q2, q3, [x9, #32]
    ldp q4, q5, [x9, #64]
    ldp q6, q7, [x9, #96]
    ldp q8, q9, [x9, #128]
    ldp q10, q11, [x9, #160]
    ldp q12, q13, [x9, #192]
    ldp q14, q15, [x9, #224]
    ldp q16, q17, [x9, #256]
    ldp q18, q19, [x9, #288]
    ldp q20, q21, [x9, #320]
    ldp q22, q23, [x9, #352]
    ldp q24, q25, [x9, #384]
    ldp q26, q27, [x9, #416]
    ldp q28, q29, [x9, #448]
    ldp q30, q31, [x9, #480]
    ldp x10, x11, [x0, #272]
    msr fpcr, x10
    msr fpsr, x11
    ldr x10, [x0, #264]
    msr nzcv, x10
    ldr x16, [x0, #312]
    ldr x9, [x0, #248]
    add x9, x9, x16
    mov sp, x9
    // Park the block address in the context: the guest stack must not be touched here (a thread
    // exiting the bionic way has already unmapped its stack, and ART's signal handler briefly
    // leaves sp misaligned, which sp-based accesses would trap on).
    str x1, [x0, #296]
    mov x17, x0
    ldp x0, x1, [x17, #0]
    ldp x2, x3, [x17, #16]
    ldp x4, x5, [x17, #32]
    ldp x6, x7, [x17, #48]
    ldp x8, x9, [x17, #64]
    ldp x10, x11, [x17, #80]
    ldp x12, x13, [x17, #96]
    ldp x14, x15, [x17, #112]
    ldp x19, x20, [x17, #152]
    ldp x21, x22, [x17, #168]
    ldp x23, x24, [x17, #184]
    ldp x25, x26, [x17, #200]
    ldp x27, x28, [x17, #216]
    ldp x29, x30, [x17, #232]
    ldr x17, [x17, #296]
    br x17

    .p2align 2
// Entered from a chunk trampoline. Guest x0,x1 are already in the context (exit stubs store them
// there) or, for _onx_exit_spilled, at [sp-32] (the inline branch probe parks them there).
// x16 = exit argument; x17 = exit kind.
_onx_exit_spilled:
    mrs x0, tpidrro_el0
    and x0, x0, #0xfffffffffffffff8
    adrp x1, _onx_tsd_off@PAGE
    ldr x1, [x1, _onx_tsd_off@PAGEOFF]
    ldr x0, [x0, x1]
    stp x16, x17, [x0, #296]
    ldp x16, x17, [sp, #-32]
    stp x16, x17, [x0, #0]
    b Lsave_rest
_onx_exit_common:
    mrs x0, tpidrro_el0
    and x0, x0, #0xfffffffffffffff8
    adrp x1, _onx_tsd_off@PAGE
    ldr x1, [x1, _onx_tsd_off@PAGEOFF]
    ldr x0, [x0, x1]
    stp x16, x17, [x0, #296]
Lsave_rest:
    stp x2, x3, [x0, #16]
    stp x4, x5, [x0, #32]
    stp x6, x7, [x0, #48]
    stp x8, x9, [x0, #64]
    stp x10, x11, [x0, #80]
    stp x12, x13, [x0, #96]
    stp x14, x15, [x0, #112]
    stp x19, x20, [x0, #152]
    stp x21, x22, [x0, #168]
    stp x23, x24, [x0, #184]
    stp x25, x26, [x0, #200]
    stp x27, x28, [x0, #216]
    stp x29, x30, [x0, #232]
    ldr x17, [x0, #312]
    mov x16, sp
    sub x16, x16, x17
    str x16, [x0, #248]
    mrs x16, nzcv
    str x16, [x0, #264]
    mrs x16, fpcr
    mrs x17, fpsr
    stp x16, x17, [x0, #272]
    add x9, x0, #320
    stp q0, q1, [x9, #0]
    stp q2, q3, [x9, #32]
    stp q4, q5, [x9, #64]
    stp q6, q7, [x9, #96]
    stp q8, q9, [x9, #128]
    stp q10, q11, [x9, #160]
    stp q12, q13, [x9, #192]
    stp q14, q15, [x9, #224]
    stp q16, q17, [x9, #256]
    stp q18, q19, [x9, #288]
    stp q20, q21, [x9, #320]
    stp q22, q23, [x9, #352]
    stp q24, q25, [x9, #384]
    stp q26, q27, [x9, #416]
    stp q28, q29, [x9, #448]
    stp q30, q31, [x9, #480]
Lrestore_host:
    add x9, x0, #832
    ldr x10, [x9, #0]
    mov sp, x10
    ldp x19, x20, [x9, #8]
    ldp x21, x22, [x9, #24]
    ldp x23, x24, [x9, #40]
    ldp x25, x26, [x9, #56]
    ldp x27, x28, [x9, #72]
    ldp x29, x30, [x9, #88]
    ldp d8, d9, [x9, #104]
    ldp d10, d11, [x9, #120]
    ldp d12, d13, [x9, #136]
    ldp d14, d15, [x9, #152]
    ret

    .p2align 2
// A signal handler redirects a thread here (x0 = ctx) to pull it out of translated code when its
// process is going away. Guest registers are not saved.
_onx_abandon:
    mov x17, #6
    str x17, [x0, #304]
    b Lrestore_host

    .data
    .p2align 3
_onx_tsd_off:
    .quad 0
