// bpf(2) without a kernel: the network stack keeps its state in BPF maps (uid ownership, stats,
// configuration) and expects the programs netbpfload pinned under /sys/fs/bpf. The maps are kept
// here, in memory, shared by every guest process like the kernel's; programs are tokens that can be
// looked up, attached and queried but never run (there is no traffic for them to see).
//
// tools/mkbpffs.py lays the pinned files out in the rootfs and writes /oni/bpf.txt, which names
// each pinned object with its type and sizes; objects created at run time (BPF_MAP_CREATE,
// BPF_PROG_LOAD) and pinned by the guest join the same registry.
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sys.h"

enum { BPF_MAP_CREATE, BPF_MAP_LOOKUP_ELEM, BPF_MAP_UPDATE_ELEM, BPF_MAP_DELETE_ELEM, BPF_MAP_GET_NEXT_KEY, BPF_PROG_LOAD,
       BPF_OBJ_PIN, BPF_OBJ_GET, BPF_PROG_ATTACH, BPF_PROG_DETACH, BPF_PROG_TEST_RUN, BPF_PROG_GET_NEXT_ID, BPF_MAP_GET_NEXT_ID,
       BPF_PROG_GET_FD_BY_ID, BPF_MAP_GET_FD_BY_ID, BPF_OBJ_GET_INFO_BY_FD, BPF_PROG_QUERY, BPF_RAW_TRACEPOINT_OPEN,
       BPF_BTF_LOAD, BPF_BTF_GET_FD_BY_ID, BPF_TASK_FD_QUERY, BPF_MAP_LOOKUP_AND_DELETE_ELEM, BPF_MAP_FREEZE };
enum { MAP_HASH = 1, MAP_ARRAY = 2, MAP_PERCPU_HASH = 5, MAP_PERCPU_ARRAY = 6, MAP_LRU_HASH = 9, MAP_LRU_PERCPU_HASH = 10,
       MAP_RINGBUF = 27 };

typedef struct BpfObj {
    bool is_map;
    uint32_t type, id;
    char name[16];
    char *pin;                 // path under /sys/fs/bpf, or NULL
    // maps
    uint32_t key_size, value_size, max_entries, stride;   // stride: bytes per value as user space sees it (per-CPU maps carry one per CPU)
    uint8_t *array;            // array maps: max_entries * stride
    struct { uint8_t *k, *v; } *ent;   // hash maps
    int n, cap;
    int host_fd;               // ring buffers: a zero-filled file the consumer maps
    struct BpfObj *next;
} BpfObj;

static BpfObj *objs;
static pthread_mutex_t bpf_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_id = 1;
static int ncpu;

static int64_t bpf_ioctl_none(FdObj *o, GuestCtx *c, uint64_t req, uint64_t arg) { (void)o; (void)c; (void)req; (void)arg; return -ENOTTY; }
static void bpf_release(FdObj *o) { (void)o; }   // objects outlive their fds (pinned, or simply leaked like a kernel would not)
static const FdOps bpf_ops = { "bpf", NULL, NULL, bpf_ioctl_none, NULL, bpf_release };

static BpfObj *obj_new_bpf(bool is_map) {
    BpfObj *b = calloc(1, sizeof *b);
    b->is_map = is_map;
    b->host_fd = -1;
    b->id = next_id++;
    b->next = objs;
    objs = b;
    return b;
}

static void map_setup(BpfObj *m) {
    bool percpu = m->type == MAP_PERCPU_HASH || m->type == MAP_PERCPU_ARRAY || m->type == MAP_LRU_PERCPU_HASH;
    m->stride = percpu ? ((m->value_size + 7) & ~7u) * (uint32_t)ncpu : m->value_size;
    if (m->type == MAP_ARRAY || m->type == MAP_PERCPU_ARRAY) m->array = calloc((size_t)m->max_entries, m->stride ? m->stride : 1);
    if (m->type == MAP_RINGBUF) {
        m->host_fd = host_tmpfile();
        if (m->host_fd >= 0) ftruncate(m->host_fd, (off_t)(2 * PG + 2 * (uint64_t)m->max_entries + PG));
    }
}

void bpf_init(void) {
    ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
    char path[1200];
    snprintf(path, sizeof path, "%s/oni/bpf.txt", g_rootfs);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024], kind[8], pin[600];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned type, ks, vs, me, uid, gid, mode;
        int got = sscanf(line, "%7s %599s %u %u %u %u %u %u %o", kind, pin, &type, &ks, &vs, &me, &uid, &gid, &mode);
        if (got < 3) continue;
        BpfObj *b = obj_new_bpf(!strcmp(kind, "map"));
        b->type = type;
        b->pin = strdup(pin);
        const char *base = strrchr(pin, '/');
        snprintf(b->name, sizeof b->name, "%.15s", base ? base + 1 + (strncmp(base + 1, "map_", 4) == 0 || strncmp(base + 1, "prog_", 5) == 0 ? (base[1] == 'm' ? 4 : 5) : 0) : pin);
        if (b->is_map && got >= 6) { b->key_size = ks; b->value_size = vs; b->max_entries = me; map_setup(b); }
        n++;
    }
    fclose(f);
    LOGV("onirun: %d pinned BPF objects\n", n);
}

