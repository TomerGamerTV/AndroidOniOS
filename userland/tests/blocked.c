// Killing processes that are blocked in a futex wait, a pipe read and a nanosleep.
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
static void *sleeper(void *a) { (void)a; sleep(100); return NULL; }
int main(void) {
    int ok = 1, fd[2];
    pipe(fd);
    pid_t kids[3];
    for (int k = 0; k < 3; k++) {
        kids[k] = fork();
        if (kids[k] == 0) {
            if (k == 0) { pthread_t t; pthread_create(&t, NULL, sleeper, NULL); pthread_join(t, NULL); }
            if (k == 1) { char c; read(fd[0], &c, 1); }
            if (k == 2) sleep(100);
            _exit(0);
        }
    }
    usleep(100000);
    for (int k = 0; k < 3; k++) {
        kill(kids[k], k == 2 ? SIGKILL : SIGTERM);
        int st;
        waitpid(kids[k], &st, 0);
        int want = k == 2 ? SIGKILL : SIGTERM;
        if (!WIFSIGNALED(st) || WTERMSIG(st) != want) { printf("child %d status %x\n", k, st); ok = 0; }
    }
    printf("%s\n", ok ? "BLOCKED OK" : "BLOCKED FAILED");
    return !ok;
}
