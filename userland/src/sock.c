// Unix-domain sockets. Guest socket names live in a table private to onirun; connections are host
// socketpairs, so fd passing (SCM_RIGHTS) and readiness (poll/epoll) come from the host for free.
// Services onirun provides itself (the property service, the log sink) register names here too.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include "sys.h"

#define L_AF_UNIX 1
#define L_SOCK_STREAM 1
#define L_SOCK_DGRAM 2
#define L_SOCK_SEQPACKET 5
#define L_SOCK_NONBLOCK 0x800
#define L_SOCK_CLOEXEC 0x80000
#define L_MSG_PEEK 2
#define L_MSG_CTRUNC 8
#define L_MSG_TRUNC 0x20
#define L_MSG_DONTWAIT 0x40
#define L_MSG_WAITALL 0x100
#define L_MSG_NOSIGNAL 0x4000
#define L_MSG_CMSG_CLOEXEC 0x40000000
#define L_SOL_SOCKET 1

enum { S_NEW, S_BOUND, S_LISTEN, S_CONN };
typedef struct Pending { int fd, pid; uint32_t uid, gid; } Pending;
typedef struct Sock {
    int type, state, pid, peer_pid;
    uint32_t peer_uid, peer_gid;
    char key[112];
    int send_fd;              // bound datagram socket: the end senders write to
    bool passcred, netlink, vsock;
    Pending *q;
    int qn, qcap;
    FdObj *o;
} Sock;

typedef struct Name {
    char key[112];
    Sock *s;                  // a guest socket, or NULL for a builtin
    BuiltinAccept accept;
    void *arg;
    int dgram_fd;             // builtin datagram sink
    struct Name *next;
} Name;
static Name *names;
static pthread_mutex_t names_lock = PTHREAD_MUTEX_INITIALIZER;

static Name *name_find_locked(const char *key) {
    for (Name *n = names; n; n = n->next) if (!strcmp(n->key, key)) return n;
    return NULL;
}

static int name_add(const char *key, Sock *s, BuiltinAccept accept, void *arg, int dgram_fd) {
    pthread_mutex_lock(&names_lock);
    if (name_find_locked(key)) { pthread_mutex_unlock(&names_lock); return -EADDRINUSE; }
    Name *n = calloc(1, sizeof *n);
    snprintf(n->key, sizeof n->key, "%s", key);
    n->s = s;
    n->accept = accept;
    n->arg = arg;
    n->dgram_fd = dgram_fd;
    n->next = names;
    names = n;
    pthread_mutex_unlock(&names_lock);
    return 0;
}

static void name_remove(const char *key) {
    pthread_mutex_lock(&names_lock);
    for (Name **pp = &names; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->key, key)) { Name *n = *pp; *pp = n->next; free(n); break; }
    pthread_mutex_unlock(&names_lock);
}

int sock_builtin_stream(const char *path, BuiltinAccept accept, void *arg) { return name_add(path, NULL, accept, arg, -1); }

static const FdOps sock_ops;

// A listening stream socket handed to a process at startup, the way init passes zygote its
// socket: bound at /dev/socket/<name>, installed as a guest fd (returned), listening.
int sock_listen_for(Proc *p, const char *name) {
    int pf[2];
    if (pipe(pf)) return -errno;
    fcntl(pf[1], F_SETFD, FD_CLOEXEC);
    Sock *s = calloc(1, sizeof *s);
    s->type = L_SOCK_STREAM;
    s->pid = p->pid;
    s->send_fd = -1;
    snprintf(s->key, sizeof s->key, "/dev/socket/%s", name);
    FdObj *o = obj_new(&sock_ops, s, false);
    s->o = o;
    o->rd = dup(pf[0]);
    o->wr = pf[1];
    int e = name_add(s->key, s, NULL, NULL, -1);
    if (e) { close(pf[0]); close(pf[1]); close(o->rd); free(s); free(o); return e; }
    s->state = S_LISTEN;
    fd_set_path(pf[0], s->key);
    return fd_install_obj(p, pf[0], o, false);
}


// macOS caps a unix datagram at net.local.dgram.maxdgram (2 KiB) unless the socket buffers are
// larger; Android sends 4 KiB log records and bigger SEQPACKET messages, so every socket we make
// gets room (the receive side too, or the sender's buffer size alone doesn't help).
static void bigbuf(int fd) {
    int v = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof v);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof v);
}
static int socketpair_big(int type, int sp[2]) {
    if (socketpair(AF_UNIX, type, 0, sp)) return -1;
    bigbuf(sp[0]); bigbuf(sp[1]);
    return 0;
}

