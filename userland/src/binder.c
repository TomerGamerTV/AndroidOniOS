// Binder: Android's IPC, done the way the kernel driver does it, but inside onirun. Each guest
// process that opens /dev/binder gets a BProc; its threads get BThreads; local objects are BNodes
// and remote references are BRefs with per-process handles. Transactions copy their data straight
// into the receiver's mapped buffer (the receiver maps the object's file read-only, we keep a
// writable alias), translating objects, handles and fds on the way, like the driver.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sys.h"

// ---- protocol (uapi/linux/android/binder.h) ----
#define PACK(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define T_BINDER PACK('s', 'b', '*', 0x85)
#define T_WEAK_BINDER PACK('w', 'b', '*', 0x85)
#define T_HANDLE PACK('s', 'h', '*', 0x85)
#define T_WEAK_HANDLE PACK('w', 'h', '*', 0x85)
#define T_FD PACK('f', 'd', '*', 0x85)
#define T_FDA PACK('f', 'd', 'a', 0x85)
#define T_PTR PACK('p', 't', '*', 0x85)
#define FLAG_ACCEPTS_FDS 0x100
#define FLAG_TXN_SECURITY_CTX 0x1000
#define BUFFER_HAS_PARENT 1
#define TF_ONE_WAY 1
#define TF_ACCEPT_FDS 0x10
#define TF_CLEAR_BUF 0x20

#define IOC_NR(c) ((c) & 0xff)
#define IOC_TYPE(c) (((c) >> 8) & 0xff)
#define IOC_SIZE(c) (((c) >> 16) & 0x3fff)
#define BR(nr, size) (((size) ? 0x80000000u | ((uint32_t)(size) << 16) : 0u) | ('r' << 8) | (nr))   // _IOR / _IO
#define BR_ERROR BR(0, 4)
#define BR_TRANSACTION BR(2, 64)
#define BR_TRANSACTION_SEC_CTX BR(2, 72)
#define BR_REPLY BR(3, 64)
#define BR_DEAD_REPLY BR(5, 0)
#define BR_TRANSACTION_COMPLETE BR(6, 0)
#define BR_INCREFS BR(7, 16)
#define BR_ACQUIRE BR(8, 16)
#define BR_RELEASE BR(9, 16)
#define BR_DECREFS BR(10, 16)
#define BR_NOOP BR(12, 0)
#define BR_SPAWN_LOOPER BR(13, 0)
#define BR_DEAD_BINDER BR(15, 8)
#define BR_CLEAR_DEATH_NOTIFICATION_DONE BR(16, 8)
#define BR_FAILED_REPLY BR(17, 0)
enum {   // BC_* by ioctl number ('c')
    BC_TRANSACTION, BC_REPLY, BC_ACQUIRE_RESULT, BC_FREE_BUFFER, BC_INCREFS, BC_ACQUIRE, BC_RELEASE, BC_DECREFS,
    BC_INCREFS_DONE, BC_ACQUIRE_DONE, BC_ATTEMPT_ACQUIRE, BC_REGISTER_LOOPER, BC_ENTER_LOOPER, BC_EXIT_LOOPER,
    BC_REQUEST_DEATH_NOTIFICATION, BC_CLEAR_DEATH_NOTIFICATION, BC_DEAD_BINDER_DONE, BC_TRANSACTION_SG, BC_REPLY_SG,
};
enum { LOOPER_REGISTERED = 1, LOOPER_ENTERED = 2, LOOPER_EXITED = 4, LOOPER_INVALID = 8, LOOPER_WAITING = 16 };

typedef struct { uint64_t target, cookie; uint32_t code, flags; int32_t sender_pid; uint32_t sender_euid;
                 uint64_t data_size, offsets_size, buffer, offsets; } TData;   // binder_transaction_data, 64 bytes
_Static_assert(sizeof(TData) == 64, "binder_transaction_data");

// ---- objects ----
typedef struct BProc BProc;
typedef struct BThread BThread;
typedef struct BNode BNode;
typedef struct BRef BRef;
typedef struct BTrans BTrans;
typedef struct BWork BWork;
typedef struct BBuf BBuf;

typedef struct BCtx { const char *name; BNode *mgr; struct BCtx *next; } BCtx;

struct BNode {
    BProc *proc;              // owner, NULL once dead
    uint64_t ptr, cookie;
    uint32_t flags;
    int internal_strong, local_strong, local_weak, nrefs;
    bool has_strong, has_weak, pending_strong, pending_weak, queued, dead;
    BRef *refs;               // all references to this node
    struct BNode *next;       // in proc->nodes
};

struct BRef {
    BProc *proc;              // holder
    BNode *node;
    uint32_t desc;
    int strong, weak;
    bool death;               // notification requested
    uint64_t death_cookie;
    bool dead_sent;
    BRef *next_in_proc, *next_in_node;
};

enum { W_TRANSACTION, W_REPLY, W_COMPLETE, W_NODE, W_DEAD_BINDER, W_CLEAR_DEATH_DONE, W_ERROR };
struct BWork {
    int kind;
    BTrans *t;
    BNode *node;
    uint64_t cookie;
    uint32_t error;
    BWork *next;
};
typedef struct { BWork *head, *tail; } WorkList;

struct BBuf {                 // a slice of the receiver's mapped area
    uint64_t off, size;
    bool free;
    BTrans *t;
    // objects translated into this buffer, undone when it is freed
    struct { uint32_t type; BNode *node; BRef *ref; int fd; } *objs;
    int nobjs;
    BBuf *next;
};

struct BTrans {
    BThread *from;            // sender thread (NULL for oneway or if it died)
    BProc *from_proc, *to_proc;
    BThread *to_thread;
    BTrans *from_parent, *to_parent;   // transaction stacks
    BNode *target;
    uint32_t code, flags;
    int sender_pid;
    BBuf *buf;
    uint64_t data_size, offsets_size, secctx_off;
    bool need_reply, is_reply, has_secctx;
};

struct BThread {
    BProc *proc;
    uint64_t tid;
    int looper;
    BTrans *stack;
    WorkList todo;
    int wake_rd, wake_wr;
    bool waiting_proc;
    BThread *next;
};

struct BProc {
    Proc *proc;
    int pid;
    BCtx *ctx;
    FdObj *obj;
    BThread *threads;
    BNode *nodes;
    BRef *refs;
    uint32_t next_desc;
    uint8_t *buf_host;        // writable alias of the receiver's mapped area
    uint64_t buf_gaddr, buf_size;
    BBuf *bufs;
    WorkList todo;
    int max_threads, requested_threads, started_threads, ready_threads;
    bool dead;
    char secctx[64];
    BProc *next_all;
};
static BProc *all_bprocs;

