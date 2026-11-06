// File-descriptor objects: eventfd, epoll, memfd and ashmem, and the /dev nodes Android opens.
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sys.h"

// ---- objects ----
static FdObj *reg;                       // live piped objects, for obj_by_fd
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
FdObj *obj_new(const FdOps *ops, void *data, bool piped) {
    FdObj *o = calloc(1, sizeof *o);
    o->ops = ops;
    o->data = data;
    o->refs = 1;
    o->rd = o->wr = o->mmap_fd = -1;
    pthread_mutex_init(&o->lock, NULL);
    if (piped) {
        int p[2];
        if (pipe(p)) { free(o); return NULL; }
        o->rd = p[0];
        o->wr = p[1];
        fcntl(o->wr, F_SETFD, FD_CLOEXEC);
        fcntl(o->rd, F_SETFD, FD_CLOEXEC);
        struct stat st;
        if (!fstat(o->rd, &st)) o->ino = st.st_ino;
        pthread_mutex_lock(&reg_lock);
        o->next_reg = reg;
        reg = o;
        pthread_mutex_unlock(&reg_lock);
    }
    return o;
}

// A host fd that arrived by SCM_RIGHTS (or any dup) is a new number for the same pipe; the inode
// says which object it belongs to.
FdObj *obj_by_fd(int host) {
    struct stat st;
    if (host < 0 || fstat(host, &st) || !S_ISFIFO(st.st_mode)) return NULL;
    pthread_mutex_lock(&reg_lock);
    FdObj *o = reg;
    while (o && o->ino != st.st_ino) o = o->next_reg;
    pthread_mutex_unlock(&reg_lock);
    return o;
}

void obj_get(FdObj *o) { __atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST); }

static void obj_free(FdObj *o);

// Releases that would run while the caller holds g_lock (closing a guest fd) are handed to this
// thread instead: a release may take its own object's locks (the GPU's, binder's), and those
// paths take g_lock in turn, so running it under g_lock deadlocks (seen: fd_close vs a KMS ioctl).
static FdObj *reap_head;
static pthread_mutex_t reap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reap_cond = PTHREAD_COND_INITIALIZER;
static void *reaper(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&reap_lock);
        while (!reap_head) pthread_cond_wait(&reap_cond, &reap_lock);
        FdObj *o = reap_head;
        reap_head = o->next_reap;
        pthread_mutex_unlock(&reap_lock);
        obj_free(o);
    }
    return NULL;
}
static pthread_once_t reaper_once = PTHREAD_ONCE_INIT;
static void reaper_start(void) {
    pthread_t t;
    pthread_create(&t, NULL, reaper, NULL);
    pthread_detach(t);
}
void obj_put_deferred(FdObj *o) {
    if (!o || __atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST)) return;
    pthread_once(&reaper_once, reaper_start);
    pthread_mutex_lock(&reap_lock);
    o->next_reap = reap_head;
    reap_head = o;
    pthread_cond_signal(&reap_cond);
    pthread_mutex_unlock(&reap_lock);
}

void obj_put(FdObj *o) {
    if (!o || __atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST)) return;
    obj_free(o);
}
static void obj_free(FdObj *o) {
    if (o->rd >= 0) {
        pthread_mutex_lock(&reg_lock);
        for (FdObj **pp = &reg; *pp; pp = &(*pp)->next_reg) if (*pp == o) { *pp = o->next_reg; break; }
        pthread_mutex_unlock(&reg_lock);
    }
    if (o->ops->release) o->ops->release(o);
    if (o->wr >= 0) close(o->wr);
    if (o->mmap_fd >= 0) close(o->mmap_fd);
    pthread_mutex_destroy(&o->lock);
    free(o);
}

void obj_set_ready(FdObj *o, bool ready) {
    if (ready == o->ready || o->wr < 0) { o->ready = ready; return; }
    o->ready = ready;
    char b = 0x7e;                       // a recognizable byte, in case it ever lands where it shouldn't
    if (ready) write(o->wr, &b, 1);
    else read(o->rd, &b, 1);
}

int obj_wait(int host, int events, bool nonblock) {
    if (nonblock) return -EAGAIN;
    struct pollfd pf = { host, (short)events, 0 };
    int r = poll(&pf, 1, -1);
    if (r < 0) return errno == EINTR ? -EINTR : -errno;
    return 0;
}

static bool fd_nonblock(int host) {
    int f = fcntl(host, F_GETFL);
    return f >= 0 && (f & O_NONBLOCK);
}