int sock_builtin_dgram(const char *path) {
    int sp[2];
    if (socketpair_big(SOCK_DGRAM, sp)) return -1;
    fcntl(sp[0], F_SETFD, FD_CLOEXEC);
    fcntl(sp[1], F_SETFD, FD_CLOEXEC);
    if (name_add(path, NULL, NULL, NULL, sp[1])) { close(sp[0]); close(sp[1]); return -1; }
    return sp[0];
}

// ---- the socket object ----
static void sock_release(FdObj *o) {
    Sock *s = o->data;
    if (s->key[0]) name_remove(s->key);
    if (s->send_fd >= 0) close(s->send_fd);
    for (int i = 0; i < s->qn; i++) close(s->q[i].fd);
    free(s->q);
    free(s);
}
static int64_t netlink_write(FdObj *o, GuestCtx *c, const void *buf, size_t n, bool nonblock);
static const FdOps sock_ops = { "socket", NULL, NULL, NULL, NULL, sock_release };
static const FdOps netlink_ops = { "socket", NULL, netlink_write, NULL, NULL, sock_release };

static const FdOps netlink_ops;
static Sock *sock_of(Proc *p, int gfd, int *host) {
    int h = fd_host(p, gfd);
    if (h < 0) return NULL;
    FdObj *o = fd_obj(h);
    if (!o || (o->ops != &sock_ops && o->ops != &netlink_ops)) return NULL;
    if (host) *host = h;
    return o->data;
}

// There is no kernel behind our netlink sockets, so every request gets an answer right away:
// dumps are empty (NLMSG_DONE), GET requests fail with ENODEV, everything else is acknowledged.
// Without this, request/response code (libnetlink users, ifaddrs, ...) would block forever.
static void netlink_answer(Sock *s, const uint8_t *msg, size_t n) {
    while (n >= 16) {
        uint32_t len, seq, pid;
        uint16_t type, flags;
        memcpy(&len, msg, 4); memcpy(&type, msg + 4, 2); memcpy(&flags, msg + 6, 2); memcpy(&seq, msg + 8, 4); memcpy(&pid, msg + 12, 4);
        if (len < 16 || len > n) break;
        uint8_t reply[36] = {0};
        uint32_t rlen;
        if (flags & 0x300) {                             // NLM_F_DUMP: an empty list
            rlen = 20;
            uint16_t t = 3;                              // NLMSG_DONE
            memcpy(reply + 4, &t, 2);
            int32_t zero = 0;
            memcpy(reply + 16, &zero, 4);
        } else {
            rlen = 36;                                   // NLMSG_ERROR + the request header
            uint16_t t = 2;
            memcpy(reply + 4, &t, 2);
            int32_t err = (type % 4 == 2 && type >= 16) ? -19 : 0;   // RTM_GET*: ENODEV; others: ACK
            memcpy(reply + 16, &err, 4);
            memcpy(reply + 20, msg, 16);
        }
        memcpy(reply, &rlen, 4);
        memcpy(reply + 8, &seq, 4);
        memcpy(reply + 12, &pid, 4);
        if (s->send_fd >= 0) write(s->send_fd, reply, rlen);
        msg += (len + 3) & ~3u;
        n -= (len + 3) & ~3u;
    }
}
static int64_t netlink_write(FdObj *o, GuestCtx *c, const void *buf, size_t n, bool nonblock) {
    (void)c; (void)nonblock;
    netlink_answer(o->data, buf, n);
    return (int64_t)n;
}

static int host_type(int ltype) { return ltype == L_SOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM; }

static int sock_create(Proc *p, int ltype, int host, int flags) {
    Sock *s = calloc(1, sizeof *s);
    s->type = ltype & 0xf;
    s->pid = p->pid;
    s->send_fd = -1;
    FdObj *o = obj_new(&sock_ops, s, false);
    s->o = o;
    fcntl(host, F_SETFD, FD_CLOEXEC);
    if (flags & L_SOCK_NONBLOCK) fcntl(host, F_SETFL, O_NONBLOCK);
    char path[32];
    snprintf(path, sizeof path, "socket:[%d]", host);
    fd_set_path(host, path);
    return fd_install_obj(p, host, o, flags & L_SOCK_CLOEXEC);
}

// Replaces what the guest's host fd refers to, keeping the fd number (and its O_NONBLOCK).
static void swap_host(int host, int newfd) {
    int fl = fcntl(host, F_GETFL) & O_NONBLOCK;
    dup2(newfd, host);
    close(newfd);
    fcntl(host, F_SETFL, fl);
}