static pthread_mutex_t blk = PTHREAD_MUTEX_INITIALIZER;   // one lock for the whole driver
static uint64_t blk_owner;                                  // host thread id holding blk (diagnostics)
static const char *blk_where;
#define BLK_LOCK(w) do { pthread_mutex_lock(&blk); pthread_threadid_np(NULL, &blk_owner); blk_where = (w); } while (0)
static BCtx *ctxs;
static __thread BThread *cur_th;      // the thread inside the ioctl, for work that belongs to the caller

// ---- work lists ----
static void work_push(WorkList *l, BWork *w) {
    w->next = NULL;
    if (l->tail) l->tail->next = w; else l->head = w;
    l->tail = w;
}
static void work_push_front(WorkList *l, BWork *w) {
    w->next = l->head;
    l->head = w;
    if (!l->tail) l->tail = w;
}
static BWork *work_pop(WorkList *l) {
    BWork *w = l->head;
    if (w) { l->head = w->next; if (!l->head) l->tail = NULL; }
    return w;
}
static BWork *work_new(int kind) { BWork *w = calloc(1, sizeof *w); w->kind = kind; return w; }

static bool proc_has_work(BProc *p) {
    if (p->todo.head) return true;
    for (BThread *t = p->threads; t; t = t->next) if (t->todo.head) return true;
    return false;
}

static void thread_wake(BThread *t) {
    // Non-blocking: a full pipe already means a wake is pending, and blocking here would hold blk.
    if (t->wake_wr >= 0) { char b = 0x7d; (void)!write(t->wake_wr, &b, 1); }
}

// Something changed in p's queues: wake a thread that waits for process work and update the fd.
static void proc_poke(BProc *p) {
    for (BThread *t = p->threads; t; t = t->next)
        if (t->waiting_proc) { thread_wake(t); break; }
    pthread_mutex_lock(&p->obj->lock);
    obj_set_ready(p->obj, proc_has_work(p));
    pthread_mutex_unlock(&p->obj->lock);
}

static void queue_proc(BProc *p, BWork *w) { work_push(&p->todo, w); proc_poke(p); }
static void queue_thread(BThread *t, BWork *w) { work_push(&t->todo, w); thread_wake(t); proc_poke(t->proc); }

// ---- nodes and refs (the driver's refcount dance) ----
static void node_check(BNode *n) {   // queues node work to its owner if its userspace refs need updating
    if (!n->proc || n->queued) return;
    bool strong = n->internal_strong || n->local_strong;
    bool weak = n->nrefs || n->local_weak || strong;
    if ((weak && !n->has_weak) || (strong && !n->has_strong) || (!strong && n->has_strong) || (!weak && n->has_weak)) {
        BWork *w = work_new(W_NODE);
        w->node = n;
        n->queued = true;
        // Like the driver: refcount work for the caller's own objects goes to the calling thread,
        // so a thread waiting for a reply still sees the BR_ACQUIRE for what it just sent.
        if (cur_th && cur_th->proc == n->proc) queue_thread(cur_th, w);
        else queue_proc(n->proc, w);
    }
}

static void node_free_if_unused(BNode *n) {
    if (n->refs || n->internal_strong || n->local_strong || n->local_weak || n->queued) return;
    if (n->proc) {
        for (BNode **pp = &n->proc->nodes; *pp; pp = &(*pp)->next) if (*pp == n) { *pp = n->next; break; }
        if (n->proc->ctx->mgr == n) n->proc->ctx->mgr = NULL;
    }
    free(n);
}

static void node_inc(BNode *n, bool strong, bool internal) {
    if (strong) { if (internal) n->internal_strong++; else n->local_strong++; }
    else if (!internal) n->local_weak++;
    node_check(n);
}
static void node_dec(BNode *n, bool strong, bool internal) {
    if (strong) { if (internal) n->internal_strong--; else n->local_strong--; }
    else if (!internal) n->local_weak--;
    if (n->dead) { node_free_if_unused(n); return; }
    node_check(n);
}

static BNode *node_lookup(BProc *p, uint64_t ptr) {
    for (BNode *n = p->nodes; n; n = n->next) if (n->ptr == ptr) return n;
    return NULL;
}
static BNode *node_get(BProc *p, uint64_t ptr, uint64_t cookie, uint32_t flags) {
    BNode *n = node_lookup(p, ptr);
    if (n) return n;
    n = calloc(1, sizeof *n);
    n->proc = p;
    n->ptr = ptr;
    n->cookie = cookie;
    n->flags = flags;
    n->next = p->nodes;
    p->nodes = n;
    return n;
}

static BRef *ref_lookup(BProc *p, uint32_t desc) {
    for (BRef *r = p->refs; r; r = r->next_in_proc) if (r->desc == desc) return r;
    return NULL;
}
static BRef *ref_for_node(BProc *p, BNode *n) {
    for (BRef *r = p->refs; r; r = r->next_in_proc) if (r->node == n) return r;
    BRef *r = calloc(1, sizeof *r);
    r->proc = p;
    r->node = n;
    if (n == p->ctx->mgr) r->desc = 0;
    else { do r->desc = p->next_desc++; while (!r->desc || ref_lookup(p, r->desc)); }
    r->next_in_proc = p->refs;
    p->refs = r;
    r->next_in_node = n->refs;
    n->refs = r;
    n->nrefs++;
    node_check(n);
    return r;
}
static void ref_inc(BRef *r, bool strong) {
    if (strong) { if (!r->strong) node_inc(r->node, true, true); r->strong++; }
    else r->weak++;
}
static void ref_free(BRef *r) {
    BNode *n = r->node;
    for (BRef **pp = &r->proc->refs; *pp; pp = &(*pp)->next_in_proc) if (*pp == r) { *pp = r->next_in_proc; break; }
    for (BRef **pp = &n->refs; *pp; pp = &(*pp)->next_in_node) if (*pp == r) { *pp = r->next_in_node; break; }
    n->nrefs--;
    free(r);
    if (n->dead) node_free_if_unused(n); else node_check(n);
}
// Returns true if the ref is gone.
static bool ref_dec(BRef *r, bool strong) {
    if (strong) {
        if (r->strong <= 0) return false;
        if (--r->strong == 0) node_dec(r->node, true, true);
    } else {
        if (r->weak <= 0) return false;
        r->weak--;
    }
    if (!r->strong && !r->weak && !r->death) { ref_free(r); return true; }
    return false;
}

