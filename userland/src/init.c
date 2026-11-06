// init-lite: the part of Android's init that services need. A service table (see data/init.txt)
// says what to start; property writes to ctl.start/stop/restart and ctl.interface_start (how
// servicemanager asks for lazy AIDL services) start and stop them; services that die are
// restarted unless marked oneshot; init.svc.<name> properties track their state.
//
//   service NAME /path/program args...     define a service
//     socket NAME                          (after a service) create /dev/socket/NAME for it
//     oneshot                              (after a service) don't restart it when it exits
//   interface IFACE NAME                   ctl.interface_start=IFACE starts service NAME
//   start NAME                             start at boot, in order
//   env KEY=VALUE                          extra environment for every service
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sys.h"

typedef struct Svc {
    char name[64];
    char *argv[32];
    int argc;
    const char *sockets[8];
    int nsockets;
    bool oneshot, disabled;
    int pid;                  // 0 = not running
    int64_t started_ns;
    int restarts;
    bool want;                // should be running
    struct Svc *next;
} Svc;
typedef struct Iface { char iface[96]; char svc[64]; struct Iface *next; } Iface;

static Svc *svcs;
static Iface *ifaces;
static char *extra_env[64];
static int n_extra_env;
static char *boot_order[64];
static int n_boot;
static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static char **base_env;

static Svc *svc_find(const char *name) {
    for (Svc *s = svcs; s; s = s->next) if (!strcmp(s->name, name)) return s;
    return NULL;
}

static void set_state(Svc *s, const char *state) {
    char key[96];
    snprintf(key, sizeof key, "init.svc.%s", s->name);
    props_set(key, state);
}

static void svc_launch_locked(Svc *s) {
    char *env[256];
    int ne = 0;
    for (; base_env[ne] && ne < 180; ne++) env[ne] = base_env[ne];
    for (int i = 0; i < n_extra_env && ne < 250; i++) env[ne++] = extra_env[i];
    env[ne] = NULL;
    Proc *p = proc_spawn(s->argv[0], s->argv, env, s->sockets);
    if (!p) { fprintf(stderr, "onirun: init: cannot start %s\n", s->name); s->disabled = true; set_state(s, "stopped"); return; }
    s->pid = p->pid;
    s->started_ns = (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC);
    set_state(s, "running");
    LOGV("onirun: init: started %s as pid %d\n", s->name, s->pid);
}

static bool svc_alive(Svc *s) {
    if (!s->pid) return false;
    Proc *p;
    LOCKED(p = proc_find(s->pid));
    return p && p->state == P_RUNNING;
}

int init_control(const char *what, const char *arg) {   // ctl.<what>=<arg>; 0 or -1
    pthread_mutex_lock(&init_lock);
    const char *target = arg;
    if (!strncmp(what, "interface_", 10)) {
        target = NULL;
        for (Iface *i = ifaces; i; i = i->next) if (!strcmp(i->iface, arg)) target = i->svc;
        what += 10;
        if (!target) { pthread_mutex_unlock(&init_lock); LOGV("onirun: init: no service for interface %s\n", arg); return -1; }
    }
    Svc *s = svc_find(target);
    if (!s) { pthread_mutex_unlock(&init_lock); LOGV("onirun: init: unknown service %s\n", target); return -1; }
    bool alive = svc_alive(s);
    if (!strcmp(what, "start")) {
        s->want = true;
        s->disabled = false;
        if (!alive) svc_launch_locked(s);
    } else if (!strcmp(what, "stop") || !strcmp(what, "restart")) {
        s->want = !strcmp(what, "restart");
        if (alive) { Proc *p; LOCKED(p = proc_find(s->pid)); if (p) { pthread_mutex_lock(&g_lock); proc_kill_locked(p, 9); pthread_mutex_unlock(&g_lock); } }
        s->pid = 0;
        set_state(s, "stopped");
        if (s->want) svc_launch_locked(s);
    }
    pthread_mutex_unlock(&init_lock);
    return 0;
}