// Linux sockaddr_un -> registry key. Abstract names (leading NUL) become "@name".
static int addr_key(uint64_t addr, uint64_t len, char *key, size_t n) {
    if (len < 2 || len > 110) return -EINVAL;
    const uint8_t *a = gptr(addr);
    if (!a) return -EFAULT;
    uint16_t fam;
    memcpy(&fam, a, 2);
    if (fam != L_AF_UNIX) return -EAFNOSUPPORT;
    size_t pl = len - 2;
    if (pl == 0) { key[0] = 0; return 0; }
    if (a[2] == 0) {
        snprintf(key, n, "@%.*s", (int)(pl - 1), a + 3);
    } else {
        snprintf(key, n, "%.*s", (int)pl, a + 2);
        key[strcspn(key, "")] = 0;
        // Resolve relative to the process's cwd without touching the filesystem.
        if (key[0] != '/') {
            char tmp[112];
            snprintf(tmp, sizeof tmp, "%s/%s", tr_cur->proc->cwd, key);
            snprintf(key, n, "%s", tmp);
        }
    }
    return 0;
}

static void put_addr(uint64_t addr, uint64_t lenp, const char *key) {
    if (!addr || !lenp) return;
    uint32_t have = *(uint32_t *)gptr(lenp);
    uint8_t out[112] = {0};
    out[0] = L_AF_UNIX;
    size_t n = 2;
    if (key && key[0] == '@') { memcpy(out + 3, key + 1, strlen(key) - 1); n = 2 + strlen(key); }
    else if (key && key[0]) { memcpy(out + 2, key, strlen(key)); n = 3 + strlen(key); }
    memcpy(gptr(addr), out, n < have ? n : have);
    *(uint32_t *)gptr(lenp) = (uint32_t)n;
}

static int host_msg_flags(uint64_t lf) {
    int f = 0;
    if (lf & L_MSG_PEEK) f |= MSG_PEEK;
    if (lf & L_MSG_DONTWAIT) f |= MSG_DONTWAIT;
    if (lf & L_MSG_WAITALL) f |= MSG_WAITALL;
    if (lf & L_MSG_TRUNC) f |= MSG_TRUNC;
    return f;
}

static int64_t send_result(GuestCtx *c, ssize_t n, uint64_t lflags) {
    if (n >= 0) return n;
    if (errno == EPIPE && !(lflags & L_MSG_NOSIGNAL)) proc_raise(c, 13);
    return -errno;
}

// ---- calls ----
static int64_t do_socket(GuestCtx *c, int domain, int type, int protocol) {
    (void)protocol;
    if (domain == 16) {                                   // AF_NETLINK: a socket nothing ever arrives on
        int sp[2];
        if (socketpair_big(SOCK_DGRAM, sp)) return -errno;
        fcntl(sp[1], F_SETFD, FD_CLOEXEC);
        int g = sock_create(c->proc, L_SOCK_DGRAM | (type & ~0xf), sp[0], type);
        if (g < 0) { close(sp[1]); return g; }
        Sock *s = sock_of(c->proc, g, NULL);
        s->state = S_CONN;
        s->send_fd = sp[1];
        s->netlink = true;
        s->o->ops = &netlink_ops;
        return g;
    }
    if (domain == 40) {                                   // AF_VSOCK: exists (libqemupipe probes it), connects to nothing
        int sp[2];
        if (socketpair_big(SOCK_STREAM, sp)) return -errno;
        fcntl(sp[1], F_SETFD, FD_CLOEXEC);
        int g = sock_create(c->proc, L_SOCK_STREAM | (type & ~0xf), sp[0], type);
        if (g < 0) { close(sp[1]); return g; }
        Sock *s = sock_of(c->proc, g, NULL);
        s->send_fd = sp[1];
        s->vsock = true;
        return g;
    }
    if (domain != L_AF_UNIX) return -EAFNOSUPPORT;
    int t = type & 0xf;
    if (t != L_SOCK_STREAM && t != L_SOCK_DGRAM && t != L_SOCK_SEQPACKET) return -EPROTONOSUPPORT;
    int h = socket(AF_UNIX, host_type(t), 0);
    if (h >= 0) bigbuf(h);
    if (h < 0) return -errno;
    return sock_create(c->proc, type, h, type);
}