// ---- buffers ----
static BBuf *buf_alloc(BProc *p, uint64_t size) {
    size = (size + 7) & ~7ull;
    if (!size) size = 8;
    for (BBuf *b = p->bufs; b; b = b->next) {
        if (!b->free || b->size < size) continue;
        if (b->size > size) {
            BBuf *rest = calloc(1, sizeof *rest);
            rest->off = b->off + size;
            rest->size = b->size - size;
            rest->free = true;
            rest->next = b->next;
            b->next = rest;
            b->size = size;
        }
        b->free = false;
        return b;
    }
    return NULL;
}
static void buf_free(BProc *p, BBuf *b) {
    b->free = true;
    b->t = NULL;
    free(b->objs);
    b->objs = NULL;
    b->nobjs = 0;
    for (BBuf *x = p->bufs; x; x = x->next) {   // merge free neighbours
        while (x->free && x->next && x->next->free) {
            BBuf *n = x->next;
            x->size += n->size;
            x->next = n->next;
            free(n);
        }
    }
}
static BBuf *buf_find(BProc *p, uint64_t gaddr) {
    for (BBuf *b = p->bufs; b; b = b->next) if (!b->free && p->buf_gaddr + b->off == gaddr) return b;
    return NULL;
}
static void buf_record(BBuf *b, uint32_t type, BNode *n, BRef *r, int fd) {
    b->objs = realloc(b->objs, (size_t)(b->nobjs + 1) * sizeof *b->objs);
    b->objs[b->nobjs].type = type;
    b->objs[b->nobjs].node = n;
    b->objs[b->nobjs].ref = r;
    b->objs[b->nobjs].fd = fd;
    b->nobjs++;
}
// Undoes the references a buffer's objects took when they were translated in.
static void buf_release_objs(BProc *p, BBuf *b) {
    for (int i = 0; i < b->nobjs; i++) {
        uint32_t ty = b->objs[i].type;
        if (ty == T_BINDER || ty == T_WEAK_BINDER) node_dec(b->objs[i].node, ty == T_BINDER, false);
        else if (ty == T_HANDLE || ty == T_WEAK_HANDLE) {
            BRef *r = b->objs[i].ref;
            // The ref may already be gone if userspace released it; look it up by node.
            for (BRef *x = p->refs; x; x = x->next_in_proc) if (x == r) { ref_dec(r, ty == T_HANDLE); break; }
        } else if (ty == T_FDA) {
            fd_close(p->proc, b->objs[i].fd);
        }
    }
}

// ---- threads ----
static BThread *thread_get(BProc *p, uint64_t tid) {
    for (BThread *t = p->threads; t; t = t->next) if (t->tid == tid) return t;
    BThread *t = calloc(1, sizeof *t);
    t->proc = p;
    t->tid = tid;
    int pf[2];
    if (pipe(pf) == 0) {
        t->wake_rd = pf[0]; t->wake_wr = pf[1];
        for (int i = 0; i < 2; i++) { fcntl(pf[i], F_SETFD, FD_CLOEXEC); fcntl(pf[i], F_SETFL, O_NONBLOCK); }
    }
    else t->wake_rd = t->wake_wr = -1;
    t->next = p->threads;
    p->threads = t;
    return t;
}
static void thread_free(BProc *p, BThread *t) {
    for (BThread **pp = &p->threads; *pp; pp = &(*pp)->next) if (*pp == t) { *pp = t->next; break; }
    // Transactions waiting on this thread for a reply fail.
    for (BTrans *x = t->stack; x;) {
        BTrans *parent = x->to_thread == t ? x->to_parent : x->from_parent;
        if (x->to_thread == t && x->from) {
            x->to_thread = NULL;
            BWork *w = work_new(W_ERROR);
            w->error = BR_DEAD_REPLY;
            queue_thread(x->from, w);
            x->from->stack = x->from_parent;
        }
        if (x->from == t) x->from = NULL;      // a reply for us now has nowhere to go (like the driver)
        x = parent;
    }
    t->stack = NULL;
    // Transactions we sent that are still queued elsewhere must not point at us either.
    for (BProc *q = all_bprocs; q; q = q->next_all) {
        for (BWork *w = q->todo.head; w; w = w->next) if ((w->kind == W_TRANSACTION || w->kind == W_REPLY) && w->t && w->t->from == t) w->t->from = NULL;
        for (BThread *o = q->threads; o; o = o->next)
            for (BWork *w = o->todo.head; w; w = w->next) if ((w->kind == W_TRANSACTION || w->kind == W_REPLY) && w->t && w->t->from == t) w->t->from = NULL;
    }
    for (BWork *w; (w = work_pop(&t->todo));) free(w);
    if (t->wake_rd >= 0) close(t->wake_rd);
    if (t->wake_wr >= 0) close(t->wake_wr);
    free(t);
}

// ---- transactions ----
typedef struct { uint64_t hdr_type_flags; uint64_t v; uint64_t cookie; } FlatObj;   // 24 bytes: type, flags, binder/handle, cookie

static void fail_sender(BThread *t, uint32_t err) {
    BWork *w = work_new(W_ERROR);
    w->error = err;
    queue_thread(t, w);
}