static BpfObj *find_pinned(const char *path) {
    for (BpfObj *b = objs; b; b = b->next) if (b->pin && !strcmp(b->pin, path)) return b;
    return NULL;
}

static BpfObj *obj_of_fd(Proc *p, int gfd) {
    int h = fd_host(p, gfd);
    FdObj *o = h >= 0 ? fd_obj(h) : NULL;
    return o && o->ops == &bpf_ops ? o->data : NULL;
}

static int install(Proc *p, BpfObj *b, int lflags) {
    FdObj *o = obj_new(&bpf_ops, b, true);
    if (!o) return -ENFILE;
    if (b->host_fd >= 0) { o->mmap_fd = dup(b->host_fd); o->map_shared = true; }
    char path[700];
    snprintf(path, sizeof path, "anon_inode:bpf-%s", b->is_map ? "map" : "prog");
    fd_set_path(o->rd, path);
    return fd_install_obj(p, o->rd, o, (lflags & 02000000) != 0);
}

// ---- hash maps ----
static int hash_find(BpfObj *m, const uint8_t *key) {
    for (int i = 0; i < m->n; i++) if (!memcmp(m->ent[i].k, key, m->key_size)) return i;
    return -1;
}

static int64_t map_lookup(BpfObj *m, const uint8_t *key, uint8_t *value) {
    if (m->array) {
        uint32_t idx; memcpy(&idx, key, 4);
        if (idx >= m->max_entries) return -ENOENT;
        memcpy(value, m->array + (size_t)idx * m->stride, m->stride);
        return 0;
    }
    int i = hash_find(m, key);
    if (i < 0) return -ENOENT;
    memcpy(value, m->ent[i].v, m->stride);
    return 0;
}

static int64_t map_update(BpfObj *m, const uint8_t *key, const uint8_t *value, uint64_t flags) {
    if (m->array) {
        uint32_t idx; memcpy(&idx, key, 4);
        if (idx >= m->max_entries) return -E2BIG;
        if (flags == 1) return -EEXIST;                    // BPF_NOEXIST: array slots always exist
        memcpy(m->array + (size_t)idx * m->stride, value, m->stride);
        return 0;
    }
    int i = hash_find(m, key);
    if (i >= 0) {
        if (flags == 1) return -EEXIST;
        memcpy(m->ent[i].v, value, m->stride);
        return 0;
    }
    if (flags == 2) return -ENOENT;                        // BPF_EXIST
    if (m->max_entries && (uint32_t)m->n >= m->max_entries && m->type != MAP_LRU_HASH && m->type != MAP_LRU_PERCPU_HASH) return -E2BIG;
    if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 16; m->ent = realloc(m->ent, (size_t)m->cap * sizeof *m->ent); }
    m->ent[m->n].k = malloc(m->key_size);
    m->ent[m->n].v = malloc(m->stride);
    memcpy(m->ent[m->n].k, key, m->key_size);
    memcpy(m->ent[m->n].v, value, m->stride);
    m->n++;
    return 0;
}

static int64_t map_delete(BpfObj *m, const uint8_t *key) {
    if (m->array) return -EINVAL;
    int i = hash_find(m, key);
    if (i < 0) return -ENOENT;
    free(m->ent[i].k); free(m->ent[i].v);
    m->ent[i] = m->ent[--m->n];
    return 0;
}

static int64_t map_next_key(BpfObj *m, const uint8_t *key, uint8_t *next) {
    if (m->array) {
        uint32_t idx = 0;
        if (key) { memcpy(&idx, key, 4); idx = idx >= m->max_entries ? 0 : idx + 1; }
        if (idx >= m->max_entries) return -ENOENT;
        memcpy(next, &idx, 4);
        return 0;
    }
    int i = key ? hash_find(m, key) : -1;
    int j = i < 0 ? 0 : i + 1;                             // unknown key: start over, like the kernel
    if (j >= m->n) return -ENOENT;
    memcpy(next, m->ent[j].k, m->key_size);
    return 0;
}

