// Switching between host C code and translated guest code. Offsets match GuestCtx in onx.h.
//
// Guest code runs with the guest's own register values in x0..x30, sp, nzcv and v0..v31 (except
// x18, which XNU zeroes on context switches, so the guest's x18 lives in the context). Every way
// out of translated code goes through a stub that first stores guest x16/x17 just below the guest
// stack pointer (AArch64 has no red zone, so the guest never keeps live data there), puts the exit
// argument in x16 and the exit kind in x17, and jumps to the code cache's trampoline, which saves
// x0/x1 below that and comes here.

    .text
    .globl _onx_enter
    .globl _onx_exit_common
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
    // Guest x16/x17 go below the guest sp; every block's entry instruction reloads them.
    ldr x9, [x0, #248]
    ldp x10, x11, [x0, #128]
    stp x10, x11, [x9, #-16]
    mov x16, x1
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
    ldr x9, [x0, #248]
    mov sp, x9
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
    br x16

    .p2align 2
// Entered from the code-cache trampoline. [sp-32] = guest x0,x1; [sp-16] = guest x16,x17;
// x16 = exit argument; x17 = exit kind.
_onx_exit_common:
    mrs x0, tpidrro_el0
    and x0, x0, #0xfffffffffffffff8
    adrp x1, _onx_tsd_off@PAGE
    ldr x1, [x1, _onx_tsd_off@PAGEOFF]
    ldr x0, [x0, x1]
    stp x16, x17, [x0, #296]
    ldp x16, x17, [sp, #-32]
    stp x16, x17, [x0, #0]
    ldp x16, x17, [sp, #-16]
    stp x16, x17, [x0, #128]
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
    mov x16, sp
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

    .data
    .p2align 3
_onx_tsd_off:
    .quad 0