// BC_TRANSACTION / BC_REPLY. tr points at the caller's binder_transaction_data(_sg) in guest memory.
static void transact(GuestCtx *c, BProc *p, BThread *th, const uint8_t *trd, bool reply, bool sg) {
    TData tr;
    memcpy(&tr, trd, 64);
    uint64_t buffers_size = 0;
    if (sg) memcpy(&buffers_size, trd + 64, 8);
    BProc *target_proc;
    BThread *target_thread = NULL;
    BNode *target_node = NULL;
    BTrans *in_reply_to = NULL;

    if (reply) {
        in_reply_to = th->stack;
        if (!in_reply_to || in_reply_to->to_thread != th) { fail_sender(th, BR_FAILED_REPLY); return; }
        th->stack = in_reply_to->to_parent;
        target_thread = in_reply_to->from;
        if (!target_thread) {      // the caller died
            goto drop_reply;
        }
        target_proc = target_thread->proc;
    } else {
        if ((uint32_t)tr.target == 0) target_node = p->ctx->mgr;
        else { BRef *r = ref_lookup(p, (uint32_t)tr.target); target_node = r ? r->node : NULL; }
        if (!target_node || !target_node->proc || target_node->proc->dead) { fail_sender(th, BR_DEAD_REPLY); return; }
        target_proc = target_node->proc;
        if (!(tr.flags & TF_ONE_WAY) && th->stack) {
            // A synchronous call back into a process we're waiting on goes to the waiting thread.
            for (BTrans *x = th->stack; x; x = x->from_parent)
                if (x->from && x->from->proc == target_proc) { target_thread = x->from; break; }
        }
    }

    BTrans *t = calloc(1, sizeof *t);
    t->from_proc = p;
    t->to_proc = target_proc;
    t->is_reply = reply;
    t->code = tr.code;
    t->flags = tr.flags;
    t->sender_pid = (tr.flags & TF_ONE_WAY) ? 0 : p->pid;
    t->target = target_node;
    t->need_reply = !reply && !(tr.flags & TF_ONE_WAY);
    if (t->need_reply) t->from = th;
    bool secctx = target_node && (target_node->flags & FLAG_TXN_SECURITY_CTX);
    const char *sec = proc_secontext(p->proc);
    uint64_t sec_len = secctx ? strlen(sec) + 1 : 0;
    uint64_t data_al = (tr.data_size + 7) & ~7ull, offs_al = (tr.offsets_size + 7) & ~7ull;
    uint64_t total = data_al + offs_al + ((buffers_size + 7) & ~7ull) + ((sec_len + 7) & ~7ull);
    if (!target_proc->buf_host || (tr.offsets_size & 7) || tr.offsets_size > tr.data_size) goto failed;
    BBuf *b = buf_alloc(target_proc, total);
    if (!b) goto failed;
    b->t = t;
    t->buf = b;
    t->data_size = tr.data_size;
    t->offsets_size = tr.offsets_size;
    uint8_t *dst = target_proc->buf_host + b->off;
    uint64_t dst_g = target_proc->buf_gaddr + b->off;
    tr.buffer = UNTAG(tr.buffer);
    tr.offsets = UNTAG(tr.offsets);
    const uint8_t *src = (const uint8_t *)(uintptr_t)(c->base + tr.buffer);
    const uint8_t *src_offs = (const uint8_t *)(uintptr_t)(c->base + tr.offsets);
    if (tr.data_size && !mm_readable(p->proc->mm, tr.buffer, tr.data_size)) goto failed_buf;
    if (tr.offsets_size && !mm_readable(p->proc->mm, tr.offsets, tr.offsets_size)) goto failed_buf;
    memcpy(dst, src, tr.data_size);
    memcpy(dst + data_al, src_offs, tr.offsets_size);
    uint64_t sg_off = data_al + offs_al, sg_end = sg_off + buffers_size;
    if (secctx) {
        t->secctx_off = total - ((sec_len + 7) & ~7ull);
        memcpy(dst + t->secctx_off, sec, sec_len);
        t->has_secctx = true;
    }

    // Translate the objects.
    uint64_t nobj = tr.offsets_size / 8;
    const uint64_t *offs = (const uint64_t *)(dst + data_al);
    bool accept_fds = reply ? (in_reply_to->flags & TF_ACCEPT_FDS) : (target_node->flags & FLAG_ACCEPTS_FDS) || (tr.flags & TF_ACCEPT_FDS);
    for (uint64_t i = 0; i < nobj; i++) {
        uint64_t o = offs[i];
        if (o & 3 || o + 24 > tr.data_size) goto failed_objs;
        uint8_t *fp = dst + o;
        uint32_t type;
        memcpy(&type, fp, 4);
        switch (type) {
        case T_BINDER: case T_WEAK_BINDER: {
            uint64_t ptr, cookie;
            uint32_t flags;
            memcpy(&flags, fp + 4, 4); memcpy(&ptr, fp + 8, 8); memcpy(&cookie, fp + 16, 8);
            BNode *n = node_get(p, ptr, cookie, flags);
            if (n->cookie != cookie) goto failed_objs;
            BRef *r = ref_for_node(target_proc, n);
            ref_inc(r, type == T_BINDER);
            uint32_t nt = type == T_BINDER ? T_HANDLE : T_WEAK_HANDLE;
            memcpy(fp, &nt, 4);
            memset(fp + 8, 0, 16);
            memcpy(fp + 8, &r->desc, 4);
            buf_record(b, nt, NULL, r, -1);
            break;
        }
        case T_HANDLE: case T_WEAK_HANDLE: {
            uint32_t h;
            memcpy(&h, fp + 8, 4);
            BRef *r = h == 0 && !ref_lookup(p, 0) && p->ctx->mgr ? ref_for_node(p, p->ctx->mgr) : ref_lookup(p, h);
            if (!r || !r->node->proc) goto failed_objs;
            BNode *n = r->node;
            if (n->proc == target_proc) {       // coming home: becomes the owner's pointer again
                uint32_t nt = type == T_HANDLE ? T_BINDER : T_WEAK_BINDER;
                memcpy(fp, &nt, 4);
                memcpy(fp + 8, &n->ptr, 8); memcpy(fp + 16, &n->cookie, 8);
                node_inc(n, type == T_HANDLE, false);
                buf_record(b, nt, n, NULL, -1);
            } else {
                BRef *nr = ref_for_node(target_proc, n);
                ref_inc(nr, type == T_HANDLE);
                memset(fp + 8, 0, 16);
                memcpy(fp + 8, &nr->desc, 4);
                buf_record(b, type, NULL, nr, -1);
            }
            break;
        }
        case T_FD: {
            if (!accept_fds) goto failed_objs;
            int32_t gfd;
            memcpy(&gfd, fp + 8, 4);
            int h = fd_host(p->proc, gfd);
            if (h < 0) goto failed_objs;
            int nh = dup(h);
            if (nh < 0) goto failed_objs;
            if (fd_obj(h)) fd_attach_obj(nh, fd_obj(h));
            int32_t nfd = fd_install(target_proc->proc, nh, 0, true);
            if (nfd < 0) goto failed_objs;
            memcpy(fp + 8, &nfd, 4);
            memset(fp + 12, 0, 4);
            buf_record(b, T_FD, NULL, NULL, nfd);
            break;
        }
        case T_PTR: {                          // scatter-gather buffer: copy it in behind the data
            uint64_t sbuf, len, parent, parent_off;
            uint32_t flags;
            if (o + 40 > tr.data_size) goto failed_objs;
            memcpy(&flags, fp + 4, 4); memcpy(&sbuf, fp + 8, 8); memcpy(&len, fp + 16, 8);
            memcpy(&parent, fp + 24, 8); memcpy(&parent_off, fp + 32, 8);
            if (len > sg_end - sg_off) { LOGV("onirun: binder: sg buffer %llu doesn't fit\n", (unsigned long long)len); goto failed_objs; }
            if (len && !mm_readable(p->proc->mm, UNTAG(sbuf), len)) { LOGV("onirun: binder: sg buffer unreadable\n"); goto failed_objs; }
            memcpy(dst + sg_off, (const void *)(uintptr_t)(c->base + UNTAG(sbuf)), len);
            uint64_t nb = dst_g + sg_off;
            memcpy(fp + 8, &nb, 8);
            if (flags & BUFFER_HAS_PARENT) {
                if (parent >= i) goto failed_objs;
                uint8_t *pfp = dst + offs[parent];
                uint32_t pt;
                uint64_t pbuf, plen;
                memcpy(&pt, pfp, 4); memcpy(&pbuf, pfp + 8, 8); memcpy(&plen, pfp + 16, 8);
                if (pt != T_PTR || parent_off + 8 > plen) { LOGV("onirun: binder: bad sg parent\n"); goto failed_objs; }
                memcpy(dst + (pbuf - dst_g) + parent_off, &nb, 8);
            }
            sg_off += (len + 7) & ~7ull;                 // the driver keeps each buffer 8-aligned
            break;
        }
        case T_FDA: {                          // fd array inside a parent buffer
            uint64_t num, parent, parent_off;
            if (o + 32 > tr.data_size) goto failed_objs;
            memcpy(&num, fp + 8, 8); memcpy(&parent, fp + 16, 8); memcpy(&parent_off, fp + 24, 8);
            if (!accept_fds || parent >= i) goto failed_objs;
            uint8_t *pfp = dst + offs[parent];
            uint32_t pt;
            uint64_t pbuf, plen;
            memcpy(&pt, pfp, 4); memcpy(&pbuf, pfp + 8, 8); memcpy(&plen, pfp + 16, 8);
            if (pt != T_PTR || parent_off + num * 4 > plen) { LOGV("onirun: binder: bad fd array parent\n"); goto failed_objs; }
            uint8_t *fds = dst + (pbuf - dst_g) + parent_off;
            for (uint64_t k = 0; k < num; k++) {
                int32_t gfd;
                memcpy(&gfd, fds + 4 * k, 4);
                int h = fd_host(p->proc, gfd);
                int nh = h >= 0 ? dup(h) : -1;
                if (nh < 0) goto failed_objs;
                if (fd_obj(h)) fd_attach_obj(nh, fd_obj(h));
                int32_t nfd = fd_install(target_proc->proc, nh, 0, true);
                if (nfd < 0) goto failed_objs;
                memcpy(fds + 4 * k, &nfd, 4);
                buf_record(b, T_FDA, NULL, NULL, nfd);
            }
            break;
        }
        default:
            LOGV("onirun: binder: unknown object type %08x at offset %llu\n", type, (unsigned long long)o);
            goto failed_objs;
        }
    }

    // Deliver.
    if (reply) {
        target_thread->stack = in_reply_to->from_parent;
        in_reply_to->from = NULL;
        free(in_reply_to);
        BWork *w = work_new(W_REPLY);
        w->t = t;
        queue_thread(target_thread, w);
        BWork *done = work_new(W_COMPLETE);
        queue_thread(th, done);
        return;
    }
    if (t->need_reply) {
        t->from_parent = th->stack;
        th->stack = t;
    }
    BWork *done = work_new(W_COMPLETE);
    queue_thread(th, done);
    BWork *w = work_new(W_TRANSACTION);
    w->t = t;
    if (target_thread) queue_thread(target_thread, w);
    else queue_proc(target_proc, w);
    return;

failed_objs:
    buf_release_objs(target_proc, b);
failed_buf:
    buf_free(target_proc, b);
failed:
    LOGV("onirun: binder: transaction from pid %d failed (%s, code %u, data %llu, offsets %llu, sg %llu, ctx %s)\n", p->pid,
         reply ? "reply" : "call", tr.code, (unsigned long long)tr.data_size, (unsigned long long)tr.offsets_size,
         (unsigned long long)buffers_size, p->ctx->name);
    free(t);
    if (reply) {
        // The waiting caller gets the failure; we still owe ourselves a completion.
        target_thread->stack = in_reply_to->from_parent;
        free(in_reply_to);
        fail_sender(target_thread, BR_FAILED_REPLY);
        fail_sender(th, BR_TRANSACTION_COMPLETE);
        return;
    }
    fail_sender(th, BR_FAILED_REPLY);
    return;

drop_reply:
    free(in_reply_to);
    fail_sender(th, BR_TRANSACTION_COMPLETE);   // nobody is waiting; the reply just vanishes
}