static int64_t do_socketpair(GuestCtx *c, int domain, int type, uint64_t sv) {
    if (domain != L_AF_UNIX) return -EAFNOSUPPORT;
    int sp[2];
    if (socketpair_big(host_type(type & 0xf), sp)) return -errno;
    int g0 = sock_create(c->proc, type, sp[0], type);
    if (g0 < 0) { close(sp[1]); return g0; }
    int g1 = sock_create(c->proc, type, sp[1], type);
    if (g1 < 0) { fd_close(c->proc, g0); return g1; }
    Sock *a = sock_of(c->proc, g0, NULL), *b = sock_of(c->proc, g1, NULL);
    a->state = b->state = S_CONN;
    a->peer_pid = b->peer_pid = c->proc->pid;
    a->peer_uid = b->peer_uid = c->proc->euid;
    a->peer_gid = b->peer_gid = c->proc->egid;
    int32_t *out = gptr(sv);
    out[0] = g0;
    out[1] = g1;
    return 0;
}

static int64_t do_bind(GuestCtx *c, int gfd, uint64_t addr, uint64_t len) {
    int h;
    Sock *s = sock_of(c->proc, gfd, &h);
    if (!s) return -ENOTSOCK;
    if (s->vsock) return -EADDRNOTAVAIL;
    if (s->netlink) return 0;
    if (s->state != S_NEW) return -EINVAL;
    char key[112];
    int e = addr_key(addr, len, key, sizeof key);
    if (e) return e;
    if (!key[0]) return -EINVAL;
    if (s->type == L_SOCK_DGRAM) {
        int sp[2];
        if (socketpair_big(SOCK_DGRAM, sp)) return -errno;
        fcntl(sp[1], F_SETFD, FD_CLOEXEC);
        if ((e = name_add(key, s, NULL, NULL, -1))) { close(sp[0]); close(sp[1]); return e; }
        swap_host(h, sp[0]);
        s->send_fd = sp[1];
    } else if ((e = name_add(key, s, NULL, NULL, -1))) {
        return e;
    }
    snprintf(s->key, sizeof s->key, "%s", key);
    s->state = S_BOUND;
    return 0;
}

static int64_t do_listen(GuestCtx *c, int gfd) {
    int h;
    Sock *s = sock_of(c->proc, gfd, &h);
    if (!s) return -ENOTSOCK;
    if (s->type == L_SOCK_DGRAM) return -EOPNOTSUPP;
    if (s->state == S_LISTEN) return 0;
    if (s->state != S_BOUND) return -EINVAL;
    int p[2];
    if (pipe(p)) return -errno;
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    FdObj *o = s->o;
    pthread_mutex_lock(&o->lock);
    o->rd = dup(p[0]);
    o->wr = p[1];
    swap_host(h, p[0]);
    s->state = S_LISTEN;
    pthread_mutex_unlock(&o->lock);
    return 0;
}

// Hands one end of a new connection to whoever owns the name.
static int64_t deliver(Name *n, int fd, int pid) {
    if (n->accept) { n->accept(fd, pid, n->arg); return 0; }
    Sock *l = n->s;
    FdObj *o = l->o;
    pthread_mutex_lock(&o->lock);
    if (l->state != S_LISTEN) { pthread_mutex_unlock(&o->lock); close(fd); return -ECONNREFUSED; }
    if (l->qn == l->qcap) { l->qcap = l->qcap ? l->qcap * 2 : 8; l->q = realloc(l->q, (size_t)l->qcap * sizeof(Pending)); }
    Proc *cp;
    LOCKED(cp = proc_find(pid));
    l->q[l->qn++] = (Pending){ fd, pid, cp ? cp->euid : 0, cp ? cp->egid : 0 };
    obj_set_ready(o, true);
    pthread_mutex_unlock(&o->lock);
    return 0;
}