// ---- the system call ----
int64_t bpf_syscall(GuestCtx *c, int cmd, uint8_t *attr, uint64_t size) {
    Proc *p = c->proc;
    if (!attr || size < 16) return -EINVAL;
    uint32_t a32[8] = {0};
    uint64_t a64[6] = {0};
    memcpy(a32, attr, size < 32 ? size : 32);
    memcpy(a64, attr, size < 48 ? size : 48);
    int64_t r = 0;
    pthread_mutex_lock(&bpf_lock);
    switch (cmd) {
    case BPF_MAP_CREATE: {
        BpfObj *m = obj_new_bpf(true);
        m->type = a32[0]; m->key_size = a32[1]; m->value_size = a32[2]; m->max_entries = a32[3];
        if (size >= 44) memcpy(m->name, attr + 28, 15);
        map_setup(m);
        r = install(p, m, 02000000);
        break;
    }
    case BPF_PROG_LOAD: {
        BpfObj *b = obj_new_bpf(false);
        b->type = a32[0];
        if (size >= 64) memcpy(b->name, attr + 48, 15);
        r = install(p, b, 02000000);
        break;
    }
    case BPF_OBJ_PIN: case BPF_OBJ_GET: {
        const char *path = (const char *)(uintptr_t)(c->base + UNTAG(a64[0]));
        if (cmd == BPF_OBJ_GET) {
            BpfObj *b = find_pinned(path);
            r = b ? install(p, b, 02000000) : -ENOENT;
        } else {
            BpfObj *b = obj_of_fd(p, (int)a32[2]);
            if (!b) r = -EBADF;
            else if (find_pinned(path)) r = -EEXIST;
            else { free(b->pin); b->pin = strdup(path); }
        }
        break;
    }
    case BPF_MAP_LOOKUP_ELEM: case BPF_MAP_UPDATE_ELEM: case BPF_MAP_DELETE_ELEM: case BPF_MAP_GET_NEXT_KEY:
    case BPF_MAP_LOOKUP_AND_DELETE_ELEM: {
        BpfObj *m = obj_of_fd(p, (int)a32[0]);
        if (!m || !m->is_map) { r = -EBADF; break; }
        if (m->type == MAP_RINGBUF) { r = -ENOTSUP; break; }
        uint8_t *key = a64[1] ? (uint8_t *)(uintptr_t)(c->base + UNTAG(a64[1])) : NULL;
        uint8_t *val = a64[2] ? (uint8_t *)(uintptr_t)(c->base + UNTAG(a64[2])) : NULL;
        if (cmd == BPF_MAP_LOOKUP_ELEM) r = key && val ? map_lookup(m, key, val) : -EFAULT;
        else if (cmd == BPF_MAP_UPDATE_ELEM) r = key && val ? map_update(m, key, val, a64[3]) : -EFAULT;
        else if (cmd == BPF_MAP_DELETE_ELEM) r = key ? map_delete(m, key) : -EFAULT;
        else if (cmd == BPF_MAP_GET_NEXT_KEY) r = val ? map_next_key(m, key, val) : -EFAULT;
        else { r = key && val ? map_lookup(m, key, val) : -EFAULT; if (!r) map_delete(m, key); }
        break;
    }
    case BPF_OBJ_GET_INFO_BY_FD: {
        BpfObj *b = obj_of_fd(p, (int)a32[0]);
        uint32_t len = a32[1];
        uint8_t *info = a64[1] ? (uint8_t *)(uintptr_t)(c->base + UNTAG(a64[1])) : NULL;
        if (!b) { r = -EBADF; break; }
        if (!info) { r = -EFAULT; break; }
        uint8_t buf[96] = {0};
        memcpy(buf + 0, &b->type, 4);
        memcpy(buf + 4, &b->id, 4);
        if (b->is_map) {
            memcpy(buf + 8, &b->key_size, 4);
            memcpy(buf + 12, &b->value_size, 4);
            memcpy(buf + 16, &b->max_entries, 4);
            memcpy(buf + 24, b->name, 16);
        } else {
            memcpy(buf + 64, b->name, 16);
        }
        memcpy(info, buf, len < sizeof buf ? len : sizeof buf);
        break;
    }
    case BPF_PROG_ATTACH: case BPF_PROG_DETACH: case BPF_MAP_FREEZE: case BPF_RAW_TRACEPOINT_OPEN:
        break;                                             // nothing runs, so attaching is a formality
    case BPF_PROG_QUERY:
        if (size >= 28) memset(attr + 24, 0, 4);           // prog_cnt = 0
        break;
    case BPF_PROG_GET_NEXT_ID: case BPF_MAP_GET_NEXT_ID:
        r = -ENOENT;
        break;
    default:
        LOGV("onirun: bpf command %d unsupported\n", cmd);
        r = -EINVAL;
    }
    pthread_mutex_unlock(&bpf_lock);
    return r;
}