// ---- the write side ----
static int do_write(GuestCtx *c, BProc *p, BThread *th, uint64_t buf, uint64_t size, uint64_t *consumed) {
    const uint8_t *base = (const uint8_t *)(uintptr_t)(c->base + UNTAG(buf));
    uint64_t pos = *consumed;
    while (pos + 4 <= size) {
        uint32_t cmd;
        memcpy(&cmd, base + pos, 4);
        uint64_t arg = pos + 4, asz = IOC_SIZE(cmd);
        if (IOC_TYPE(cmd) != 'c' || arg + asz > size) return -EINVAL;
        const uint8_t *a = base + arg;
        switch (IOC_NR(cmd)) {
        case BC_TRANSACTION: case BC_REPLY: case BC_TRANSACTION_SG: case BC_REPLY_SG: {
            int nr = IOC_NR(cmd);
            transact(c, p, th, a, nr == BC_REPLY || nr == BC_REPLY_SG, nr >= BC_TRANSACTION_SG);
            break;
        }
        case BC_FREE_BUFFER: {
            uint64_t ga;
            memcpy(&ga, a, 8);
            BBuf *b = buf_find(p, ga);
            if (b) {
                if (b->t && b->t->flags & TF_CLEAR_BUF) memset(p->buf_host + b->off, 0, b->size);
                if (b->t) b->t->buf = NULL;
                buf_release_objs(p, b);
                buf_free(p, b);
            }
            break;
        }
        case BC_INCREFS: case BC_ACQUIRE: case BC_RELEASE: case BC_DECREFS: {
            uint32_t desc;
            memcpy(&desc, a, 4);
            int nr = IOC_NR(cmd);
            BRef *r = ref_lookup(p, desc);
            if (!r && desc == 0 && p->ctx->mgr && (nr == BC_INCREFS || nr == BC_ACQUIRE)) r = ref_for_node(p, p->ctx->mgr);
            if (!r) break;
            if (nr == BC_INCREFS) ref_inc(r, false);
            else if (nr == BC_ACQUIRE) ref_inc(r, true);
            else ref_dec(r, nr == BC_RELEASE);
            break;
        }
        case BC_INCREFS_DONE: case BC_ACQUIRE_DONE: {
            uint64_t ptr, cookie;
            memcpy(&ptr, a, 8); memcpy(&cookie, a + 8, 8);
            BNode *n = node_lookup(p, ptr);
            if (!n) break;
            if (IOC_NR(cmd) == BC_ACQUIRE_DONE) { n->pending_strong = false; node_dec(n, true, false); }
            else { n->pending_weak = false; node_dec(n, false, false); }
            break;
        }
        case BC_REGISTER_LOOPER:
            if (th->looper & LOOPER_ENTERED) th->looper |= LOOPER_INVALID;
            else if (p->requested_threads == 0) th->looper |= LOOPER_INVALID;
            else { p->requested_threads--; p->started_threads++; }
            th->looper |= LOOPER_REGISTERED;
            break;
        case BC_ENTER_LOOPER:
            if (th->looper & LOOPER_REGISTERED) th->looper |= LOOPER_INVALID;
            th->looper |= LOOPER_ENTERED;
            break;
        case BC_EXIT_LOOPER:
            th->looper |= LOOPER_EXITED;
            break;
        case BC_REQUEST_DEATH_NOTIFICATION: case BC_CLEAR_DEATH_NOTIFICATION: {
            uint32_t desc;
            uint64_t cookie;
            memcpy(&desc, a, 4); memcpy(&cookie, a + 4, 8);
            BRef *r = ref_lookup(p, desc);
            if (!r) break;
            if (IOC_NR(cmd) == BC_REQUEST_DEATH_NOTIFICATION) {
                if (r->death) break;
                r->death = true;
                r->death_cookie = cookie;
                if (!r->node->proc || r->node->dead) {       // already dead: tell them now
                    BWork *w = work_new(W_DEAD_BINDER);
                    w->cookie = cookie;
                    r->dead_sent = true;
                    queue_proc(p, w);
                }
            } else {
                if (!r->death || r->death_cookie != cookie) break;
                r->death = false;
                BWork *w = work_new(W_CLEAR_DEATH_DONE);
                w->cookie = cookie;
                queue_thread(th, w);
                if (!r->strong && !r->weak) ref_free(r);
            }
            break;
        }
        case BC_DEAD_BINDER_DONE: {
            uint64_t cookie;
            memcpy(&cookie, a, 8);
            for (BRef *r = p->refs; r; r = r->next_in_proc)
                if (r->death && r->dead_sent && r->death_cookie == cookie) {
                    r->death = false;
                    if (!r->strong && !r->weak) ref_free(r);
                    break;
                }
            break;
        }
        case BC_ACQUIRE_RESULT: case BC_ATTEMPT_ACQUIRE:
            return -EINVAL;
        default:
            fprintf(stderr, "onirun: binder: unknown command %08x\n", cmd);
            return -EINVAL;
        }
        pos = arg + asz;
        *consumed = pos;
    }
    return 0;
}

