// Thread test for onirun: pthread create/join, mutex + condvar (incl. timed wait), atomics,
// thread-locals, and a signal delivered to one specific thread.
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define THREADS 8
#define ITERS 200000

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long locked_counter;
static atomic_long atomic_counter;
static int ready;
static __thread int tls_value;
static atomic_int got_signal_tid;

static void *worker(void *arg) {
    int id = (int)(long)arg;
    tls_value = id * 1000;
    pthread_mutex_lock(&mu);
    while (!ready) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < ITERS; i++) {
        pthread_mutex_lock(&mu);
        locked_counter++;
        pthread_mutex_unlock(&mu);
        atomic_fetch_add(&atomic_counter, 1);
    }
    if (tls_value != id * 1000) return (void *)1;
    return (void *)(long)(id + 100);
}

static void on_usr1(int sig) { (void)sig; atomic_store(&got_signal_tid, gettid()); }

static void *sleeper(void *arg) {
    atomic_int *tid = arg;
    atomic_store(tid, gettid());
    for (int i = 0; i < 200 && !atomic_load(&got_signal_tid); i++) usleep(10000);
    return NULL;
}

int main(void) {
    pthread_t t[THREADS];
    for (long i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, worker, (void *)i);
    usleep(20000);
    pthread_mutex_lock(&mu);
    ready = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    int ok = 1;
    for (int i = 0; i < THREADS; i++) {
        void *ret;
        pthread_join(t[i], &ret);
        if ((long)ret != i + 100) { printf("thread %d returned %ld\n", i, (long)ret); ok = 0; }
    }
    long want = (long)THREADS * ITERS;
    printf("mutex counter %ld/%ld, atomic counter %ld/%ld\n", locked_counter, want,
           (long)atomic_load(&atomic_counter), want);
    if (locked_counter != want || atomic_load(&atomic_counter) != want) ok = 0;

    // Timed wait must time out (~50 ms).
    struct timespec ts, t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 50 * 1000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&mu);
    int rc = pthread_cond_timedwait(&cv, &mu, &ts);
    pthread_mutex_unlock(&mu);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    printf("timedwait: %s after %ld ms\n", rc == ETIMEDOUT ? "ETIMEDOUT" : strerror(rc), ms);
    if (rc != ETIMEDOUT || ms < 40 || ms > 500) ok = 0;

    // Signal one specific thread.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    atomic_int sleeper_tid = 0;
    pthread_t s;
    pthread_create(&s, NULL, sleeper, &sleeper_tid);
    while (!atomic_load(&sleeper_tid)) usleep(1000);
    pthread_kill(s, SIGUSR1);
    pthread_join(s, NULL);
    printf("SIGUSR1 handled on thread %d (target %d)\n", atomic_load(&got_signal_tid), atomic_load(&sleeper_tid));
    if (atomic_load(&got_signal_tid) != atomic_load(&sleeper_tid)) ok = 0;

    printf("%s\n", ok ? "THREADS OK" : "THREADS FAILED");
    return ok ? 0 : 1;
}
