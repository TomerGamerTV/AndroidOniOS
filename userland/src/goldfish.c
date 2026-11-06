// Goldfish "qemud" pipes (/dev/goldfish_pipe*): the emulator's channel from guest HALs to host
// services. A client connects by writing "pipe:<service>[:args]\0"; qemud channels then frame every
// message with a 4-hex-digit length. Only what the emulator image's HALs need to start is served:
// the sensors service answers "list-sensors" with an empty mask, so the sensors HAL comes up with
// no sensors instead of aborting.
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include "sys.h"

typedef struct {
    bool connected;
    char svc[64];
    uint8_t in[4096];
    size_t inlen;
    uint8_t *out;
    size_t outlen, outcap;
} GPipe;

static void gp_queue(FdObj *o, GPipe *g, const char *msg) {    // o->lock held
    size_t n = strlen(msg);
    char hdr[8];
    snprintf(hdr, sizeof hdr, "%04zx", n);
    if (g->outlen + n + 4 > g->outcap) { g->outcap = (g->outlen + n + 4) * 2; g->out = realloc(g->out, g->outcap); }
    memcpy(g->out + g->outlen, hdr, 4);
    memcpy(g->out + g->outlen + 4, msg, n);
    g->outlen += n + 4;
    obj_set_ready(o, true);
}

static void gp_message(FdObj *o, GPipe *g, const char *msg) {
    if (!strcmp(g->svc, "qemud:sensors")) {
        if (!strncmp(msg, "list-sensors", 12)) gp_queue(o, g, "0");
        // set:<sensor>:<on>, set-delay:<ms>, time:<ns>, wake: nothing to report
    }
}

static int64_t gp_read(FdObj *o, GuestCtx *c, void *buf, size_t n, bool nonblock) {
    (void)c;
    GPipe *g = o->data;
    for (;;) {
        pthread_mutex_lock(&o->lock);
        if (g->outlen) {
            size_t k = n < g->outlen ? n : g->outlen;
            memcpy(buf, g->out, k);
            memmove(g->out, g->out + k, g->outlen - k);
            g->outlen -= k;
            obj_set_ready(o, g->outlen != 0);
            pthread_mutex_unlock(&o->lock);
            return (int64_t)k;
        }
        pthread_mutex_unlock(&o->lock);
        int r = obj_wait(o->rd, POLLIN, nonblock);
        if (r) return r;
    }
}

static int64_t gp_write(FdObj *o, GuestCtx *c, const void *buf, size_t n, bool nonblock) {
    (void)nonblock;
    GPipe *g = o->data;
    pthread_mutex_lock(&o->lock);
    size_t k = n < sizeof g->in - g->inlen ? n : sizeof g->in - g->inlen;
    memcpy(g->in + g->inlen, buf, k);
    g->inlen += k;
    if (!g->connected) {
        uint8_t *nul = memchr(g->in, 0, g->inlen);
        if (nul) {
            const char *name = (const char *)g->in;
            if (strncmp(name, "pipe:", 5)) { pthread_mutex_unlock(&o->lock); return -EINVAL; }
            name += 5;
            if (strcmp(name, "qemud:sensors") && strcmp(name, "qemud:gps")) {   // others: connect fails (gps: no fixes ever arrive)
                LOGV("onirun: goldfish pipe: no service %s\n", name);
                g->inlen = 0;
                pthread_mutex_unlock(&o->lock);
                return -ENXIO;
            }
            snprintf(g->svc, sizeof g->svc, "%s", name);
            g->connected = true;
            LOGV("onirun: goldfish pipe: pid %d connected to %s\n", c->proc->pid, name);
            size_t used = (size_t)(nul + 1 - g->in);
            memmove(g->in, g->in + used, g->inlen - used);
            g->inlen -= used;
        }
    }
    while (g->connected && g->inlen >= 4) {
        char hex[5] = { (char)g->in[0], (char)g->in[1], (char)g->in[2], (char)g->in[3], 0 };
        size_t len = (size_t)strtoul(hex, NULL, 16);
        if (g->inlen < 4 + len) break;
        char msg[4096];
        size_t ml = len < sizeof msg - 1 ? len : sizeof msg - 1;
        memcpy(msg, g->in + 4, ml);
        msg[ml] = 0;
        gp_message(o, g, msg);
        memmove(g->in, g->in + 4 + len, g->inlen - 4 - len);
        g->inlen -= 4 + len;
    }
    pthread_mutex_unlock(&o->lock);
    return (int64_t)k;
}

static void gp_release(FdObj *o) { GPipe *g = o->data; free(g->out); free(g); }
static const FdOps gp_ops = { "goldfish-pipe", gp_read, gp_write, NULL, NULL, gp_release };

// /dev/tun: a TUN device with nothing behind it. TUNSETIFF names the interface, packets written
// to it are dropped and nothing ever arrives (the Thread network stack opens one at boot).
static int64_t tun_read(FdObj *o, GuestCtx *c, void *buf, size_t n, bool nonblock) {
    (void)c; (void)buf; (void)n;
    int r = obj_wait(o->rd, POLLIN, nonblock);
    return r ? r : 0;
}
static int64_t tun_write(FdObj *o, GuestCtx *c, const void *buf, size_t n, bool nonblock) { (void)o; (void)c; (void)buf; (void)nonblock; return (int64_t)n; }
static int64_t tun_ioctl(FdObj *o, GuestCtx *c, uint64_t req, uint64_t arg) {
    (void)o;
    uint8_t *ifr = arg ? (uint8_t *)(uintptr_t)(c->base + UNTAG(arg)) : NULL;
    switch (req & 0xffff) {
    case 0x54ca:                                       // TUNSETIFF: struct ifreq { char name[16]; short flags; }
        if (ifr && !ifr[0]) memcpy(ifr, "tun0", 5);
        return 0;
    case 0x54d2:                                       // TUNGETIFF
        if (ifr) memcpy(ifr, "tun0", 5);
        return 0;
    default:
        return 0;                                      // TUNSETPERSIST, TUNSETOWNER, offload flags...
    }
}
static const FdOps tun_ops = { "tun", tun_read, tun_write, tun_ioctl, NULL, NULL };
int tun_open(Proc *p, const char *path, int lflags) {
    FdObj *o = obj_new(&tun_ops, NULL, true);
    if (!o) return -ENFILE;
    fd_set_path(o->rd, path);
    return fd_install_obj(p, o->rd, o, lflags & 02000000);
}

int goldfish_pipe_open(Proc *p, const char *path, int lflags) {
    FdObj *o = obj_new(&gp_ops, calloc(1, sizeof(GPipe)), true);
    if (!o) return -ENFILE;
    fd_set_path(o->rd, path);
    return fd_install_obj(p, o->rd, o, lflags & 02000000);
}