// ---- the read side ----
static bool can_take_proc_work(BThread *t) {
    return !t->stack && !t->todo.head && (t->looper & (LOOPER_ENTERED | LOOPER_REGISTERED));
}

static void put_tdata(BTrans *t, BProc *p, TData *tr) {
    memset(tr, 0, sizeof *tr);
    if (t->target && !t->is_reply) { tr->target = t->target->ptr; tr->cookie = t->target->cookie; }
    tr->code = t->code;
    tr->flags = t->flags;
    tr->sender_pid = t->sender_pid;
    tr->sender_euid = t->from_proc ? t->from_proc->proc->euid : 0;
    tr->data_size = t->data_size;
    tr->offsets_size = t->offsets_size;
    tr->buffer = p->buf_gaddr + t->buf->off;
    tr->offsets = tr->buffer + ((t->data_size + 7) & ~7ull);
}

// Returns the number of bytes written, or a negative errno.
static int64_t do_read(GuestCtx *c, BProc *p, BThread *th, uint64_t buf, uint64_t size, uint64_t *consumed, bool nonblock) {
    uint8_t *out = (uint8_t *)(uintptr_t)(c->base + UNTAG(buf));
    uint64_t pos = *consumed;
#define PUT(ptr, n) do { memcpy(out + pos, (ptr), (n)); pos += (n); } while (0)
#define PUT32(v) do { uint32_t _v = (v); PUT(&_v, 4); } while (0)
    if (pos == 0 && size >= 4) PUT32(BR_NOOP);
    uint64_t start = pos;
    for (;;) {
        bool proc_work = can_take_proc_work(th);
        BWork *w = NULL;
        if (th->todo.head) w = work_pop(&th->todo);
        else if (proc_work && p->todo.head) w = work_pop(&p->todo);
        if (!w) {
            if (pos > start) break;                           // delivered something already
            if (nonblock) break;
            // Wait for work. Mark us available for process work so senders wake us.
            th->waiting_proc = proc_work;
            if (proc_work) p->ready_threads++;
            th->looper |= LOOPER_WAITING;
            pthread_mutex_unlock(&blk);
            struct pollfd pf = { th->wake_rd, POLLIN, 0 };
            int r = poll(&pf, 1, -1);
            BLK_LOCK(__func__);
            th->looper &= ~LOOPER_WAITING;
            if (th->waiting_proc) p->ready_threads--;
            th->waiting_proc = false;
            char junk[64];
            while (read(th->wake_rd, junk, sizeof junk) > 0) {}      // drain every pending wake
            if (r < 0) { *consumed = pos; return -EINTR; }
            if (c->kill) { *consumed = pos; return -EINTR; }
            continue;
        }
        switch (w->kind) {
        case W_TRANSACTION: case W_REPLY: {
            BTrans *t = w->t;
            uint32_t cmd = w->kind == W_REPLY ? BR_REPLY : t->has_secctx ? BR_TRANSACTION_SEC_CTX : BR_TRANSACTION;
            uint64_t need = 4 + IOC_SIZE(cmd);
            if (pos + need > size) { work_push_front(&th->todo, w); goto out; }   // leave it for the next read
            if (w->kind == W_TRANSACTION && t->need_reply) {
                t->to_thread = th;
                t->to_parent = th->stack;
                th->stack = t;
            }
            TData tr;
            put_tdata(t, p, &tr);
            PUT32(cmd);
            PUT(&tr, 64);
            if (cmd == BR_TRANSACTION_SEC_CTX) { uint64_t sc = tr.buffer + t->secctx_off; PUT(&sc, 8); }
            if (w->kind == W_REPLY || !t->need_reply) {   // nothing more to track for these
                if (t->buf) t->buf->t = NULL;
                free(t);
            }
            free(w);
            goto out;                                      // one transaction per read, like the driver
        }
        case W_COMPLETE: PUT32(BR_TRANSACTION_COMPLETE); free(w); break;
        case W_ERROR: PUT32(w->error); free(w); break;
        case W_DEAD_BINDER: PUT32(BR_DEAD_BINDER); PUT(&w->cookie, 8); free(w); break;
        case W_CLEAR_DEATH_DONE: PUT32(BR_CLEAR_DEATH_NOTIFICATION_DONE); PUT(&w->cookie, 8); free(w); break;
        case W_NODE: {
            BNode *n = w->node;
            n->queued = false;
            free(w);
            bool strong = n->internal_strong || n->local_strong;
            bool weak = n->nrefs || n->local_weak || strong;
            uint64_t pc[2] = { n->ptr, n->cookie };
            if (pos + 4 * (4 + 16) > size) { node_check(n); goto out; }
            if (weak && !n->has_weak) { n->has_weak = n->pending_weak = true; n->local_weak++; PUT32(BR_INCREFS); PUT(pc, 16); }
            if (strong && !n->has_strong) { n->has_strong = n->pending_strong = true; n->local_strong++; PUT32(BR_ACQUIRE); PUT(pc, 16); }
            if (!strong && n->has_strong) { n->has_strong = false; PUT32(BR_RELEASE); PUT(pc, 16); }
            if (!weak && n->has_weak) { n->has_weak = false; PUT32(BR_DECREFS); PUT(pc, 16); }
            if (!weak && !strong) node_free_if_unused(n);
            break;
        }
        }
        if (pos + 4 > size) break;
    }
out:
    // The driver asks for another looper when nobody is left to take process work.
    if (p->requested_threads == 0 && p->ready_threads == 0 && p->started_threads < p->max_threads &&
        (th->looper & (LOOPER_ENTERED | LOOPER_REGISTERED)) && pos + 4 <= size && pos > start) {
        bool other_waiting = false;
        for (BThread *t = p->threads; t; t = t->next) if (t->waiting_proc) other_waiting = true;
        if (!other_waiting) { p->requested_threads++; PUT32(BR_SPAWN_LOOPER); }
    }
    *consumed = pos;
    pthread_mutex_lock(&p->obj->lock);
    obj_set_ready(p->obj, proc_has_work(p));
    pthread_mutex_unlock(&p->obj->lock);
    return 0;
#undef PUT
#undef PUT32
}

