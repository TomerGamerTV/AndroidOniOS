// Stress test for onirun: many threads allocating and freeing, refcounting with atomics, using
// x16/x17 as data registers in memory instructions, and (built with shadow call stacks) x18.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 12
#define ROUNDS 40000
typedef struct { atomic_int refs; uint64_t pad[7]; } Obj;
static Obj *shared[256];
static pthread_mutex_t mu[256];
static atomic_long frees, allocs, errors;

static __attribute__((noinline)) uint64_t via_x16(uint64_t *p, uint64_t v) {
    uint64_t out;
    __asm__ volatile("mov x16, %1\n str x16, [%2]\n ldr x17, [%2]\n mov %0, x17\n mov x16, %2\n ldr x17, [x16]\n add x17, x17, #1\n str x17, [x16]\n ldr %0, [x16]"
                     : "=&r"(out) : "r"(v), "r"(p) : "x16", "x17", "memory");
    return out;
}

static void obj_put(Obj *o) {
    if (atomic_fetch_sub(&o->refs, 1) == 1) { memset(o, 0xdd, sizeof *o); free(o); atomic_fetch_add(&frees, 1); }
}

static void *worker(void *arg) {
    unsigned seed = (unsigned)(uintptr_t)arg * 7919u + 1;
    void *bag[64] = {0};
    for (int i = 0; i < ROUNDS; i++) {
        seed = seed * 1103515245u + 12345u;
        int k = (seed >> 8) & 63;
        if (bag[k]) { free(bag[k]); bag[k] = NULL; atomic_fetch_add(&frees, 1); }
        else { size_t sz = ((seed >> 14) & 0x3ff) + 1; bag[k] = malloc(sz); memset(bag[k], (int)i, sz); atomic_fetch_add(&allocs, 1); }
        int s = (seed >> 20) & 255;
        pthread_mutex_lock(&mu[s]);
        Obj *o = shared[s];
        if (o && atomic_load(&o->refs) <= 0) atomic_fetch_add(&errors, 1);
        if (!o || ((seed >> 3) & 7) == 0) {
            if (o) { shared[s] = NULL; pthread_mutex_unlock(&mu[s]); obj_put(o); }
            else { o = malloc(sizeof *o); atomic_init(&o->refs, 1); shared[s] = o; atomic_fetch_add(&allocs, 1); pthread_mutex_unlock(&mu[s]); }
        } else {
            atomic_fetch_add(&o->refs, 1);
            pthread_mutex_unlock(&mu[s]);
            uint64_t v = via_x16(&o->pad[0], (uint64_t)i);
            if (v == 0xdddddddddddddddd) atomic_fetch_add(&errors, 1);
            obj_put(o);
        }
    }
    for (int k = 0; k < 64; k++) if (bag[k]) { free(bag[k]); atomic_fetch_add(&frees, 1); }
    return NULL;
}

int main(void) {
    for (int i = 0; i < 256; i++) pthread_mutex_init(&mu[i], NULL);
    pthread_t t[THREADS];
    for (long i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, worker, (void *)i);
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    for (int s = 0; s < 256; s++) if (shared[s]) obj_put(shared[s]);
    printf("allocs %ld frees %ld errors %ld\n", atomic_load(&allocs), atomic_load(&frees), atomic_load(&errors));
    int ok = atomic_load(&errors) == 0 && atomic_load(&allocs) == atomic_load(&frees);
    printf("%s\n", ok ? "STRESS OK" : "STRESS FAILED");
    return !ok;
}