static int64_t do_connect(GuestCtx *c, int gfd, uint64_t addr, uint64_t len) {
    int h;
    Sock *s = sock_of(c->proc, gfd, &h);
    if (!s) return -ENOTSOCK;
    if (s->vsock) {
        // The emulator's host serves qemud pipes on vsock port 5000 (libqemupipe's VsockPort::Data):
        // the connected socket becomes a goldfish pipe. Nothing listens on any other port.
        const uint8_t *sa = gptr(addr);
        uint32_t port = len >= 12 && sa ? *(const uint32_t *)(sa + 4) : 0;
        if (port != 5000) return -ECONNREFUSED;
        bool cloexec = fd_cloexec(c->proc, gfd, -1) > 0;
        int g = goldfish_pipe_open(c->proc, "/dev/goldfish_pipe", cloexec ? 02000000 : 0);
        if (g < 0) return g;
        int r = fd_dup(c->proc, g, gfd, true, cloexec);
        fd_close(c->proc, g);
        return r < 0 ? r : 0;
    }
    char key[112];
    int e = addr_key(addr, len, key, sizeof key);
    if (e) return e;
    pthread_mutex_lock(&names_lock);
    Name *n = name_find_locked(key);
    if (!n || (n->s && n->s->type != s->type && !(n->s->type == L_SOCK_STREAM && s->type == L_SOCK_SEQPACKET))) {
        pthread_mutex_unlock(&names_lock);
        return -ECONNREFUSED;
    }
    if (s->type == L_SOCK_DGRAM) {
        int target = n->s ? n->s->send_fd : n->dgram_fd;
        if (target < 0) { pthread_mutex_unlock(&names_lock); return -ECONNREFUSED; }
        int d = dup(target);
        pthread_mutex_unlock(&names_lock);
        swap_host(h, d);
        s->state = S_CONN;
        s->peer_pid = n->s ? n->s->pid : 0;
        return 0;
    }
    if (s->state == S_CONN) { pthread_mutex_unlock(&names_lock); return -EISCONN; }
    int sp[2];
    if (socketpair_big(host_type(s->type), sp)) { pthread_mutex_unlock(&names_lock); return -errno; }
    fcntl(sp[1], F_SETFD, FD_CLOEXEC);
    s->peer_pid = n->s ? n->s->pid : 0;
    if (n->s) { Proc *lp; LOCKED(lp = proc_find(n->s->pid)); if (lp) { s->peer_uid = lp->euid; s->peer_gid = lp->egid; } }
    int64_t r = deliver(n, sp[1], c->proc->pid);
    pthread_mutex_unlock(&names_lock);
    if (r) { close(sp[0]); return r; }
    swap_host(h, sp[0]);
    s->state = S_CONN;
    return 0;
}

static int64_t do_accept(GuestCtx *c, int gfd, uint64_t addr, uint64_t lenp, int flags) {
    int h;
    Sock *s = sock_of(c->proc, gfd, &h);
    if (!s) return -ENOTSOCK;
    if (s->state != S_LISTEN) return -EINVAL;
    FdObj *o = s->o;
    bool nonblock = fcntl(h, F_GETFL) & O_NONBLOCK;
    for (;;) {
        pthread_mutex_lock(&o->lock);
        if (s->qn) {
            Pending pe = s->q[0];
            memmove(s->q, s->q + 1, (size_t)--s->qn * sizeof(Pending));
            obj_set_ready(o, s->qn > 0);
            pthread_mutex_unlock(&o->lock);
            int g = sock_create(c->proc, s->type | (flags & L_SOCK_NONBLOCK), pe.fd, flags);
            if (g < 0) return g;
            Sock *ns = sock_of(c->proc, g, NULL);
            ns->state = S_CONN;
            ns->peer_pid = pe.pid;
            ns->peer_uid = pe.uid;
            ns->peer_gid = pe.gid;
            put_addr(addr, lenp, NULL);
            return g;
        }
        pthread_mutex_unlock(&o->lock);
        int r = obj_wait(h, POLLIN, nonblock);
        if (r) return r;
    }
}

static int64_t do_sendto(GuestCtx *c, int gfd, uint64_t buf, uint64_t len, uint64_t lflags, uint64_t addr, uint64_t alen) {
    int h;
    Sock *s = sock_of(c->proc, gfd, &h);
    if (!s) return -ENOTSOCK;
    if (addr && s->type == L_SOCK_DGRAM && s->state != S_CONN) {
        char key[112];
        int e = addr_key(addr, alen, key, sizeof key);
        if (e) return e;
        pthread_mutex_lock(&names_lock);
        Name *n = name_find_locked(key);
        int target = n ? (n->s ? n->s->send_fd : n->dgram_fd) : -1;
        int d = target >= 0 ? dup(target) : -1;
        pthread_mutex_unlock(&names_lock);
        if (d < 0) return -ECONNREFUSED;
        ssize_t r = send(d, gptr(buf), len, host_msg_flags(lflags));
        close(d);
        return send_result(c, r, lflags);
    }
    if (s->netlink) { netlink_answer(s, gptr(buf), len); return (int64_t)len; }
    return send_result(c, send(h, gptr(buf), len, host_msg_flags(lflags)), lflags);
}