// ---- ioctl ----
static int64_t binder_ioctl(FdObj *o, GuestCtx *c, uint64_t req, uint64_t arg) {
    BProc *p = o->data;
    if (IOC_TYPE(req) != 'b') return -ENOTTY;
    int64_t r = 0;
    BLK_LOCK(__func__);
    if (p->dead) { pthread_mutex_unlock(&blk); return -ENODEV; }
    BThread *th = thread_get(p, c->tid);
    cur_th = th;
    switch (IOC_NR(req)) {
    case 1: {   // BINDER_WRITE_READ
        uint64_t *bwr = gptr(arg);   // write_size, write_consumed, write_buffer, read_size, read_consumed, read_buffer
        if (!bwr) { r = -EFAULT; break; }
        if (bwr[0] > bwr[1]) {
            int e = do_write(c, p, th, bwr[2], bwr[0], &bwr[1]);
            if (e) { bwr[4] = 0; r = e; break; }
        }
        if (bwr[3] > bwr[4]) {
            bool nonblock = fcntl(o->rd, F_GETFL) & O_NONBLOCK;
            r = do_read(c, p, th, bwr[5], bwr[3], &bwr[4], nonblock);
        }
        break;
    }
    case 5: p->max_threads = *(int32_t *)gptr(arg); break;                 // BINDER_SET_MAX_THREADS
    case 7: case 13: {                                                     // BINDER_SET_CONTEXT_MGR(_EXT)
        if (p->ctx->mgr && p->ctx->mgr->proc && !p->ctx->mgr->proc->dead) { r = -EBUSY; break; }
        uint64_t ptr = 0, cookie = 0;
        uint32_t flags = 0;
        if (IOC_NR(req) == 13) { const uint8_t *fo = gptr(arg); memcpy(&flags, fo + 4, 4); memcpy(&ptr, fo + 8, 8); memcpy(&cookie, fo + 16, 8); }
        BNode *n = node_get(p, ptr, cookie, flags);
        n->local_weak++;
        n->local_strong++;
        n->has_strong = n->has_weak = true;
        p->ctx->mgr = n;
        break;
    }
    case 8: {   // BINDER_THREAD_EXIT
        thread_free(p, th);
        break;
    }
    case 9: *(int32_t *)gptr(arg) = 8; break;                              // BINDER_VERSION
    case 12: {  // BINDER_GET_NODE_INFO_FOR_REF
        uint32_t *info = gptr(arg);
        BRef *ref = ref_lookup(p, info[0]);
        if (!ref) { r = -EINVAL; break; }
        info[1] = (uint32_t)(ref->node->internal_strong + ref->node->local_strong);
        info[2] = (uint32_t)(ref->node->nrefs + ref->node->local_weak);
        break;
    }
    case 16: break;                                                        // BINDER_ENABLE_ONEWAY_SPAM_DETECTION
    case 17: { uint32_t *ee = gptr(arg); ee[0] = 0; ee[1] = 0; ee[2] = 0; break; }   // BINDER_GET_EXTENDED_ERROR
    case 3: case 6: case 11: case 14: case 15: break;                      // timeouts, priorities, debug info, freeze
    default: r = -EINVAL;
    }
    cur_th = NULL;
    pthread_mutex_unlock(&blk);
    return r;
}

static void binder_mapped(FdObj *o, GuestCtx *c, uint64_t gaddr, uint64_t len, int prot, uint64_t off) {
    (void)c; (void)prot; (void)off;
    BProc *p = o->data;
    BLK_LOCK(__func__);
    if (!p->buf_host) {
        ftruncate(o->mmap_fd, (off_t)len);
        void *alias = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, o->mmap_fd, 0);
        if (alias != MAP_FAILED) {
            p->buf_host = alias;
            p->buf_gaddr = gaddr;
            p->buf_size = len;
            BBuf *b = calloc(1, sizeof *b);
            b->size = len;
            b->free = true;
            p->bufs = b;
        }
    }
    pthread_mutex_unlock(&blk);
}