int host_tmpfile(void) {
    const char *dir = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/onirun.XXXXXX", dir && *dir ? dir : "/tmp");
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    unlink(path);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

// ---- eventfd ----
typedef struct { uint64_t count; bool semaphore; } EventFd;

static int64_t eventfd_read(FdObj *o, GuestCtx *c, void *buf, size_t n, bool nonblock) {
    (void)c;
    EventFd *e = o->data;
    if (n < 8) return -EINVAL;
    for (;;) {
        pthread_mutex_lock(&o->lock);
        if (e->count) {
            uint64_t v = e->semaphore ? 1 : e->count;
            e->count -= v;
            obj_set_ready(o, e->count != 0);
            pthread_mutex_unlock(&o->lock);
            memcpy(buf, &v, 8);
            return 8;
        }
        pthread_mutex_unlock(&o->lock);
        int r = obj_wait(o->rd, POLLIN, nonblock);
        if (r) return r;
    }
}

static int64_t eventfd_write(FdObj *o, GuestCtx *c, const void *buf, size_t n, bool nonblock) {
    (void)c; (void)nonblock;
    EventFd *e = o->data;
    uint64_t v;
    if (n < 8) return -EINVAL;
    memcpy(&v, buf, 8);
    if (v == ~0ull) return -EINVAL;
    pthread_mutex_lock(&o->lock);
    if (e->count + v < e->count || e->count + v == ~0ull) { pthread_mutex_unlock(&o->lock); return -EAGAIN; }
    e->count += v;
    obj_set_ready(o, true);
    pthread_mutex_unlock(&o->lock);
    return 8;
}

static void data_release(FdObj *o) { free(o->data); }
static const FdOps eventfd_ops = { "eventfd", eventfd_read, eventfd_write, NULL, NULL, data_release };

int64_t vfs_eventfd(GuestCtx *c, uint64_t count, int lflags) {
    EventFd *e = calloc(1, sizeof *e);
    e->count = count;
    e->semaphore = lflags & 1;                      // EFD_SEMAPHORE
    FdObj *o = obj_new(&eventfd_ops, e, true);
    if (!o) { free(e); return -ENFILE; }
    if (count) obj_set_ready(o, true);
    if (lflags & 0x800) fcntl(o->rd, F_SETFL, O_NONBLOCK);
    int g = fd_install_obj(c->proc, o->rd, o, lflags & 0x80000);
    return g;
}

// ---- timerfd, on a libdispatch timer (available on iOS too) ----
typedef struct TimerFd {
    pthread_mutex_t mu;
    FdObj *o;
    dispatch_source_t src;
    uint64_t expirations;
    int64_t deadline_ns, interval_ns;      // CLOCK_MONOTONIC deadline, 0 = disarmed
    bool dead;
} TimerFd;

static int64_t now_ns(void) { return (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC); }

static void timer_disarm(TimerFd *t) {
    if (t->src) { dispatch_source_cancel(t->src); dispatch_release(t->src); t->src = NULL; }
    t->deadline_ns = 0;
}

static int64_t timerfd_read(FdObj *o, GuestCtx *c, void *buf, size_t n, bool nonblock) {
    (void)c;
    TimerFd *t = o->data;
    if (n < 8) return -EINVAL;
    for (;;) {
        pthread_mutex_lock(&t->mu);
        if (t->expirations) {
            uint64_t v = t->expirations;
            t->expirations = 0;
            pthread_mutex_lock(&o->lock);
            obj_set_ready(o, false);
            pthread_mutex_unlock(&o->lock);
            pthread_mutex_unlock(&t->mu);
            memcpy(buf, &v, 8);
            return 8;
        }
        pthread_mutex_unlock(&t->mu);
        int r = obj_wait(o->rd, POLLIN, nonblock);
        if (r) return r;
    }
}

static void timerfd_release(FdObj *o) {
    TimerFd *t = o->data;
    pthread_mutex_lock(&t->mu);
    t->dead = true;
    dispatch_source_t src = t->src;
    t->src = NULL;
    pthread_mutex_unlock(&t->mu);
    if (src) {
        dispatch_source_set_cancel_handler(src, ^{ pthread_mutex_destroy(&t->mu); free(t); });
        dispatch_source_cancel(src);
        dispatch_release(src);
    } else {
        pthread_mutex_destroy(&t->mu);
        free(t);
    }
}
static const FdOps timerfd_ops = { "timerfd", timerfd_read, NULL, NULL, NULL, timerfd_release };

int64_t vfs_timerfd_create(GuestCtx *c, int clockid, int lflags) {
    if (clockid != 0 && clockid != 1 && clockid != 7 && clockid != 8 && clockid != 9) return -EINVAL;
    TimerFd *t = calloc(1, sizeof *t);
    pthread_mutex_init(&t->mu, NULL);
    FdObj *o = obj_new(&timerfd_ops, t, true);
    if (!o) { free(t); return -ENFILE; }
    t->o = o;
    if (lflags & 0x800) fcntl(o->rd, F_SETFL, O_NONBLOCK);
    fd_set_path(o->rd, "anon_inode:[timerfd]");
    return fd_install_obj(c->proc, o->rd, o, lflags & 0x80000);
}

static TimerFd *timerfd_of(Proc *p, int gfd) {
    int h = fd_host(p, gfd);
    FdObj *o = h >= 0 ? fd_obj(h) : NULL;
    return o && o->ops == &timerfd_ops ? o->data : NULL;
}

static void put_itimerspec(uint64_t addr, int64_t value_ns, int64_t interval_ns) {
    int64_t *it = gptr(addr);
    if (!it) return;
    it[0] = interval_ns / 1000000000; it[1] = interval_ns % 1000000000;
    it[2] = value_ns / 1000000000; it[3] = value_ns % 1000000000;
}

int64_t vfs_timerfd_settime(GuestCtx *c, int gfd, int lflags, uint64_t newv, uint64_t oldv) {
    TimerFd *t = timerfd_of(c->proc, gfd);
    if (!t) return -EINVAL;
    const int64_t *it = gptr(newv);
    if (!it) return -EFAULT;
    int64_t interval = it[0] * 1000000000 + it[1], value = it[2] * 1000000000 + it[3];
    if (it[1] < 0 || it[1] >= 1000000000 || it[3] < 0 || it[3] >= 1000000000) return -EINVAL;
    pthread_mutex_lock(&t->mu);
    if (oldv) {
        int64_t rem = t->deadline_ns ? t->deadline_ns - now_ns() : 0;
        put_itimerspec(oldv, rem > 0 ? rem : 0, t->interval_ns);
    }
    timer_disarm(t);
    t->expirations = 0;
    pthread_mutex_lock(&t->o->lock);
    obj_set_ready(t->o, false);
    pthread_mutex_unlock(&t->o->lock);
    if (value) {
        int64_t delay = value;
        if (lflags & 1) {                                   // TFD_TIMER_ABSTIME, against the timer's clock
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);             // absolute realtime and monotonic differ; use realtime
            int64_t now_rt = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
            delay = value - now_rt;
            if (delay < 0) delay = 0;
        }
        t->interval_ns = interval;
        t->deadline_ns = now_ns() + delay;
        dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0));
        dispatch_source_set_timer(src, dispatch_time(DISPATCH_TIME_NOW, delay), interval ? (uint64_t)interval : DISPATCH_TIME_FOREVER, 500000);
        dispatch_source_set_event_handler(src, ^{
            pthread_mutex_lock(&t->mu);
            if (!t->dead) {
                t->expirations += dispatch_source_get_data(src) ? dispatch_source_get_data(src) : 1;
                t->deadline_ns = t->interval_ns ? now_ns() + t->interval_ns : 0;
                pthread_mutex_lock(&t->o->lock);
                obj_set_ready(t->o, true);
                pthread_mutex_unlock(&t->o->lock);
            }
            pthread_mutex_unlock(&t->mu);
        });
        t->src = src;
        dispatch_resume(src);
    } else {
        t->interval_ns = interval;
    }
    pthread_mutex_unlock(&t->mu);
    return 0;
}

