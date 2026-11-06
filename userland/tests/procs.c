// Process test for onirun: fork (copy-on-write), shared memory, pipes, exec, vfork/posix_spawn,
// signals between processes, killing busy and multithreaded processes, crashes, and a zygote-style
// fan-out. All of it must run inside the one onirun host process.
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int global = 1;

static void test_cow(void) {
    int *heap = malloc(sizeof(int));
    *heap = 10;
    int stack = 100;
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        int ok = getppid() == parent && global == 1 && *heap == 10 && stack == 100;
        global = 2; *heap = 20; stack = 200;
        usleep(50000);                              // parent changes its copies meanwhile
        ok = ok && global == 2 && *heap == 20 && stack == 200;
        _exit(ok ? 7 : 1);
    }
    usleep(10000);
    global = 3; *heap = 30; stack = 300;
    int st;
    CHECK(waitpid(pid, &st, 0) == pid, "waitpid");
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7, "child saw wrong memory (status %x)", st);
    CHECK(global == 3 && *heap == 30 && stack == 300, "parent memory changed by child");
    free(heap);
    printf("fork copy-on-write: ok\n");
}

static void test_shared(void) {
    atomic_int *s = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *s = 0;
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0; i < 1000; i++) atomic_fetch_add(s, 1);
        _exit(0);
    }
    for (int i = 0; i < 1000; i++) atomic_fetch_add(s, 1);
    waitpid(pid, NULL, 0);
    CHECK(*s == 2000, "shared counter %d", *s);
    printf("MAP_SHARED across fork: %d\n", *s);
}

static void test_pipe(void) {
    int fd[2];
    pipe(fd);
    const size_t total = 4 << 20;
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        char buf[8192];
        for (size_t sent = 0; sent < total; sent += sizeof buf) {
            for (size_t i = 0; i < sizeof buf; i++) buf[i] = (char)(sent + i);
            if (write(fd[1], buf, sizeof buf) != (ssize_t)sizeof buf) _exit(2);
        }
        _exit(0);
    }
    close(fd[1]);
    char buf[65536];
    size_t got = 0;
    int bad = 0;
    ssize_t n;
    while ((n = read(fd[0], buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++) if (buf[i] != (char)(got + i)) bad++;
        got += n;
    }
    close(fd[0]);
    int st;
    waitpid(pid, &st, 0);
    CHECK(got == total && !bad && WIFEXITED(st) && WEXITSTATUS(st) == 0, "pipe got %zu bad %d", got, bad);
    printf("pipe: %zu bytes\n", got);
}

static void test_exec(void) {
    int fd[2];
    pipe(fd);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd[1], 1);
        close(fd[0]);
        close(fd[1]);
        execl("/system/bin/toybox", "echo", "from exec", NULL);
        _exit(99);
    }
    close(fd[1]);
    char buf[64] = {0};
    read(fd[0], buf, sizeof buf - 1);
    close(fd[0]);
    int st;
    waitpid(pid, &st, 0);
    CHECK(!strcmp(buf, "from exec\n") && WEXITSTATUS(st) == 0, "exec output '%s' status %x", buf, st);
    // exec failure must come back to the caller
    pid = fork();
    if (pid == 0) { execl("/no/such/program", "x", NULL); _exit(errno == ENOENT ? 3 : 4); }
    waitpid(pid, &st, 0);
    CHECK(WEXITSTATUS(st) == 3, "failed exec status %x", st);
    printf("fork+exec: ok\n");
}

static void test_spawn(void) {
    pid_t pid;
    char *argv[] = {"sh", "-c", "exit 5", NULL};
    int rc = posix_spawn(&pid, "/system/bin/sh", NULL, NULL, argv, environ);
    int st = 0;
    if (!rc) waitpid(pid, &st, 0);
    CHECK(!rc && WIFEXITED(st) && WEXITSTATUS(st) == 5, "posix_spawn rc %d status %x", rc, st);
    pid = vfork();
    if (pid == 0) _exit(6);
    waitpid(pid, &st, 0);
    CHECK(WEXITSTATUS(st) == 6, "vfork status %x", st);
    printf("posix_spawn and vfork: ok\n");
}