static int64_t do_recvfrom(GuestCtx *c, int gfd, uint64_t buf, uint64_t len, uint64_t lflags, uint64_t addr, uint64_t alenp) {
    int h;
    Sock *s = sock_of(c->proc, gfd, &h);
    if (!s) return -ENOTSOCK;
    ssize_t r = recv(h, gptr(buf), len, host_msg_flags(lflags));
    if (r < 0) return -errno;
    put_addr(addr, alenp, NULL);
    return r;
}

// Linux arm64 struct msghdr: name 0, namelen 8, iov 16, iovlen 24, control 32, controllen 40, flags 48.
typedef struct { uint64_t name; uint32_t namelen; uint64_t iov, iovlen, control, controllen; int32_t flags; } LxMsg;
static void get_msg(uint64_t a, LxMsg *m) {
    const uint8_t *p = gptr(a);
    memcpy(&m->name, p, 8); memcpy(&m->namelen, p + 8, 4); memcpy(&m->iov, p + 16, 8);
    memcpy(&m->iovlen, p + 24, 8); memcpy(&m->control, p + 32, 8); memcpy(&m->controllen, p + 40, 8);
    memcpy(&m->flags, p + 48, 4);
}

static int64_t do_sendmsg(GuestCtx *c, int gfd, uint64_t msg, uint64_t lflags) {
    int h;
    Sock *s = sock_of(c->proc, gfd, &h);
    if (!s) return -ENOTSOCK;
    LxMsg m;
    get_msg(msg, &m);
    if (m.iovlen > 1024) return -EMSGSIZE;
    struct iovec iov[1024];
    const uint64_t *gi = gptr(m.iov);
    for (uint64_t i = 0; i < m.iovlen; i++) { iov[i].iov_base = gptr(gi[2 * i]); iov[i].iov_len = gi[2 * i + 1]; }
    // Control: pass SCM_RIGHTS through with host fds; drop SCM_CREDENTIALS (the receiver gets ours).
    uint8_t ctl[1024];
    size_t cl = 0;
    const uint8_t *gc = gptr(m.control);
    for (uint64_t off = 0; gc && off + 16 <= m.controllen;) {
        uint64_t clen;
        int32_t level, type;
        memcpy(&clen, gc + off, 8); memcpy(&level, gc + off + 8, 4); memcpy(&type, gc + off + 12, 4);
        if (clen < 16 || off + clen > m.controllen) return -EINVAL;
        if (level == L_SOL_SOCKET && type == SCM_RIGHTS) {
            int nfd = (int)((clen - 16) / 4);
            struct cmsghdr *cm = (struct cmsghdr *)(ctl + cl);
            if (cl + CMSG_SPACE(nfd * sizeof(int)) > sizeof ctl) return -EINVAL;
            cm->cmsg_len = (socklen_t)CMSG_LEN(nfd * sizeof(int));
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type = SCM_RIGHTS;
            int *fds = (int *)CMSG_DATA(cm);
            for (int i = 0; i < nfd; i++) {
                int32_t g;
                memcpy(&g, gc + off + 16 + 4 * i, 4);
                if ((fds[i] = fd_host(c->proc, g)) < 0) return -EBADF;
            }
            cl += CMSG_SPACE(nfd * sizeof(int));
        }
        off += (clen + 7) & ~7ull;
    }
    struct msghdr hm = { .msg_iov = iov, .msg_iovlen = (int)m.iovlen, .msg_control = cl ? ctl : NULL, .msg_controllen = (socklen_t)cl };
    if (m.name && s->type == L_SOCK_DGRAM && s->state != S_CONN) {
        char key[112];
        int e = addr_key(m.name, m.namelen, key, sizeof key);
        if (e) return e;
        pthread_mutex_lock(&names_lock);
        Name *n = name_find_locked(key);
        int target = n ? (n->s ? n->s->send_fd : n->dgram_fd) : -1;
        int d = target >= 0 ? dup(target) : -1;
        pthread_mutex_unlock(&names_lock);
        if (d < 0) return -ECONNREFUSED;
        ssize_t r = sendmsg(d, &hm, host_msg_flags(lflags));
        close(d);
        return send_result(c, r, lflags);
    }
    if (s->netlink) {
        size_t total = 0;
        for (uint64_t i = 0; i < m.iovlen; i++) total += iov[i].iov_len;
        uint8_t *flat = malloc(total + 1);
        size_t off = 0;
        for (uint64_t i = 0; i < m.iovlen; i++) { memcpy(flat + off, iov[i].iov_base, iov[i].iov_len); off += iov[i].iov_len; }
        netlink_answer(s, flat, total);
        free(flat);
        return (int64_t)total;
    }
    return send_result(c, sendmsg(h, &hm, host_msg_flags(lflags)), lflags);
}