int64_t vfs_timerfd_gettime(GuestCtx *c, int gfd, uint64_t curv) {
    TimerFd *t = timerfd_of(c->proc, gfd);
    if (!t) return -EINVAL;
    pthread_mutex_lock(&t->mu);
    int64_t rem = t->deadline_ns ? t->deadline_ns - now_ns() : 0;
    put_itimerspec(curv, rem > 0 ? rem : 0, t->deadline_ns ? t->interval_ns : 0);
    pthread_mutex_unlock(&t->mu);
    return 0;
}

// ---- memfd and ashmem ----
typedef struct { char name[256]; uint32_t prot_mask; } Ashmem;

// memfd: an unlinked host file plus the seal state (F_ADD_SEALS / F_GET_SEALS), which libcutils
// uses to tell memfds from ashmem.
static const FdOps memfd_ops = { "memfd", NULL, NULL, NULL, NULL, NULL };
int64_t vfs_memfd(GuestCtx *c, const char *name, int lflags) {
    int fd = host_tmpfile();
    if (fd < 0) return -errno;
    char path[300];
    snprintf(path, sizeof path, "/memfd:%s", name);
    fd_set_path(fd, path);
    FdObj *o = obj_new(&memfd_ops, NULL, false);
    o->mmap_fd = dup(fd);
    o->data = (void *)(uintptr_t)((lflags & 2) ? 0 : 1);   // without MFD_ALLOW_SEALING, F_SEAL_SEAL is set
    return fd_install_obj(c->proc, fd, o, lflags & 1);      // MFD_CLOEXEC
}