// Watches services: notes exits, restarts the ones that should run.
static void *init_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < n_boot; i++) {
        pthread_mutex_lock(&init_lock);
        Svc *s = svc_find(boot_order[i]);
        if (s) { s->want = true; svc_launch_locked(s); }
        else fprintf(stderr, "onirun: init: start %s: no such service\n", boot_order[i]);
        pthread_mutex_unlock(&init_lock);
        usleep(300000);
    }
    for (;;) {
        usleep(500000);
        pthread_mutex_lock(&init_lock);
        for (Svc *s = svcs; s; s = s->next) {
            if (!s->pid || svc_alive(s)) continue;
            int64_t ran = (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC) - s->started_ns;
            fprintf(stderr, "onirun: init: service %s (pid %d) exited after %.1f s\n", s->name, s->pid, ran / 1e9);
            s->pid = 0;
            set_state(s, "stopped");
            if (s->oneshot || !s->want || s->disabled) continue;
            if (ran < 5000000000ll && ++s->restarts > 5) { fprintf(stderr, "onirun: init: %s keeps dying; giving up\n", s->name); s->disabled = true; continue; }
            if (ran >= 5000000000ll) s->restarts = 0;
            set_state(s, "restarting");
            pthread_mutex_unlock(&init_lock);
            usleep(1000000);
            pthread_mutex_lock(&init_lock);
            if (!s->pid && s->want && !s->disabled) svc_launch_locked(s);
        }
        pthread_mutex_unlock(&init_lock);
    }
    return NULL;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

// Parses the table and starts the boot services. Returns false if the file can't be read.
bool init_start(const char *table_host_path, char **envp) {
    FILE *f = fopen(table_host_path, "r");
    if (!f) return false;
    base_env = envp;
    Svc *cur = NULL, *tail = NULL;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        char *save;
        char *kw = strtok_r(l, " \t", &save);
        if (!kw) continue;
        if (!strcmp(kw, "service")) {
            char *name = strtok_r(NULL, " \t", &save);
            Svc *s = calloc(1, sizeof *s);
            snprintf(s->name, sizeof s->name, "%s", name ? name : "?");
            char *a;
            while ((a = strtok_r(NULL, " \t", &save)) && s->argc < 31) s->argv[s->argc++] = strdup(a);
            s->argv[s->argc] = NULL;
            if (tail) tail->next = s; else svcs = s;
            tail = cur = s;
        } else if (!strcmp(kw, "socket") && cur) {
            char *name = strtok_r(NULL, " \t", &save);
            if (name && cur->nsockets < 7) cur->sockets[cur->nsockets++] = strdup(name);
        } else if (!strcmp(kw, "oneshot") && cur) {
            cur->oneshot = true;
        } else if (!strcmp(kw, "interface")) {
            char *iface = strtok_r(NULL, " \t", &save), *svc = strtok_r(NULL, " \t", &save);
            if (iface && svc) {
                Iface *i = calloc(1, sizeof *i);
                snprintf(i->iface, sizeof i->iface, "%s", iface);
                snprintf(i->svc, sizeof i->svc, "%s", svc);
                i->next = ifaces;
                ifaces = i;
            }
        } else if (!strcmp(kw, "start")) {
            char *name = strtok_r(NULL, " \t", &save);
            if (name && n_boot < 63) boot_order[n_boot++] = strdup(name);
        } else if (!strcmp(kw, "env")) {
            char *kv = strtok_r(NULL, " \t", &save);
            if (kv && n_extra_env < 63) extra_env[n_extra_env++] = strdup(kv);
        } else {
            fprintf(stderr, "onirun: init: unknown directive %s\n", kw);
        }
    }
    fclose(f);
    for (Svc *s = svcs; s; s = s->next) set_state(s, "stopped");
    pthread_t t;
    pthread_create(&t, NULL, init_thread, NULL);
    pthread_detach(t);
    return true;
}