static int64_t do_recvmsg(GuestCtx *c, int gfd, uint64_t msg, uint64_t lflags) {
    int h;
    Sock *s = sock_of(c->proc, gfd, &h);
    if (!s) return -ENOTSOCK;
    LxMsg m;
    get_msg(msg, &m);
    if (m.iovlen > 1024) return -EMSGSIZE;
    struct iovec iov[1024];
    const uint64_t *gi = gptr(m.iov);
    for (uint64_t i = 0; i < m.iovlen; i++) { iov[i].iov_base = gptr(gi[2 * i]); iov[i].iov_len = gi[2 * i + 1]; }
    uint8_t ctl[1024];
    struct msghdr hm = { .msg_iov = iov, .msg_iovlen = (int)m.iovlen, .msg_control = m.control ? ctl : NULL,
                         .msg_controllen = m.control ? sizeof ctl : 0 };
    ssize_t r = recvmsg(h, &hm, host_msg_flags(lflags));
    if (r < 0) return -errno;
    int32_t oflags = 0;
    if (hm.msg_flags & MSG_TRUNC) oflags |= L_MSG_TRUNC;
    uint64_t used = 0;
    uint8_t *gc = gptr(m.control);
    for (struct cmsghdr *cm = hm.msg_controllen ? CMSG_FIRSTHDR(&hm) : NULL; cm; cm = CMSG_NXTHDR(&hm, cm)) {
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
        int nfd = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        int *fds = (int *)CMSG_DATA(cm);
        uint64_t need = 16 + 4 * (uint64_t)nfd, space = (need + 7) & ~7ull;
        if (!gc || used + need > m.controllen) {
            for (int i = 0; i < nfd; i++) close(fds[i]);
            oflags |= L_MSG_CTRUNC;
            continue;
        }
        uint64_t clen = need;
        int32_t level = L_SOL_SOCKET, type = SCM_RIGHTS;
        memcpy(gc + used, &clen, 8); memcpy(gc + used + 8, &level, 4); memcpy(gc + used + 12, &type, 4);
        for (int i = 0; i < nfd; i++) {
            FdObj *po = obj_by_fd(fds[i]);                 // a sync_file, eventfd, ... keeps its identity
            if (po) fd_attach_obj(fds[i], po);
            int32_t g = fd_install(c->proc, fds[i], 0, lflags & L_MSG_CMSG_CLOEXEC);
            memcpy(gc + used + 16 + 4 * i, &g, 4);
        }
        used += space;
    }
    uint8_t *gm = gptr(msg);
    memcpy(gm + 40, &used, 8);
    memcpy(gm + 48, &oflags, 4);
    if (m.name) put_addr(m.name, 0, NULL), memcpy(gm + 8, &(uint32_t){0}, 4);
    return r;
}

static int64_t do_getsockopt(GuestCtx *c, int gfd, int level, int opt, uint64_t val, uint64_t lenp) {
    int h;
    Sock *s = sock_of(c->proc, gfd, &h);
    if (!s) return -ENOTSOCK;
    if (level != L_SOL_SOCKET) return -ENOPROTOOPT;
    uint32_t *len = gptr(lenp);
    uint8_t out[16] = {0};
    uint32_t n = 4;
    switch (opt) {
    case 3: *(int32_t *)out = s->type; break;                                 // SO_TYPE
    case 4: break;                                                            // SO_ERROR
    case 7: case 8: *(int32_t *)out = 212992; break;                          // SO_SNDBUF/RCVBUF
    case 17: {                                                                // SO_PEERCRED
        uint32_t cred[3] = { (uint32_t)s->peer_pid, s->peer_uid, s->peer_gid };
        memcpy(out, cred, 12);
        n = 12;
        break;
    }
    case 38: break;                                                           // SO_PROTOCOL
    case 39: *(int32_t *)out = L_AF_UNIX; break;                              // SO_DOMAIN
    case 16: *(int32_t *)out = s->passcred; break;                            // SO_PASSCRED
    default: return -ENOPROTOOPT;
    }
    if (*len < n) n = *len;
    memcpy(gptr(val), out, n);
    *len = n;
    return 0;
}