// F_ADD_SEALS (1033) / F_GET_SEALS (1034). Returns VFS_PASS for fds that aren't memfds.
int64_t vfs_memfd_seals(int host, int cmd, uint64_t arg) {
    FdObj *o = fd_obj(host);
    if (!o || o->ops != &memfd_ops) return -EINVAL;
    uint64_t seals = (uintptr_t)o->data;
    if (cmd == 1034) return (int64_t)seals;
    if (seals & 1) return -EPERM;                           // F_SEAL_SEAL
    o->data = (void *)(uintptr_t)(seals | (arg & 0x1f));
    return 0;
}

static int64_t ashmem_ioctl(FdObj *o, GuestCtx *c, uint64_t req, uint64_t arg) {
    Ashmem *a = o->data;
    struct stat st;
    if (((req >> 8) & 0xff) != 0x77) return -ENOTTY;
    switch (req & 0xff) {
    case 1: snprintf(a->name, sizeof a->name, "%s", gstr(arg)); return 0;          // SET_NAME
    case 2: strncpy(gptr(arg), a->name[0] ? a->name : "dev/ashmem", 256); return 0; // GET_NAME
    case 3: return ftruncate(o->mmap_fd, (off_t)arg) ? -errno : 0;        // SET_SIZE
    case 4: return fstat(o->mmap_fd, &st) ? -errno : st.st_size;          // GET_SIZE
    case 5: a->prot_mask &= (uint32_t)arg; return 0;                                // SET_PROT_MASK
    case 6: return a->prot_mask;                                                    // GET_PROT_MASK
    case 7: case 8: return 0;                                                       // PIN/UNPIN: ASHMEM_NOT_PURGED
    case 9: return 0;                                                               // GET_PIN_STATUS: pinned
    case 10: return 0;                                                              // PURGE_ALL_CACHES
    case 11: *(uint64_t *)gptr(arg) = fstat(o->mmap_fd, &st) ? 0 : st.st_ino; return 0;   // GET_FILE_ID
    }
    (void)c;
    return -ENOTTY;
}
static const FdOps ashmem_ops = { "ashmem", NULL, NULL, ashmem_ioctl, NULL, data_release };

static int ashmem_open(Proc *p, int lflags) {
    int fd = host_tmpfile();
    if (fd < 0) return -errno;
    Ashmem *a = calloc(1, sizeof *a);
    a->prot_mask = 7;
    FdObj *o = obj_new(&ashmem_ops, a, false);
    o->mmap_fd = dup(fd);
    fd_set_path(fd, "/dev/ashmem");
    return fd_install_obj(p, fd, o, lflags & 02000000);
}

// ---- epoll ----
typedef struct { int gfd, host; uint32_t events; uint64_t data; bool disabled; } EpEnt;
typedef struct { EpEnt *e; int n, cap; } Epoll;

static void epoll_release(FdObj *o) { Epoll *ep = o->data; free(ep->e); free(ep); }
static const FdOps epoll_ops = { "epoll", NULL, NULL, NULL, NULL, epoll_release };

int64_t vfs_epoll_create(GuestCtx *c, int lflags) {
    Epoll *ep = calloc(1, sizeof *ep);
    FdObj *o = obj_new(&epoll_ops, ep, true);
    if (!o) { free(ep); return -ENFILE; }
    int g = fd_install_obj(c->proc, o->rd, o, lflags & 0x80000);
    if (g >= 0) LOCKED(c->proc->nepoll++);
    return g;
}

static FdObj *epoll_of(Proc *p, int epfd) {
    int h = fd_host(p, epfd);
    FdObj *o = h >= 0 ? fd_obj(h) : NULL;
    return o && o->ops == &epoll_ops ? o : NULL;
}

