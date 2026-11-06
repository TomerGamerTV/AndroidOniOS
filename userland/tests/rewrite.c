// Exercises the translator's rarer rewrite paths with hand-written instructions: the guest's
// x16/x17/x18 (which the host takes over), sp read or written as a value, sp-based writeback,
// exclusives and atomics through x16, SIMD post-index, literal loads, TLS and DC ZVA.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(name, got, want) do { uint64_t _g = (uint64_t)(got), _w = (uint64_t)(want); \
    if (_g != _w) { failures++; printf("FAIL %s: got %llx want %llx\n", name, (unsigned long long)_g, (unsigned long long)_w); } } while (0)

static uint64_t buf[64] __attribute__((aligned(64)));

int main(void) {
    uint64_t r0, r1, r2;

    // Push and pop x16/x17 with sp writeback.
    __asm__ volatile("mov x16, #0x1616\n mov x17, #0x1717\n"
                     "stp x16, x17, [sp, #-16]!\n mov x16, #0\n mov x17, #0\n"
                     "ldp x16, x17, [sp], #16\n mov %0, x16\n mov %1, x17"
                     : "=r"(r0), "=r"(r1) :: "x16", "x17", "memory");
    CHECK("stp/ldp x16,x17 sp writeback", r0, 0x1616);
    CHECK("stp/ldp x17", r1, 0x1717);

    // sp as a value into a stolen register and back; cmp against sp.
    __asm__ volatile("mov x2, sp\n add x16, sp, #32\n sub x16, x16, #32\n mov x17, sp\n"
                     "cmp x17, x2\n cset %0, eq\n cmp sp, x16\n cset %1, eq\n"
                     "mov x0, sp\n mov sp, x16\n mov %2, sp\n mov sp, x0"
                     : "=r"(r0), "=r"(r1), "=r"(r2) :: "x0", "x2", "x16", "x17", "cc", "memory");
    CHECK("mov x17, sp", r0, 1);
    CHECK("cmp sp, x16", r1, 1);
    uint64_t sp_now;
    __asm__ volatile("mov %0, sp" : "=r"(sp_now));
    CHECK("mov sp, x16", r2 == sp_now, 1);

    // and sp, xN, #imm (logical immediate writing sp) and sub sp, sp, x16.
    __asm__ volatile("mov x0, sp\n mov x1, sp\n sub x1, x1, #40\n and sp, x1, #~15\n mov %0, sp\n"
                     "mov x16, #32\n sub sp, sp, x16\n mov %1, sp\n mov sp, x0"
                     : "=r"(r0), "=r"(r1) :: "x0", "x1", "x16", "memory");
    CHECK("and sp", r0, (sp_now - 40) & ~15ull);
    CHECK("sub sp, sp, x16", r1, ((sp_now - 40) & ~15ull) - 32);

    // Loads and stores through stolen base and index registers.
    for (int i = 0; i < 64; i++) buf[i] = 0x1000 + i;
    __asm__ volatile("mov x16, %3\n mov x17, #16\n ldr %0, [x16, x17]\n ldr x17, [x16, #8]\n mov %1, x17\n"
                     "mov x9, x18\n add x18, x16, #24\n ldr %2, [x18]\n mov x18, x9\n str x17, [x16, #32]"
                     : "=&r"(r0), "=&r"(r1), "=&r"(r2) : "r"(buf) : "x9", "x16", "x17", "memory");
    CHECK("ldr [x16, x17]", r0, 0x1002);
    CHECK("ldr x17, [x16, #8]", r1, 0x1001);
    CHECK("ldr [x18]", r2, 0x1003);
    CHECK("str x17, [x16, #32]", buf[4], 0x1001);

    // Pre/post-index through a stolen base.
    __asm__ volatile("mov x16, %2\n ldr %0, [x16, #8]!\n ldr %1, [x16], #8\n sub x16, x16, %2\n mov x0, x16\n str x0, [%2, #48]"
                     : "=&r"(r0), "=&r"(r1) : "r"(buf) : "x0", "x16", "memory");
    CHECK("ldr [x16, #8]!", r0, 0x1001);
    CHECK("ldr [x16], #8", r1, 0x1001);
    CHECK("x16 after writeback", buf[6], 16);

    // Exclusives, CAS and LSE atomics through x16 with x17 as the status/compare register.
    buf[8] = 5;
    __asm__ volatile("mov x16, %1\n 1: ldaxr x0, [x16]\n add x0, x0, #1\n stlxr w17, x0, [x16]\n cbnz w17, 1b\n"
                     ".arch_extension lse\n mov x17, #6\n mov x0, #9\n cas x17, x0, [x16]\n mov %0, x17"
                     : "=r"(r0) : "r"(&buf[8]) : "x0", "x16", "x17", "memory");
    CHECK("ldaxr/stlxr/cas", buf[8], 9);
    CHECK("cas old value", r0, 6);
    __asm__ volatile(".arch_extension lse\n mov x16, %1\n mov x17, #3\n ldadd x17, x0, [x16]\n mov %0, x0" : "=r"(r0) : "r"(&buf[8]) : "x0", "x16", "x17", "memory");
    CHECK("ldadd", buf[8], 12);
    CHECK("ldadd old", r0, 9);

    // SIMD post-index through x16; FP/SIMD <-> stolen register moves.
    for (int i = 0; i < 4; i++) buf[16 + i] = 0x2000 + i;
    __asm__ volatile("mov x16, %2\n ld1 {v0.2d}, [x16], #16\n umov x17, v0.d[1]\n mov %0, x17\n"
                     "sub x16, x16, %2\n mov %1, x16\n fmov d1, x16\n fmov x0, d1"
                     : "=&r"(r0), "=&r"(r1) : "r"(&buf[16]) : "x0", "x16", "x17", "v0", "v1", "memory");
    CHECK("ld1 post-index + umov x17", r0, 0x2001);
    CHECK("x16 after ld1 post-index", r1, 16);

    // Literal load, adr and conditional branch on stolen registers.
    __asm__ volatile("ldr x16, 1f\n b 2f\n .p2align 3\n 1: .quad 0x123456789abcdef0\n 2: mov %0, x16\n"
                     "adr x17, 2b\n adr x0, 2b\n sub %1, x17, x0\n"
                     "mov x16, #0\n mov %2, #1\n cbz x16, 3f\n mov %2, #2\n 3:"
                     : "=&r"(r0), "=&r"(r1), "=&r"(r2) :: "x0", "x16", "x17");
    CHECK("ldr x16, literal", r0, 0x123456789abcdef0ull);
    CHECK("adr x17", r1, 0);
    CHECK("cbz x16", r2, 1);

    // TLS register through stolen registers, and nzcv into x16.
    uint64_t tls;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
    __asm__ volatile("mrs x16, tpidr_el0\n mov %0, x16\n mov x17, #0x55\n msr tpidr_el0, x17\n mrs %1, tpidr_el0\n"
                     "msr tpidr_el0, x16\n cmp xzr, xzr\n mrs x16, nzcv\n mov %2, x16"
                     : "=&r"(r0), "=&r"(r1), "=&r"(r2) :: "x16", "x17", "cc");
    CHECK("mrs x16, tpidr_el0", r0, tls);
    CHECK("msr tpidr_el0, x17", r1, 0x55);
    CHECK("mrs x16, nzcv", r2, 0x60000000);

    // Indirect calls through x16/x17 (the PLT pattern) and DC ZVA.
    uint64_t dczid;
    __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
    memset(buf, 0xff, sizeof buf);
    __asm__ volatile("mov x16, %0\n dc zva, x16" :: "r"(buf) : "x16", "memory");
    CHECK("dc zva", buf[0] == 0 && buf[(4u << (dczid & 15)) / 8 - 1] == 0 && buf[63] != 0, 1);
    __asm__ volatile("adr x17, 1f\n blr x17\n b 2f\n 1: mov %0, #77\n ret\n 2:" : "=r"(r0) :: "x17", "x30");
    CHECK("blr x17", r0, 77);

    printf("%s\n", failures ? "REWRITE FAILED" : "REWRITE OK");
    return failures != 0;
}