static int64_t do_setsockopt(GuestCtx *c, int gfd, int level, int opt, uint64_t val, uint64_t len) {
    int h;
    Sock *s = sock_of(c->proc, gfd, &h);
    if (!s) return -ENOTSOCK;
    if (level != L_SOL_SOCKET) return 0;
    switch (opt) {
    case 16: s->passcred = *(int32_t *)gptr(val) != 0; return 0;              // SO_PASSCRED
    case 20: case 21: {                                                       // SO_RCVTIMEO / SO_SNDTIMEO
        if (len < 16) return -EINVAL;
        const int64_t *tv = gptr(val);
        struct timeval htv = { (time_t)tv[0], (suseconds_t)tv[1] };
        return setsockopt(h, SOL_SOCKET, opt == 20 ? SO_RCVTIMEO : SO_SNDTIMEO, &htv, sizeof htv) ? -errno : 0;
    }
    default: return 0;
    }
}

static int64_t do_name(GuestCtx *c, int gfd, uint64_t addr, uint64_t lenp, bool peer) {
    Sock *s = sock_of(c->proc, gfd, NULL);
    if (!s) return -ENOTSOCK;
    if (s->netlink) {                                     // sockaddr_nl: family, pad, pid, groups
        uint32_t nl[3] = { 16, (uint32_t)c->proc->pid, 0 };
        uint32_t have = *(uint32_t *)gptr(lenp);
        memcpy(gptr(addr), nl, have < 12 ? have : 12);
        *(uint32_t *)gptr(lenp) = 12;
        return 0;
    }
    put_addr(addr, lenp, peer ? NULL : s->key);
    return 0;
}

int64_t sock_call(GuestCtx *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    switch (nr) {
    case 198: return do_socket(c, (int)a0, (int)a1, (int)a2);
    case 199: return do_socketpair(c, (int)a0, (int)a1, a3);
    case 200: return do_bind(c, (int)a0, a1, a2);
    case 201: return do_listen(c, (int)a0);
    case 202: return do_accept(c, (int)a0, a1, a2, 0);
    case 242: return do_accept(c, (int)a0, a1, a2, (int)a3);
    case 203: return do_connect(c, (int)a0, a1, a2);
    case 204: return do_name(c, (int)a0, a1, a2, false);
    case 205: return do_name(c, (int)a0, a1, a2, true);
    case 206: return do_sendto(c, (int)a0, a1, a2, a3, a4, a5);
    case 207: return do_recvfrom(c, (int)a0, a1, a2, a3, a4, a5);
    case 208: return do_setsockopt(c, (int)a0, (int)a1, (int)a2, a3, a4);
    case 209: return do_getsockopt(c, (int)a0, (int)a1, (int)a2, a3, a4);
    case 210: {                                                               // shutdown
        int h;
        Sock *s = sock_of(c->proc, (int)a0, &h);
        if (!s) return -ENOTSOCK;
        if (s->state == S_CONN) shutdown(h, (int)a1);
        return 0;
    }
    case 211: return do_sendmsg(c, (int)a0, a1, a2);
    case 212: return do_recvmsg(c, (int)a0, a1, a2);
    default: return -ENOSYS;
    }
}

// ---- the log sink: liblog's datagrams to /dev/socket/logdw ----
static void *log_sink(void *arg) {
    int fd = (int)(intptr_t)arg;
    int min_prio = getenv("ONIRUN_LOGCAT") ? 2 : 6;                           // all, or errors and worse
    static const char prio_ch[] = "??VDIWEFS";
    uint8_t buf[8192];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return NULL;
        if (n < 12) continue;
        uint8_t id = buf[0];
        uint16_t tid;
        memcpy(&tid, buf + 1, 2);
        if (id == 2 || id == 5 || id == 6) continue;                          // binary event logs
        int prio = buf[11];
        const char *tag = (const char *)buf + 12;
        size_t tl = strnlen(tag, (size_t)n - 12);
        const char *msg = tag + tl + 1;
        size_t ml = msg < (const char *)buf + n ? strnlen(msg, (size_t)(buf + n - (const uint8_t *)msg)) : 0;
        if (prio < min_prio) continue;
        fprintf(stderr, "%c/%.*s(%u): %.*s\n", prio < 9 ? prio_ch[prio] : '?', (int)tl, tag, tid, (int)ml, msg);
    }
}

void sock_init(void) {
    int fd = sock_builtin_dgram("/dev/socket/logdw");
    if (fd >= 0) {
        pthread_t t;
        pthread_create(&t, NULL, log_sink, (void *)(intptr_t)fd);
        pthread_detach(t);
    }
}