int64_t vfs_epoll_ctl(GuestCtx *c, int epfd, int op, int fd, uint64_t event) {
    FdObj *o = epoll_of(c->proc, epfd);
    if (!o) return -EINVAL;
    int h = fd_host(c->proc, fd);
    if (h < 0) return -EBADF;
    Epoll *ep = o->data;
    uint32_t events = 0;
    uint64_t data = 0;
    if (op != 2) {
        const uint8_t *ev = gptr(event);
        if (!ev) return -EFAULT;
        memcpy(&events, ev, 4);
        memcpy(&data, ev + 8, 8);
    }
    int64_t r = 0;
    pthread_mutex_lock(&o->lock);
    int i = 0;
    while (i < ep->n && ep->e[i].gfd != fd) i++;
    switch (op) {
    case 1:                                          // EPOLL_CTL_ADD
        if (i < ep->n) { r = -EEXIST; break; }
        if (ep->n == ep->cap) { ep->cap = ep->cap ? ep->cap * 2 : 8; ep->e = realloc(ep->e, (size_t)ep->cap * sizeof(EpEnt)); }
        ep->e[ep->n++] = (EpEnt){ fd, h, events, data, false };
        break;
    case 2:                                          // EPOLL_CTL_DEL
        if (i == ep->n) { r = -ENOENT; break; }
        ep->e[i] = ep->e[--ep->n];
        break;
    case 3:                                          // EPOLL_CTL_MOD
        if (i == ep->n) { r = -ENOENT; break; }
        ep->e[i].events = events;
        ep->e[i].data = data;
        ep->e[i].host = h;
        ep->e[i].disabled = false;
        break;
    default: r = -EINVAL;
    }
    pthread_mutex_unlock(&o->lock);
    return r;
}

void vfs_fd_closed(Proc *p, int gfd) {
    if (!p->nepoll) return;
    for (int i = 0; i < MAX_FDS; i++) {
        int h = p->fd[i].host;
        FdObj *o = h >= 0 ? fd_obj(h) : NULL;
        if (!o || o->ops != &epoll_ops) continue;
        Epoll *ep = o->data;
        pthread_mutex_lock(&o->lock);
        for (int k = 0; k < ep->n; k++)
            if (ep->e[k].gfd == gfd) { ep->e[k] = ep->e[--ep->n]; break; }
        pthread_mutex_unlock(&o->lock);
    }
}

int64_t vfs_epoll_wait(GuestCtx *c, int epfd, uint64_t events, int maxevents, int64_t timeout_ms, uint64_t sigmask) {
    FdObj *o = epoll_of(c->proc, epfd);
    if (!o) return -EINVAL;
    if (maxevents <= 0) return -EINVAL;
    Epoll *ep = o->data;
    uint64_t saved = c->sigmask;
    if (sigmask) c->sigmask = *(uint64_t *)gptr(sigmask) & ~((1ull << 8) | (1ull << 18));
    pthread_mutex_lock(&o->lock);
    int n = ep->n;
    struct pollfd *pf = calloc((size_t)n + 1, sizeof *pf);
    EpEnt *ents = calloc((size_t)n + 1, sizeof *ents);
    memcpy(ents, ep->e, (size_t)n * sizeof(EpEnt));
    pthread_mutex_unlock(&o->lock);
    for (int i = 0; i < n; i++) {
        pf[i].fd = ents[i].disabled ? -1 : ents[i].host;
        pf[i].events = (short)(ents[i].events & 0x3ff);
    }
    int64_t r;
    int rc = poll(pf, (nfds_t)n, timeout_ms < 0 ? -1 : (int)(timeout_ms > 0x7fffffff ? 0x7fffffff : timeout_ms));
    if (rc < 0) { r = errno == EINTR ? -EINTR : -errno; goto out; }
    uint8_t *out = gptr(events);
    int k = 0;
    pthread_mutex_lock(&o->lock);
    for (int i = 0; i < n && k < maxevents; i++) {
        if (!pf[i].revents) continue;
        uint32_t ev = (uint32_t)pf[i].revents & 0x3ff;
        if (ev & POLLNVAL) {                          // closed behind our back: forget it
            for (int j = 0; j < ep->n; j++) if (ep->e[j].gfd == ents[i].gfd) { ep->e[j] = ep->e[--ep->n]; break; }
            continue;
        }
        memcpy(out + 16 * k, &ev, 4);
        memcpy(out + 16 * k + 8, &ents[i].data, 8);
        k++;
        if (ents[i].events & (1u << 30))              // EPOLLONESHOT
            for (int j = 0; j < ep->n; j++) if (ep->e[j].gfd == ents[i].gfd) ep->e[j].disabled = true;
    }
    pthread_mutex_unlock(&o->lock);
    r = k;
out:
    free(pf);
    free(ents);
    c->sigmask = saved;
    return r;
}