static volatile sig_atomic_t chld_seen;
static void on_chld(int s) { (void)s; chld_seen++; }

static void test_signals(void) {
    struct sigaction sa = {0};
    sa.sa_handler = on_chld;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    pid_t pid = fork();
    if (pid == 0) _exit(0);
    int st;
    waitpid(pid, &st, 0);
    for (int i = 0; i < 100 && !chld_seen; i++) usleep(1000);
    CHECK(chld_seen, "no SIGCHLD");
    signal(SIGCHLD, SIG_DFL);

    // A child spinning without system calls must still die from SIGTERM.
    pid = fork();
    if (pid == 0) { volatile unsigned long n = 0; for (;;) n++; }
    usleep(50000);
    double t0 = now();
    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "busy child status %x", st);
    printf("SIGCHLD handler, killing a busy child (%.1f ms): ok\n", (now() - t0) * 1e3);

    // A child crashing doesn't take the parent with it.
    pid = fork();
    if (pid == 0) { volatile int *p = NULL; *p = 1; _exit(0); }
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "crashing child status %x", st);
    printf("crashing child: ok\n");
}

static void *spin(void *a) { (void)a; volatile unsigned long n = 0; for (;;) n++; return NULL; }

static void test_threads_exit(void) {
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t t;
        for (int i = 0; i < 4; i++) pthread_create(&t, NULL, spin, NULL);
        usleep(20000);
        exit(9);                                    // exit_group must stop the spinning threads
    }
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 9, "threaded exit status %x", st);
    pid = fork();
    if (pid == 0) {
        pthread_t t;
        for (int i = 0; i < 4; i++) pthread_create(&t, NULL, spin, NULL);
        usleep(20000);
        execl("/system/bin/toybox", "true", NULL);  // exec from a multithreaded process
        _exit(98);
    }
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "threaded exec status %x", st);
    printf("exit and exec with busy threads: ok\n");
}

static void test_zygote(void) {
    // A zygote-style parent: big initialized state, many children that only read it.
    const size_t sz = 64 << 20;
    unsigned char *state = malloc(sz);
    for (size_t i = 0; i < sz; i++) state[i] = (unsigned char)(i * 31);
    unsigned long expect = 0;
    for (size_t i = 0; i < sz; i += 4096) expect += state[i];
    const int kids = 8;
    pid_t pids[8];
    double t0 = now();
    for (int k = 0; k < kids; k++) {
        pids[k] = fork();
        if (pids[k] == 0) {
            unsigned long sum = 0;
            for (size_t i = 0; i < sz; i += 4096) sum += state[i];
            state[k] = 0;                           // one private write each
            _exit(sum == expect ? 0 : 1);
        }
    }
    double t1 = now();
    int ok = 0;
    for (int k = 0; k < kids; k++) {
        int st;
        waitpid(pids[k], &st, 0);
        ok += WIFEXITED(st) && WEXITSTATUS(st) == 0;
    }
    CHECK(ok == kids && state[0] == 0 && state[1] == 31, "zygote children ok %d", ok);
    CHECK(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD, "children left over");
    printf("zygote fan-out: %d children of a 64 MiB parent, %.2f ms per fork, all done in %.1f ms\n", kids,
           (t1 - t0) * 1e3 / kids, (now() - t0) * 1e3);
    free(state);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   // children must not re-print inherited buffered output
    test_cow();
    test_shared();
    test_pipe();
    test_exec();
    test_spawn();
    test_signals();
    test_threads_exit();
    test_zygote();
    printf("%s\n", failures ? "PROCS FAILED" : "PROCS OK");
    return failures ? 1 : 0;
}