// The last fd to this BProc closed: the process is gone as far as binder is concerned.
static void binder_release(FdObj *o) {
    BProc *p = o->data;
    BLK_LOCK(__func__);
    p->dead = true;
    for (BProc **pp = &all_bprocs; *pp; pp = &(*pp)->next_all) if (*pp == p) { *pp = p->next_all; break; }
    if (p->ctx->mgr && p->ctx->mgr->proc == p) p->ctx->mgr = NULL;
    // Our references go away.
    while (p->refs) { BRef *r = p->refs; r->death = false; while (r->strong) ref_dec(r, true); while (r->weak) ref_dec(r, false); if (p->refs == r) ref_free(r); }
    // Our nodes die: holders with death notifications hear about it.
    for (BNode *n = p->nodes; n;) {
        BNode *next = n->next;
        n->dead = true;
        n->proc = NULL;
        for (BRef *r = n->refs; r; r = r->next_in_node)
            if (r->death && !r->dead_sent) { BWork *w = work_new(W_DEAD_BINDER); w->cookie = r->death_cookie; r->dead_sent = true; queue_proc(r->proc, w); }
        if (!n->refs) free(n);
        n = next;
    }
    p->nodes = NULL;
    // Callers waiting on us get dead replies; work queued to us is dropped.
    for (BWork *w; (w = work_pop(&p->todo));) {
        if ((w->kind == W_TRANSACTION) && w->t) {
            if (w->t->from) { w->t->from->stack = w->t->from_parent; fail_sender(w->t->from, BR_DEAD_REPLY); }
            free(w->t);
        }
        free(w);
    }
    while (p->threads) {
        BThread *t = p->threads;
        for (BWork *w; (w = work_pop(&t->todo));) {
            if ((w->kind == W_TRANSACTION) && w->t) {
                if (w->t->from) { w->t->from->stack = w->t->from_parent; fail_sender(w->t->from, BR_DEAD_REPLY); }
                free(w->t);
            } else if (w->kind == W_REPLY && w->t) free(w->t);
            free(w);
        }
        thread_free(p, t);
    }
    for (BBuf *b = p->bufs; b;) { BBuf *n = b->next; free(b->objs); free(b); b = n; }
    if (p->buf_host) munmap(p->buf_host, p->buf_size);
    // Nothing anywhere may still name this process.
    for (BProc *q = all_bprocs; q; q = q->next_all) {
        for (BWork *w = q->todo.head; w; w = w->next)
            if ((w->kind == W_TRANSACTION || w->kind == W_REPLY) && w->t) { if (w->t->from_proc == p) w->t->from_proc = NULL; if (w->t->to_proc == p) w->t->to_proc = NULL; }
        for (BThread *o = q->threads; o; o = o->next) {
            for (BWork *w = o->todo.head; w; w = w->next)
                if ((w->kind == W_TRANSACTION || w->kind == W_REPLY) && w->t) { if (w->t->from_proc == p) w->t->from_proc = NULL; if (w->t->to_proc == p) w->t->to_proc = NULL; }
            for (BTrans *x = o->stack; x; x = x->to_thread == o ? x->to_parent : x->from_parent) { if (x->from_proc == p) x->from_proc = NULL; if (x->to_proc == p) x->to_proc = NULL; }
        }
    }
    pthread_mutex_unlock(&blk);
    free(p);
}


static void dump_trans(const char *tag, BTrans *t) {
    fprintf(stderr, "      %s %s code %u flags %#x from %d/%llu to %d/%llu need_reply %d size %llu\n", tag,
            t->is_reply ? "reply" : "txn", t->code, t->flags, t->from_proc ? t->from_proc->pid : -1,
            (unsigned long long)(t->from ? t->from->tid : 0), t->to_proc ? t->to_proc->pid : -1,
            (unsigned long long)(t->to_thread ? t->to_thread->tid : 0), t->need_reply, (unsigned long long)t->data_size);
}

// SIGINFO companion of proc_dump_all: every binder thread's transaction stack and pending work.
void binder_dump(void) {
    int tries = 0;
    while (pthread_mutex_trylock(&blk) != 0) {
        if (++tries > 200) { fprintf(stderr, "onirun: binder lock is held; dumping unlocked (owner tid %llu in %s)\n", (unsigned long long)blk_owner, blk_where ? blk_where : "?"); break; }
        usleep(10000);
    }
    bool locked = tries <= 200;
    fprintf(stderr, "onirun: --- binder ---\n");
    for (BProc *p = all_bprocs; p; p = p->next_all) {
        int ntodo = 0;
        for (BWork *w = p->todo.head; w; w = w->next) ntodo++;
        fprintf(stderr, "  [%d] %s threads max %d started %d ready %d requested %d proc-todo %d\n", p->pid, p->ctx->name,
                p->max_threads, p->started_threads, p->ready_threads, p->requested_threads, ntodo);
        for (BWork *w = p->todo.head; w; w = w->next)
            if (w->kind == W_TRANSACTION && w->t) dump_trans("queued", w->t);
            else fprintf(stderr, "      queued work kind %d\n", w->kind);
        for (BThread *t = p->threads; t; t = t->next) {
            int n = 0;
            for (BWork *w = t->todo.head; w; w = w->next) n++;
            if (!t->stack && !n) continue;
            fprintf(stderr, "    tid %llu looper %#x waiting_proc %d todo %d\n", (unsigned long long)t->tid, t->looper, t->waiting_proc, n);
            for (BTrans *x = t->stack; x; x = x->from == t ? x->from_parent : x->to_parent) dump_trans("stack", x);
            for (BWork *w = t->todo.head; w; w = w->next)
                if ((w->kind == W_TRANSACTION || w->kind == W_REPLY) && w->t) dump_trans("todo", w->t);
                else fprintf(stderr, "      todo work kind %d err %#x\n", w->kind, w->error);
        }
    }
    if (locked) pthread_mutex_unlock(&blk);
}

static const FdOps binder_ops = { "binder", NULL, NULL, binder_ioctl, binder_mapped, binder_release };

static BCtx *ctx_get(const char *name) {
    for (BCtx *x = ctxs; x; x = x->next) if (!strcmp(x->name, name)) return x;
    BCtx *x = calloc(1, sizeof *x);
    x->name = strdup(name);
    x->next = ctxs;
    ctxs = x;
    return x;
}

int binder_open(Proc *proc, const char *context, int lflags) {
    BProc *p = calloc(1, sizeof *p);
    p->proc = proc;
    p->pid = proc->pid;
    p->next_desc = 1;
    p->max_threads = 15;
    BLK_LOCK(__func__);
    p->next_all = all_bprocs;
    all_bprocs = p;
    p->ctx = ctx_get(context);
    pthread_mutex_unlock(&blk);
    FdObj *o = obj_new(&binder_ops, p, true);
    if (!o) { free(p); return -ENFILE; }
    o->mmap_fd = host_tmpfile();
    o->map_shared = true;                // libbinder maps it private; it must still see our writes
    ftruncate(o->mmap_fd, 8 << 20);      // libbinder maps about 1 MiB; hwbinder up to 8
    p->obj = o;
    if (lflags & 04000) fcntl(o->rd, F_SETFL, O_NONBLOCK);
    char path[32];
    snprintf(path, sizeof path, "/dev/%s", context);
    fd_set_path(o->rd, path);
    return fd_install_obj(proc, o->rd, o, lflags & 02000000);
}