// ---- /dev/kmsg: Android's kernel-log writers (libbase's KernelLogger) end up on stderr ----
static int64_t kmsg_write(FdObj *o, GuestCtx *c, const void *buf, size_t n, bool nonblock) {
    (void)o; (void)nonblock;
    const char *s = buf;
    size_t k = n;
    while (k && (s[k - 1] == '\n' || s[k - 1] == 0)) k--;
    if (*s == '<') { const char *e = memchr(s, '>', k); if (e) { k -= (size_t)(e + 1 - s); s = e + 1; } }   // "<prio>" prefix
    fprintf(stderr, "kmsg[%d]: %.*s\n", c->proc->pid, (int)k, s);
    return (int64_t)n;
}
static const FdOps kmsg_ops = { "kmsg", NULL, kmsg_write, NULL, NULL, NULL };

// ---- inotify: nothing in the rootfs changes behind the guest's back, so watches never fire ----
static const FdOps inotify_ops = { "inotify", NULL, NULL, NULL, NULL, NULL };
int64_t vfs_inotify_init(GuestCtx *c, int lflags) {
    FdObj *o = obj_new(&inotify_ops, NULL, true);
    if (!o) return -ENFILE;
    if (lflags & 0x800) fcntl(o->rd, F_SETFL, O_NONBLOCK);
    fd_set_path(o->rd, "anon_inode:inotify");
    return fd_install_obj(c->proc, o->rd, o, lflags & 0x80000);
}

// ---- /dev ----
int vfs_open_dev(Proc *p, const char *path, int lflags) {
    if (!strncmp(path, "/proc", 5) || !strncmp(path, "/sys/", 5)) return procfs_open(p, path, lflags);
    if (strncmp(path, "/dev/", 5)) return (int)1;
    const char *n = path + 5;
    if (!strncmp(n, "ashmem", 6)) return ashmem_open(p, lflags);
    if (!strcmp(n, "kmsg")) {
        FdObj *o = obj_new(&kmsg_ops, NULL, true);
        if (!o) return -ENFILE;
        fd_set_path(o->rd, path);
        return fd_install_obj(p, o->rd, o, lflags & 02000000);
    }
    if (!strcmp(n, "binder") || !strcmp(n, "binderfs/binder")) return binder_open(p, "binder", lflags);
    if (!strcmp(n, "hwbinder") || !strcmp(n, "binderfs/hwbinder")) return binder_open(p, "hwbinder", lflags);
    if (!strcmp(n, "vndbinder") || !strcmp(n, "binderfs/vndbinder")) return binder_open(p, "vndbinder", lflags);
    if (!strncmp(n, "goldfish_pipe", 13) || !strcmp(n, "qemu_pipe")) return goldfish_pipe_open(p, path, lflags);
    if (!strcmp(n, "tun") || !strcmp(n, "net/tun")) return tun_open(p, path, lflags);
    if (!strcmp(n, "dri/renderD128")) return gpu_open(p, false, lflags);
    if (!strcmp(n, "dri/card0")) return gpu_open(p, true, lflags);
    return 1;
}
// (1 above means "not a device"; callers compare against 1, never against a guest fd of 1, because
// a guest fd is only ever returned for the device paths matched here.)

// Generic object dispatch used by the system-call layer.
int64_t vfs_read(GuestCtx *c, int host, void *buf, size_t n) {
    FdObj *o = fd_obj(host);
    if (!o || !o->ops->read) return VFS_PASS;
    return o->ops->read(o, c, buf, n, fd_nonblock(host));
}
int64_t vfs_write(GuestCtx *c, int host, const void *buf, size_t n) {
    FdObj *o = fd_obj(host);
    if (!o || !o->ops->write) return VFS_PASS;
    return o->ops->write(o, c, buf, n, fd_nonblock(host));
}
int64_t vfs_ioctl(GuestCtx *c, int host, uint64_t req, uint64_t arg) {
    FdObj *o = fd_obj(host);
    if (!o || !o->ops->ioctl) return VFS_PASS;
    return o->ops->ioctl(o, c, req, arg);
}
