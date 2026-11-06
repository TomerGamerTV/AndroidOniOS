// A virtio-gpu device for the guest, backed by the host's rutabaga/gfxstream (the same libraries
// the QEMU app uses). /dev/dri/renderD128 is the render node: the gfxstream guest libraries,
// minigbm and ANGLE talk to it with the virtgpu ioctls, and we forward them to rutabaga, mapping
// GPU buffers straight into the guest's window. /dev/dri/card0 is a tiny KMS device with one
// connector, CRTC and plane; atomic commits give us the frames to present.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include "sys.h"
#include "uapi/drm/drm.h"
#include "uapi/drm/drm_mode.h"
#include "uapi/drm/drm_fourcc.h"
#include "uapi/drm/virtgpu_drm.h"
#include <rutabaga_gfx/rutabaga_gfx_ffi.h>

// ---- state ----
#ifndef DRM_PLANE_TYPE_PRIMARY
#define DRM_PLANE_TYPE_OVERLAY 0
#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_PLANE_TYPE_CURSOR 2
#endif

typedef struct GpuRes {
    uint32_t res_id;
    int refs;
    uint64_t size;
    uint32_t blob_mem;        // 0 = classic 3D resource
    uint32_t width, height, format, stride;
    void *backing;            // guest-visible memory for guest-backed resources (host alloc)
    void *map;                // rutabaga mapping (host3d blobs)
    uint64_t map_size;
    struct GpuRes *next;
} GpuRes;

typedef struct GpuFile {
    uint32_t ctx_id;          // 0 until a context exists
    uint32_t capset;
    bool card;
    GpuRes **handles;         // handle -> resource (index 0 unused)
    int nhandles;
    // KMS
    uint8_t events[4096];
    int ev_len;
    struct { uint32_t id; GpuRes *res; uint32_t w, h; } fbs[64];
    struct { uint32_t id; void *data; uint32_t len; } blobs[16];
    uint32_t cur_fb;
    FdObj *o;
} GpuFile;

typedef struct GpuFence { uint64_t id; bool done; struct GpuFence *next; } GpuFence;
typedef struct SyncFile { uint64_t *fences; int n; FdObj *o; struct SyncFile *next; } SyncFile;

static struct {
    struct rutabaga *r;
    pthread_mutex_t lock;     // rutabaga calls and the tables
    pthread_mutex_t flock;    // fences and sync files (also taken from gfxstream's threads)
    uint32_t next_res, next_ctx;
    uint64_t next_fence;
    GpuRes *resources;
    GpuFence *fences;
    SyncFile *syncs;
    uint32_t width, height;
    uint8_t *frame;           // last presented frame, RGBA
    uint64_t frames;
    const char *frames_dir;
} G = { .lock = PTHREAD_MUTEX_INITIALIZER, .flock = PTHREAD_MUTEX_INITIALIZER, .next_res = 1, .next_ctx = 1, .next_fence = 1 };

#define LOCK() pthread_mutex_lock(&G.lock)
#define UNLOCK() pthread_mutex_unlock(&G.lock)

// ---- fences and sync files ----
static void fence_cb(uint64_t user_data, const struct rutabaga_fence *f) {
    (void)user_data;
    pthread_mutex_lock(&G.flock);
    for (GpuFence *x = G.fences; x; x = x->next) if (x->id == f->fence_id) x->done = true;
    for (SyncFile *s = G.syncs; s; s = s->next) {
        bool all = true;
        for (int i = 0; i < s->n && all; i++) {
            bool found = false;
            for (GpuFence *x = G.fences; x; x = x->next) if (x->id == s->fences[i]) { found = true; all = x->done; }
            if (!found) all = true;                       // an already forgotten fence is done
        }
        if (all) { pthread_mutex_lock(&s->o->lock); obj_set_ready(s->o, true); pthread_mutex_unlock(&s->o->lock); }
    }
    pthread_mutex_unlock(&G.flock);
}

static void debug_cb(uint64_t user_data, const struct rutabaga_debug *d) {
    (void)user_data;
    if (d->debug_type == RUTABAGA_DEBUG_ERROR || g_verbose) fprintf(stderr, "gfxstream: %s\n", d->message);
}

static bool fences_done_locked(const uint64_t *ids, int n) {
    for (int i = 0; i < n; i++)
        for (GpuFence *x = G.fences; x; x = x->next) if (x->id == ids[i] && !x->done) return false;
    return true;
}

static void sync_release(FdObj *o) {
    SyncFile *s = o->data;
    pthread_mutex_lock(&G.flock);
    for (SyncFile **pp = &G.syncs; *pp; pp = &(*pp)->next) if (*pp == s) { *pp = s->next; break; }
    pthread_mutex_unlock(&G.flock);
    free(s->fences);
    free(s);
}

// sync_file ioctls: SYNC_IOC_MERGE (0xc0303e03), SYNC_IOC_FILE_INFO (0xc0203e04)
static int64_t sync_ioctl(FdObj *o, GuestCtx *c, uint64_t req, uint64_t arg);
static const FdOps sync_ops = { "sync_file", NULL, NULL, sync_ioctl, NULL, sync_release };

static int sync_create(Proc *p, const uint64_t *ids, int n, const char *name) {
    SyncFile *s = calloc(1, sizeof *s);
    s->fences = calloc((size_t)n + 1, sizeof(uint64_t));
    memcpy(s->fences, ids, (size_t)n * sizeof(uint64_t));
    s->n = n;
    FdObj *o = obj_new(&sync_ops, s, true);
    s->o = o;
    pthread_mutex_lock(&G.flock);
    s->next = G.syncs;
    G.syncs = s;
    if (fences_done_locked(ids, n)) { pthread_mutex_lock(&o->lock); obj_set_ready(o, true); pthread_mutex_unlock(&o->lock); }
    pthread_mutex_unlock(&G.flock);
    char path[64];
    snprintf(path, sizeof path, "sync_file:[%s]", name);
    fd_set_path(o->rd, path);
    return fd_install_obj(p, o->rd, o, true);
}

static SyncFile *sync_of(Proc *p, int gfd) {
    int h = fd_host(p, gfd);
    FdObj *o = h >= 0 ? fd_obj(h) : NULL;
    return o && o->ops == &sync_ops ? o->data : NULL;
}

static int64_t sync_ioctl(FdObj *o, GuestCtx *c, uint64_t req, uint64_t arg) {
    SyncFile *s = o->data;
    if (_IOC_NR(req) == 3 && _IOC_TYPE(req) == '>') {     // SYNC_IOC_MERGE: name[32], fd2, fence(out), flags, pad
        uint8_t *a = gptr(arg);
        int32_t fd2;
        memcpy(&fd2, a + 32, 4);
        SyncFile *other = sync_of(c->proc, fd2);
        if (!other) return -EINVAL;
        uint64_t ids[64];
        int n = 0;
        for (int i = 0; i < s->n && n < 64; i++) ids[n++] = s->fences[i];
        for (int i = 0; i < other->n && n < 64; i++) ids[n++] = other->fences[i];
        int32_t nfd = sync_create(c->proc, ids, n, (const char *)a);
        if (nfd < 0) return nfd;
        memcpy(a + 36, &nfd, 4);
        return 0;
    }
    if (_IOC_NR(req) == 4 && _IOC_TYPE(req) == '>') {     // SYNC_IOC_FILE_INFO: name[32], status, flags, num_fences, pad, sync_fence_info (56 bytes)
        uint8_t *a = gptr(arg);
        pthread_mutex_lock(&G.flock);
        int32_t status = fences_done_locked(s->fences, s->n) ? 1 : 0;
        pthread_mutex_unlock(&G.flock);
        snprintf((char *)a, 32, "onirun");
        memcpy(a + 32, &status, 4);
        uint32_t num = 0;
        memcpy(a + 40, &num, 4);                          // report no per-fence details
        return 0;
    }
    return -ENOTTY;
}

// ---- resources ----
static GpuRes *res_new(uint64_t size) {
    GpuRes *r = calloc(1, sizeof *r);
    r->res_id = G.next_res++;
    r->refs = 1;
    r->size = size;
    r->next = G.resources;
    G.resources = r;
    return r;
}

static void res_put_locked(GpuRes *r) {
    if (--r->refs > 0) return;
    if (r->map) rutabaga_resource_unmap(G.r, r->res_id);
    rutabaga_resource_unref(G.r, r->res_id);
    if (r->backing) munmap(r->backing, PG_UP(r->size));
    for (GpuRes **pp = &G.resources; *pp; pp = &(*pp)->next) if (*pp == r) { *pp = r->next; break; }
    free(r);
}

static GpuRes *res_find_locked(uint32_t id) {
    for (GpuRes *r = G.resources; r; r = r->next) if (r->res_id == id) return r;
    return NULL;
}

// Host pointer for CPU access to a resource (maps host3d blobs on first use).
static void *res_memory_locked(GpuRes *r, uint64_t *size) {
    if (r->backing) { *size = PG_UP(r->size); return r->backing; }
    if (!r->map) {
        struct rutabaga_mapping m = {0};
        if (rutabaga_resource_map(G.r, r->res_id, &m) || !m.ptr) return NULL;
        r->map = m.ptr;
        r->map_size = m.size;
    }
    *size = r->map_size;
    return r->map;
}

static uint32_t handle_add(GpuFile *f, GpuRes *r) {
    for (int i = 1; i < f->nhandles; i++) if (!f->handles[i]) { f->handles[i] = r; return (uint32_t)i; }
    if (!f->nhandles) f->nhandles = 1;
    f->handles = realloc(f->handles, (size_t)(f->nhandles + 1) * sizeof *f->handles);
    f->handles[f->nhandles] = r;
    return (uint32_t)f->nhandles++;
}
static GpuRes *handle_get(GpuFile *f, uint32_t h) { return h && (int)h < f->nhandles ? f->handles[h] : NULL; }

// ---- dma-buf fds ----
static int64_t dmabuf_mmap(FdObj *o, GuestCtx *c, uint64_t addr, uint64_t len, int prot, int lflags, uint64_t off);
static void dmabuf_release(FdObj *o) { LOCK(); res_put_locked(o->data); UNLOCK(); }
static int64_t dmabuf_ioctl(FdObj *o, GuestCtx *c, uint64_t req, uint64_t arg) { (void)o; (void)c; (void)arg; return ((req >> 8) & 0xff) == 'b' ? 0 : -ENOTTY; }   // DMA_BUF_IOCTL_SYNC and friends
static const FdOps dmabuf_ops = { "dmabuf", NULL, NULL, dmabuf_ioctl, NULL, dmabuf_release, dmabuf_mmap };

static int dmabuf_export(Proc *p, GpuRes *r) {
    FdObj *o = obj_new(&dmabuf_ops, r, true);
    r->refs++;
    o->size = r->size;
    char path[64];
    snprintf(path, sizeof path, "dmabuf:%u", r->res_id);
    fd_set_path(o->rd, path);
    return fd_install_obj(p, o->rd, o, true);
}

static int64_t map_res(GuestCtx *c, GpuRes *r, uint64_t addr, uint64_t len, int prot, uint64_t off) {
    uint64_t size;
    void *mem = res_memory_locked(r, &size);
    if (!mem) return -EINVAL;
    if (off >= size) return -EINVAL;
    if (len > size - off) len = size - off;
    char name[48];
    snprintf(name, sizeof name, "/dev/dri/res%u", r->res_id);
    return mm_map_host(c->proc->mm, addr, len, prot, (uint8_t *)mem + off, name);
}

static int64_t dmabuf_mmap(FdObj *o, GuestCtx *c, uint64_t addr, uint64_t len, int prot, int lflags, uint64_t off) {
    (void)lflags;
    LOCK();
    int64_t r = map_res(c, o->data, addr, len, prot, off);
    UNLOCK();
    return r;
}

// ---- the device ----
static bool gpu_init_locked(void) {
    if (G.r) return true;
    const char *w = getenv("RUTABAGA_DISPLAY_WIDTH"), *h = getenv("RUTABAGA_DISPLAY_HEIGHT");
    G.width = w ? (uint32_t)atoi(w) : 1080;
    G.height = h ? (uint32_t)atoi(h) : 2400;
    G.frames_dir = getenv("ONIRUN_FRAMES_DIR");
    struct rutabaga_builder b = {
        .capset_mask = (1u << RUTABAGA_CAPSET_GFXSTREAM_VULKAN) | (1u << RUTABAGA_CAPSET_GFXSTREAM_COMPOSER),
        .wsi = RUTABAGA_WSI_SURFACELESS,
        .fence_cb = fence_cb,
        .debug_cb = debug_cb,
        .renderer_features = getenv("RUTABAGA_RENDERER_FEATURES"),
    };
    int32_t rc = rutabaga_init(&b, &G.r);
    if (rc) { fprintf(stderr, "onirun: rutabaga_init failed (%d)\n", rc); return false; }
    uint32_t n = 0;
    rutabaga_get_num_capsets(G.r, &n);
    LOGV("onirun: gfxstream up: %u capsets, display %ux%u\n", n, G.width, G.height);
    return true;
}

static int ctx_ensure_locked(GpuFile *f, uint32_t capset) {
    if (f->ctx_id) return 0;
    uint32_t id = G.next_ctx++;
    int32_t rc = rutabaga_context_create(G.r, id, capset ? capset : RUTABAGA_CAPSET_GFXSTREAM_VULKAN, "onirun", 6);
    if (rc) { LOGV("onirun: virtgpu: context_create(capset %u) failed: %d\n", capset, rc); return -ENODEV; }
    f->ctx_id = id;
    f->capset = capset;
    return 0;
}

static void gpu_release(FdObj *o) {
    GpuFile *f = o->data;
    LOCK();
    for (int i = 1; i < f->nhandles; i++) if (f->handles[i]) res_put_locked(f->handles[i]);
    for (int i = 0; i < 64; i++) if (f->fbs[i].res) res_put_locked(f->fbs[i].res);
    for (int i = 0; i < 16; i++) free(f->blobs[i].data);
    if (f->ctx_id) rutabaga_context_destroy(G.r, f->ctx_id);
    UNLOCK();
    free(f->handles);
    free(f);
}

static void push_event(GpuFile *f, const void *ev, int len) {
    if (f->ev_len + len > (int)sizeof f->events) return;
    memcpy(f->events + f->ev_len, ev, (size_t)len);
    f->ev_len += len;
    pthread_mutex_lock(&f->o->lock);
    obj_set_ready(f->o, true);
    pthread_mutex_unlock(&f->o->lock);
}

static int64_t gpu_read(FdObj *o, GuestCtx *c, void *buf, size_t n, bool nonblock) {
    (void)c;
    GpuFile *f = o->data;
    for (;;) {
        LOCK();
        if (f->ev_len) {
            int k = (int)n < f->ev_len ? (int)n : f->ev_len;
            memcpy(buf, f->events, (size_t)k);
            memmove(f->events, f->events + k, (size_t)(f->ev_len - k));
            f->ev_len -= k;
            pthread_mutex_lock(&o->lock);
            obj_set_ready(o, f->ev_len > 0);
            pthread_mutex_unlock(&o->lock);
            UNLOCK();
            return k;
        }
        UNLOCK();
        int r = obj_wait(o->rd, POLLIN, nonblock);
        if (r) return r;
    }
}

// A frame reached the "screen": read it back for whoever shows it.
static void present_locked(GpuRes *r, uint32_t w, uint32_t h) {
    if (!G.frame) G.frame = calloc((size_t)G.width * G.height * 4, 1);
    if (w > G.width) w = G.width;
    if (h > G.height) h = G.height;
    struct rutabaga_transfer t = { .w = w, .h = h, .d = 1, .stride = G.width * 4 };
    struct iovec iov = { G.frame, (size_t)G.width * G.height * 4 };
    int32_t rc = rutabaga_resource_transfer_read(G.r, 0, r->res_id, &t, &iov);
    G.frames++;
    if (rc) LOGV("onirun: present: transfer_read of res %u failed: %d\n", r->res_id, rc);
    if (G.frames == 1 || G.frames % 60 == 0) LOGV("onirun: frame %llu (%ux%u, res %u)\n", (unsigned long long)G.frames, w, h, r->res_id);
    if (G.frames_dir && (G.frames <= 3 || G.frames % 120 == 0)) {
        char path[512];
        snprintf(path, sizeof path, "%s/frame-%06llu.ppm", G.frames_dir, (unsigned long long)G.frames);
        FILE *fp = fopen(path, "wb");
        if (fp) {
            fprintf(fp, "P6\n%u %u\n255\n", w, h);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) fwrite(G.frame + ((size_t)y * G.width + x) * 4, 1, 3, fp);
            fclose(fp);
        }
    }
}

// ---- KMS object model ----
enum { ID_CONNECTOR = 32, ID_ENCODER = 33, ID_CRTC = 34, ID_PLANE = 35, ID_MODE_BLOB = 36,
       P_CRTC_ID = 40, P_DPMS, P_ACTIVE, P_MODE_ID, P_OUT_FENCE_PTR, P_TYPE, P_FB_ID, P_PLANE_CRTC_ID,
       P_CRTC_X, P_CRTC_Y, P_CRTC_W, P_CRTC_H, P_SRC_X, P_SRC_Y, P_SRC_W, P_SRC_H, P_IN_FENCE_FD, P_IN_FORMATS, P_EDID, P_LINK_STATUS };
typedef struct { uint32_t id; const char *name; uint32_t flags; } PropDef;
static const PropDef connector_props[] = { { P_CRTC_ID, "CRTC_ID", DRM_MODE_PROP_OBJECT }, { P_DPMS, "DPMS", DRM_MODE_PROP_ENUM }, { P_LINK_STATUS, "link-status", DRM_MODE_PROP_ENUM }, { P_EDID, "EDID", DRM_MODE_PROP_BLOB } };
static const PropDef crtc_props[] = { { P_ACTIVE, "ACTIVE", DRM_MODE_PROP_RANGE }, { P_MODE_ID, "MODE_ID", DRM_MODE_PROP_BLOB }, { P_OUT_FENCE_PTR, "OUT_FENCE_PTR", DRM_MODE_PROP_RANGE } };
static const PropDef plane_props[] = { { P_TYPE, "type", DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE }, { P_FB_ID, "FB_ID", DRM_MODE_PROP_OBJECT }, { P_PLANE_CRTC_ID, "CRTC_ID", DRM_MODE_PROP_OBJECT },
    { P_CRTC_X, "CRTC_X", DRM_MODE_PROP_SIGNED_RANGE }, { P_CRTC_Y, "CRTC_Y", DRM_MODE_PROP_SIGNED_RANGE }, { P_CRTC_W, "CRTC_W", DRM_MODE_PROP_RANGE }, { P_CRTC_H, "CRTC_H", DRM_MODE_PROP_RANGE },
    { P_SRC_X, "SRC_X", DRM_MODE_PROP_RANGE }, { P_SRC_Y, "SRC_Y", DRM_MODE_PROP_RANGE }, { P_SRC_W, "SRC_W", DRM_MODE_PROP_RANGE }, { P_SRC_H, "SRC_H", DRM_MODE_PROP_RANGE },
    { P_IN_FENCE_FD, "IN_FENCE_FD", DRM_MODE_PROP_SIGNED_RANGE }, { P_IN_FORMATS, "IN_FORMATS", DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE } };

static void fill_mode(struct drm_mode_modeinfo *m) {
    memset(m, 0, sizeof *m);
    m->clock = (G.width + 100) * (G.height + 50) * 60 / 1000;
    m->hdisplay = m->hsync_start = m->hsync_end = m->htotal = (uint16_t)G.width;
    m->hsync_start += 20; m->hsync_end += 40; m->htotal += 100;
    m->vdisplay = m->vsync_start = m->vsync_end = m->vtotal = (uint16_t)G.height;
    m->vsync_start += 10; m->vsync_end += 20; m->vtotal += 50;
    m->vrefresh = 60;
    m->type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
    snprintf(m->name, sizeof m->name, "%ux%u", G.width, G.height);
}

static uint64_t prop_value(GpuFile *f, uint32_t prop) {
    switch (prop) {
    case P_CRTC_ID: case P_PLANE_CRTC_ID: return ID_CRTC;
    case P_ACTIVE: return 1;
    case P_MODE_ID: return ID_MODE_BLOB;
    case P_TYPE: return DRM_PLANE_TYPE_PRIMARY;
    case P_FB_ID: return f->cur_fb;
    case P_CRTC_W: case P_SRC_W: return prop == P_SRC_W ? (uint64_t)G.width << 16 : G.width;
    case P_CRTC_H: case P_SRC_H: return prop == P_SRC_H ? (uint64_t)G.height << 16 : G.height;
    case P_IN_FENCE_FD: return (uint64_t)(int64_t)-1;
    case P_IN_FORMATS: return 37;                        // blob id
    default: return 0;
    }
}

static int64_t put_array(uint64_t ptr, uint32_t *count_io, const void *items, uint32_t n, size_t sz) {
    if (ptr && *count_io >= n) memcpy(gptr(ptr), items, n * sz);
    *count_io = n;
    return 0;
}

static int64_t kms_ioctl(GuestCtx *c, GpuFile *f, uint64_t req, void *a) {
    switch (req) {
    case DRM_IOCTL_MODE_GETRESOURCES: {
        struct drm_mode_card_res *r = a;
        uint32_t fbs[64], nfb = 0, crtc = ID_CRTC, conn = ID_CONNECTOR, enc = ID_ENCODER;
        for (int i = 0; i < 64; i++) if (f->fbs[i].res) fbs[nfb++] = f->fbs[i].id;
        put_array(r->fb_id_ptr, &r->count_fbs, fbs, nfb, 4);
        put_array(r->crtc_id_ptr, &r->count_crtcs, &crtc, 1, 4);
        put_array(r->connector_id_ptr, &r->count_connectors, &conn, 1, 4);
        put_array(r->encoder_id_ptr, &r->count_encoders, &enc, 1, 4);
        r->min_width = r->min_height = 16;
        r->max_width = r->max_height = 8192;
        return 0;
    }
    case DRM_IOCTL_MODE_GETCONNECTOR: {
        struct drm_mode_get_connector *r = a;
        if (r->connector_id != ID_CONNECTOR) return -ENOENT;
        struct drm_mode_modeinfo m;
        fill_mode(&m);
        uint32_t enc = ID_ENCODER;
        put_array(r->modes_ptr, &r->count_modes, &m, 1, sizeof m);
        put_array(r->encoders_ptr, &r->count_encoders, &enc, 1, 4);
        uint32_t props[8], nprops = 0;
        uint64_t vals[8];
        for (unsigned i = 0; i < sizeof connector_props / sizeof *connector_props; i++) { props[nprops] = connector_props[i].id; vals[nprops++] = prop_value(f, connector_props[i].id); }
        if (r->props_ptr && r->count_props >= nprops) { memcpy(gptr(r->props_ptr), props, nprops * 4); memcpy(gptr(r->prop_values_ptr), vals, nprops * 8); }
        r->count_props = nprops;
        r->encoder_id = ID_ENCODER;
        r->connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
        r->connector_type_id = 1;
        r->connection = 1;                                // connected
        r->mm_width = G.width * 254 / 4000;              // ~400 dpi
        r->mm_height = G.height * 254 / 4000;
        r->subpixel = 0;
        return 0;
    }
    case DRM_IOCTL_MODE_GETENCODER: {
        struct drm_mode_get_encoder *r = a;
        if (r->encoder_id != ID_ENCODER) return -ENOENT;
        r->encoder_type = DRM_MODE_ENCODER_VIRTUAL;
        r->crtc_id = ID_CRTC;
        r->possible_crtcs = 1;
        r->possible_clones = 0;
        return 0;
    }
    case DRM_IOCTL_MODE_GETCRTC: {
        struct drm_mode_crtc *r = a;
        if (r->crtc_id != ID_CRTC) return -ENOENT;
        r->fb_id = f->cur_fb;
        r->x = r->y = 0;
        r->mode_valid = 1;
        fill_mode(&r->mode);
        r->gamma_size = 0;
        return 0;
    }
    case DRM_IOCTL_MODE_GETPLANERESOURCES: {
        struct drm_mode_get_plane_res *r = a;
        uint32_t plane = ID_PLANE;
        put_array(r->plane_id_ptr, &r->count_planes, &plane, 1, 4);
        return 0;
    }
    case DRM_IOCTL_MODE_GETPLANE: {
        struct drm_mode_get_plane *r = a;
        if (r->plane_id != ID_PLANE) return -ENOENT;
        r->crtc_id = ID_CRTC;
        r->fb_id = f->cur_fb;
        r->possible_crtcs = 1;
        r->gamma_size = 0;
        uint32_t fmts[] = { DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_RGB565 };
        put_array(r->format_type_ptr, &r->count_format_types, fmts, 5, 4);
        return 0;
    }
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES: {
        struct drm_mode_obj_get_properties *r = a;
        const PropDef *defs;
        uint32_t n;
        if (r->obj_id == ID_CONNECTOR) { defs = connector_props; n = sizeof connector_props / sizeof *defs; }
        else if (r->obj_id == ID_CRTC) { defs = crtc_props; n = sizeof crtc_props / sizeof *defs; }
        else if (r->obj_id == ID_PLANE) { defs = plane_props; n = sizeof plane_props / sizeof *defs; }
        else return -ENOENT;
        uint32_t ids[24];
        uint64_t vals[24];
        for (uint32_t i = 0; i < n; i++) { ids[i] = defs[i].id; vals[i] = prop_value(f, defs[i].id); }
        if (r->props_ptr && r->count_props >= n) { memcpy(gptr(r->props_ptr), ids, n * 4); memcpy(gptr(r->prop_values_ptr), vals, n * 8); }
        r->count_props = n;
        return 0;
    }
    case DRM_IOCTL_MODE_GETPROPERTY: {
        struct drm_mode_get_property *r = a;
        const PropDef *d = NULL;
        for (unsigned i = 0; i < sizeof connector_props / sizeof *connector_props && !d; i++) if (connector_props[i].id == r->prop_id) d = &connector_props[i];
        for (unsigned i = 0; i < sizeof crtc_props / sizeof *crtc_props && !d; i++) if (crtc_props[i].id == r->prop_id) d = &crtc_props[i];
        for (unsigned i = 0; i < sizeof plane_props / sizeof *plane_props && !d; i++) if (plane_props[i].id == r->prop_id) d = &plane_props[i];
        if (!d) return -ENOENT;
        snprintf(r->name, sizeof r->name, "%s", d->name);
        r->flags = d->flags;
        if (d->id == P_TYPE) {
            struct drm_mode_property_enum e[3] = { { DRM_PLANE_TYPE_OVERLAY, "Overlay" }, { DRM_PLANE_TYPE_PRIMARY, "Primary" }, { DRM_PLANE_TYPE_CURSOR, "Cursor" } };
            put_array(r->enum_blob_ptr, &r->count_enum_blobs, e, 3, sizeof e[0]);
            r->count_values = 0;
        } else if (d->id == P_DPMS) {
            struct drm_mode_property_enum e[2] = { { 0, "On" }, { 3, "Off" } };
            put_array(r->enum_blob_ptr, &r->count_enum_blobs, e, 2, sizeof e[0]);
            r->count_values = 0;
        } else if (d->id == P_LINK_STATUS) {
            struct drm_mode_property_enum e[2] = { { 0, "Good" }, { 1, "Bad" } };
            put_array(r->enum_blob_ptr, &r->count_enum_blobs, e, 2, sizeof e[0]);
            r->count_values = 0;
        } else if (d->flags & (DRM_MODE_PROP_RANGE | DRM_MODE_PROP_SIGNED_RANGE)) {
            uint64_t v[2] = { 0, UINT32_MAX };
            if (d->flags & DRM_MODE_PROP_SIGNED_RANGE) { v[0] = (uint64_t)INT32_MIN; v[1] = INT32_MAX; }
            if (d->id == P_ACTIVE) v[1] = 1;
            if (d->id == P_OUT_FENCE_PTR) v[1] = UINT64_MAX;
            put_array(r->values_ptr, &r->count_values, v, 2, 8);
            r->count_enum_blobs = 0;
        } else {
            r->count_values = 0;
            r->count_enum_blobs = 0;
        }
        return 0;
    }
    case DRM_IOCTL_MODE_GETPROPBLOB: {
        struct drm_mode_get_blob *r = a;
        if (r->blob_id == ID_MODE_BLOB) {
            struct drm_mode_modeinfo m;
            fill_mode(&m);
            if (r->data && r->length >= sizeof m) memcpy(gptr(r->data), &m, sizeof m);
            r->length = sizeof m;
            return 0;
        }
        if (r->blob_id == 37) {                           // IN_FORMATS: a drm_format_modifier_blob with one linear modifier
            uint32_t fmts[] = { DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_RGB565 };
            struct drm_format_modifier mods[1] = { { .formats = 0x1f, .offset = 0, .pad = 0, .modifier = DRM_FORMAT_MOD_LINEAR } };
            struct drm_format_modifier_blob hdr = { .version = FORMAT_BLOB_CURRENT, .count_formats = 5, .formats_offset = sizeof hdr,
                                                    .count_modifiers = 1, .modifiers_offset = sizeof hdr + sizeof fmts };
            uint8_t blob[sizeof hdr + sizeof fmts + sizeof mods];
            memcpy(blob, &hdr, sizeof hdr); memcpy(blob + sizeof hdr, fmts, sizeof fmts); memcpy(blob + sizeof hdr + sizeof fmts, mods, sizeof mods);
            if (r->data && r->length >= sizeof blob) memcpy(gptr(r->data), blob, sizeof blob);
            r->length = sizeof blob;
            return 0;
        }
        for (int i = 0; i < 16; i++)
            if (f->blobs[i].id == r->blob_id && f->blobs[i].data) {
                if (r->data && r->length >= f->blobs[i].len) memcpy(gptr(r->data), f->blobs[i].data, f->blobs[i].len);
                r->length = f->blobs[i].len;
                return 0;
            }
        return -ENOENT;
    }
    case DRM_IOCTL_MODE_CREATEPROPBLOB: {
        struct drm_mode_create_blob *r = a;
        for (int i = 0; i < 16; i++)
            if (!f->blobs[i].data) {
                f->blobs[i].data = malloc(r->length);
                memcpy(f->blobs[i].data, gptr(r->data), r->length);
                f->blobs[i].len = r->length;
                f->blobs[i].id = 100 + (uint32_t)i;
                r->blob_id = f->blobs[i].id;
                return 0;
            }
        return -ENOSPC;
    }
    case DRM_IOCTL_MODE_DESTROYPROPBLOB: {
        struct drm_mode_destroy_blob *r = a;
        for (int i = 0; i < 16; i++) if (f->blobs[i].id == r->blob_id) { free(f->blobs[i].data); f->blobs[i].data = NULL; f->blobs[i].id = 0; }
        return 0;
    }
    case DRM_IOCTL_MODE_ADDFB2: {
        struct drm_mode_fb_cmd2 *r = a;
        GpuRes *res = handle_get(f, r->handles[0]);
        if (!res) return -ENOENT;
        for (int i = 0; i < 64; i++)
            if (!f->fbs[i].res) {
                res->refs++;
                f->fbs[i].res = res;
                f->fbs[i].id = 200 + (uint32_t)i;
                f->fbs[i].w = r->width;
                f->fbs[i].h = r->height;
                r->fb_id = f->fbs[i].id;
                return 0;
            }
        return -ENOSPC;
    }
    case DRM_IOCTL_MODE_ADDFB: {
        struct drm_mode_fb_cmd *r = a;
        GpuRes *res = handle_get(f, r->handle);
        if (!res) return -ENOENT;
        for (int i = 0; i < 64; i++)
            if (!f->fbs[i].res) { res->refs++; f->fbs[i].res = res; f->fbs[i].id = 200 + (uint32_t)i; f->fbs[i].w = r->width; f->fbs[i].h = r->height; r->fb_id = f->fbs[i].id; return 0; }
        return -ENOSPC;
    }
    case DRM_IOCTL_MODE_RMFB: {
        uint32_t id = *(uint32_t *)a;
        for (int i = 0; i < 64; i++) if (f->fbs[i].res && f->fbs[i].id == id) { res_put_locked(f->fbs[i].res); f->fbs[i].res = NULL; return 0; }
        return -ENOENT;
    }
    case DRM_IOCTL_MODE_ATOMIC: {
        struct drm_mode_atomic *r = a;
        if (r->flags & DRM_MODE_ATOMIC_TEST_ONLY) return 0;
        const uint32_t *objs = gptr(r->objs_ptr), *counts = gptr(r->count_props_ptr), *props = gptr(r->props_ptr);
        const uint64_t *vals = gptr(r->prop_values_ptr);
        uint32_t k = 0, fb = f->cur_fb;
        uint64_t out_fence_ptr = 0;
        int in_fence = -1;
        for (uint32_t i = 0; i < r->count_objs; i++)
            for (uint32_t j = 0; j < counts[i]; j++, k++) {
                if (props[k] == P_FB_ID) fb = (uint32_t)vals[k];
                if (props[k] == P_OUT_FENCE_PTR) out_fence_ptr = vals[k];
                if (props[k] == P_IN_FENCE_FD) in_fence = (int)(int32_t)vals[k];
            }
        (void)objs; (void)in_fence;                       // the in-fence: GPU work is complete when we read the frame back
        f->cur_fb = fb;
        for (int i = 0; i < 64; i++) if (f->fbs[i].res && f->fbs[i].id == fb) present_locked(f->fbs[i].res, f->fbs[i].w, f->fbs[i].h);
        if (out_fence_ptr) {
            uint64_t none = 0;
            int32_t sfd = sync_create(c->proc, &none, 0, "kms");  // already signaled
            memcpy(gptr(out_fence_ptr), &sfd, 4);
        }
        if (r->flags & DRM_MODE_PAGE_FLIP_EVENT) {
            struct drm_event_vblank ev = { .base = { DRM_EVENT_FLIP_COMPLETE, sizeof ev }, .user_data = r->user_data, .crtc_id = ID_CRTC };
            struct timeval tv;
            gettimeofday(&tv, NULL);
            ev.tv_sec = (uint32_t)tv.tv_sec;
            ev.tv_usec = (uint32_t)tv.tv_usec;
            ev.sequence = (uint32_t)G.frames;
            push_event(f, &ev, sizeof ev);
        }
        return 0;
    }
    case DRM_IOCTL_MODE_SETCRTC: {
        struct drm_mode_crtc *r = a;
        f->cur_fb = r->fb_id;
        for (int i = 0; i < 64; i++) if (f->fbs[i].res && f->fbs[i].id == r->fb_id) present_locked(f->fbs[i].res, f->fbs[i].w, f->fbs[i].h);
        return 0;
    }
    case DRM_IOCTL_MODE_PAGE_FLIP: {
        struct drm_mode_crtc_page_flip *r = a;
        f->cur_fb = r->fb_id;
        for (int i = 0; i < 64; i++) if (f->fbs[i].res && f->fbs[i].id == r->fb_id) present_locked(f->fbs[i].res, f->fbs[i].w, f->fbs[i].h);
        if (r->flags & DRM_MODE_PAGE_FLIP_EVENT) {
            struct drm_event_vblank ev = { .base = { DRM_EVENT_FLIP_COMPLETE, sizeof ev }, .user_data = r->user_data, .crtc_id = ID_CRTC };
            ev.sequence = (uint32_t)G.frames;
            push_event(f, &ev, sizeof ev);
        }
        return 0;
    }
    case DRM_IOCTL_MODE_CURSOR: case DRM_IOCTL_MODE_CURSOR2: case DRM_IOCTL_MODE_DIRTYFB: return 0;
    case DRM_IOCTL_MODE_GETGAMMA: case DRM_IOCTL_MODE_SETGAMMA: return -ENOSYS;
    default:
        return -ENOTTY;
    }
}

// ---- render node ioctls ----
static int64_t gpu_ioctl(FdObj *o, GuestCtx *c, uint64_t req, uint64_t arg) {
    GpuFile *f = o->data;
    void *a = gptr(arg);
    if (((req >> 8) & 0xff) != DRM_IOCTL_BASE) return -ENOTTY;
    // Newer guest headers grow some structs (CREATE_BLOB is 56 bytes there); the leading fields are the same.
    if (_IOC_NR(req) == DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_CREATE_BLOB) req = DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB;
    if (_IOC_NR(req) == DRM_COMMAND_BASE + DRM_VIRTGPU_EXECBUFFER) req = DRM_IOCTL_VIRTGPU_EXECBUFFER;
    LOCK();
    int64_t r = 0;
    switch (req) {
    case DRM_IOCTL_VERSION: {
        struct drm_version *v = a;
        v->version_major = 0; v->version_minor = 1; v->version_patchlevel = 0;
        const char *name = "virtio_gpu", *date = "0", *desc = "virtio GPU";
        if (v->name && v->name_len >= strlen(name)) memcpy(gptr((uint64_t)(uintptr_t)v->name), name, strlen(name));
        if (v->date && v->date_len >= strlen(date)) memcpy(gptr((uint64_t)(uintptr_t)v->date), date, strlen(date));
        if (v->desc && v->desc_len >= strlen(desc)) memcpy(gptr((uint64_t)(uintptr_t)v->desc), desc, strlen(desc));
        v->name_len = strlen(name); v->date_len = strlen(date); v->desc_len = strlen(desc);
        break;
    }
    case DRM_IOCTL_GET_CAP: {
        struct drm_get_cap *g = a;
        switch (g->capability) {
        case DRM_CAP_DUMB_BUFFER: g->value = 0; break;
        case DRM_CAP_PRIME: g->value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; break;
        case DRM_CAP_ADDFB2_MODIFIERS: g->value = 1; break;
        case DRM_CAP_CURSOR_WIDTH: case DRM_CAP_CURSOR_HEIGHT: g->value = 64; break;
        case DRM_CAP_TIMESTAMP_MONOTONIC: g->value = 1; break;
        case DRM_CAP_CRTC_IN_VBLANK_EVENT: g->value = 1; break;
        case DRM_CAP_SYNCOBJ: case DRM_CAP_SYNCOBJ_TIMELINE: g->value = 0; break;
        default: g->value = 0;
        }
        break;
    }
    case DRM_IOCTL_SET_CLIENT_CAP: case DRM_IOCTL_SET_MASTER: case DRM_IOCTL_DROP_MASTER: break;
    case DRM_IOCTL_GET_MAGIC: ((struct drm_auth *)a)->magic = 1; break;
    case DRM_IOCTL_AUTH_MAGIC: break;
    case DRM_IOCTL_GEM_CLOSE: {
        struct drm_gem_close *g = a;
        GpuRes *res = handle_get(f, g->handle);
        if (!res) { r = -EINVAL; break; }
        f->handles[g->handle] = NULL;
        if (f->ctx_id) rutabaga_context_detach_resource(G.r, f->ctx_id, res->res_id);
        res_put_locked(res);
        break;
    }
    case DRM_IOCTL_PRIME_HANDLE_TO_FD: {
        struct drm_prime_handle *p = a;
        GpuRes *res = handle_get(f, p->handle);
        if (!res) { r = -ENOENT; break; }
        int fd = dmabuf_export(c->proc, res);
        if (fd < 0) { r = fd; break; }
        p->fd = fd;
        break;
    }
    case DRM_IOCTL_PRIME_FD_TO_HANDLE: {
        struct drm_prime_handle *p = a;
        int h = fd_host(c->proc, p->fd);
        FdObj *d = h >= 0 ? fd_obj(h) : NULL;
        if (!d || d->ops != &dmabuf_ops) { r = -EINVAL; break; }
        GpuRes *res = d->data;
        uint32_t existing = 0;
        for (int i = 1; i < f->nhandles; i++) if (f->handles[i] == res) existing = (uint32_t)i;
        if (!existing) {
            res->refs++;
            existing = handle_add(f, res);
            if (f->ctx_id || !ctx_ensure_locked(f, 0)) rutabaga_context_attach_resource(G.r, f->ctx_id, res->res_id);
        }
        p->handle = existing;
        break;
    }
    case DRM_IOCTL_VIRTGPU_GETPARAM: {
        struct drm_virtgpu_getparam *g = a;
        uint64_t v = 0;
        switch (g->param) {
        case VIRTGPU_PARAM_3D_FEATURES: case VIRTGPU_PARAM_CAPSET_QUERY_FIX: case VIRTGPU_PARAM_RESOURCE_BLOB:
        case VIRTGPU_PARAM_HOST_VISIBLE: case VIRTGPU_PARAM_CONTEXT_INIT: v = 1; break;
        case VIRTGPU_PARAM_CROSS_DEVICE: v = 0; break;
        case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs: v = (1u << RUTABAGA_CAPSET_GFXSTREAM_VULKAN) | (1u << RUTABAGA_CAPSET_GFXSTREAM_COMPOSER); break;
        default: v = 0; r = -EINVAL;
        }
        if (!r) *(uint64_t *)gptr(g->value) = v;
        break;
    }
    case DRM_IOCTL_VIRTGPU_GET_CAPS: {
        struct drm_virtgpu_get_caps *g = a;
        uint32_t id = g->cap_set_id ? g->cap_set_id : RUTABAGA_CAPSET_GFXSTREAM_VULKAN;
        int32_t rc = rutabaga_get_capset(G.r, id, g->cap_set_ver, gptr(g->addr), g->size);
        if (rc) r = -EINVAL;
        break;
    }
    case DRM_IOCTL_VIRTGPU_CONTEXT_INIT: {
        struct drm_virtgpu_context_init *ci = a;
        uint32_t capset = 0;
        const struct drm_virtgpu_context_set_param *ps = gptr(ci->ctx_set_params);
        for (uint32_t i = 0; i < ci->num_params; i++) if (ps[i].param == VIRTGPU_CONTEXT_PARAM_CAPSET_ID) capset = (uint32_t)ps[i].value;
        if (f->ctx_id) { r = -EEXIST; break; }
        r = ctx_ensure_locked(f, capset);
        break;
    }
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE: {
        struct drm_virtgpu_resource_create *rc = a;
        if (!f->ctx_id && (r = ctx_ensure_locked(f, 0))) break;
        uint64_t size = rc->size ? rc->size : (uint64_t)rc->stride * rc->height;
        if (!size) size = (uint64_t)rc->width * rc->height * 4;
        GpuRes *res = res_new(size);
        res->width = rc->width; res->height = rc->height; res->format = rc->format; res->stride = rc->stride;
        struct rutabaga_create_3d c3 = { .target = rc->target, .format = rc->format, .bind = rc->bind, .width = rc->width, .height = rc->height,
                                         .depth = rc->depth, .array_size = rc->array_size, .last_level = rc->last_level, .nr_samples = rc->nr_samples, .flags = rc->flags };
        int32_t ret = rutabaga_resource_create_3d(G.r, res->res_id, &c3);
        if (ret) { res->refs = 0; free(res); G.resources = G.resources->next; r = -EINVAL; break; }
        res->backing = mmap(NULL, PG_UP(size), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        struct iovec iov = { res->backing, PG_UP(size) };
        struct rutabaga_iovecs vecs = { &iov, 1 };
        rutabaga_resource_attach_backing(G.r, res->res_id, &vecs);
        rutabaga_context_attach_resource(G.r, f->ctx_id, res->res_id);
        rc->bo_handle = handle_add(f, res);
        rc->res_handle = res->res_id;
        rc->size = (uint32_t)size;
        break;
    }
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB: {
        struct drm_virtgpu_resource_create_blob *rb = a;
        if (!f->ctx_id && (r = ctx_ensure_locked(f, 0))) break;
        if (rb->cmd_size) {                                  // the creation command goes first, as the kernel does
            struct rutabaga_command cmd = { .ctx_id = f->ctx_id, .cmd_size = rb->cmd_size, .cmd = gptr(rb->cmd) };
            int32_t ret = rutabaga_submit_command(G.r, &cmd);
            if (ret) { LOGV("onirun: virtgpu: blob create command failed: %d\n", ret); r = -EINVAL; break; }
        }
        GpuRes *res = res_new(rb->size);
        res->blob_mem = rb->blob_mem;
        struct rutabaga_create_blob cb = { .blob_mem = rb->blob_mem, .blob_flags = rb->blob_flags, .blob_id = rb->blob_id, .size = rb->size };
        struct rutabaga_iovecs vecs = {0};
        struct iovec iov;
        if (rb->blob_mem == VIRTGPU_BLOB_MEM_GUEST || rb->blob_mem == VIRTGPU_BLOB_MEM_HOST3D_GUEST) {
            res->backing = mmap(NULL, PG_UP(rb->size), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            iov.iov_base = res->backing; iov.iov_len = PG_UP(rb->size);
            vecs.iovecs = &iov; vecs.num_iovecs = 1;
        }
        int32_t ret = rutabaga_resource_create_blob(G.r, f->ctx_id, res->res_id, &cb, vecs.num_iovecs ? &vecs : NULL, NULL);
        if (ret) {
            LOGV("onirun: virtgpu: create_blob(mem %u flags %u size %llu) failed: %d\n", rb->blob_mem, rb->blob_flags, (unsigned long long)rb->size, ret);
            if (res->backing) munmap(res->backing, PG_UP(rb->size));
            G.resources = res->next; free(res);
            r = -ENOMEM;
            break;
        }
        rutabaga_context_attach_resource(G.r, f->ctx_id, res->res_id);
        rb->bo_handle = handle_add(f, res);
        rb->res_handle = res->res_id;
        break;
    }
    case DRM_IOCTL_VIRTGPU_RESOURCE_INFO: {
        struct drm_virtgpu_resource_info *ri = a;
        GpuRes *res = handle_get(f, ri->bo_handle);
        if (!res) { r = -ENOENT; break; }
        ri->res_handle = res->res_id;
        ri->size = (uint32_t)res->size;
        ri->blob_mem = res->blob_mem;
        break;
    }
    case DRM_IOCTL_VIRTGPU_MAP: {
        struct drm_virtgpu_map *m = a;
        GpuRes *res = handle_get(f, m->handle);
        if (!res) { r = -ENOENT; break; }
        m->offset = (uint64_t)m->handle << 20;
        break;
    }
    case DRM_IOCTL_VIRTGPU_EXECBUFFER: {
        struct drm_virtgpu_execbuffer *e = a;
        if (!f->ctx_id && (r = ctx_ensure_locked(f, 0))) break;
        if (e->flags & VIRTGPU_EXECBUF_FENCE_FD_IN) {
            SyncFile *s = sync_of(c->proc, e->fence_fd);
            if (s) {                                         // wait for it before submitting
                UNLOCK();
                struct pollfd pf = { fd_host(c->proc, e->fence_fd), POLLIN, 0 };
                poll(&pf, 1, 5000);
                LOCK();
            }
        }
        struct rutabaga_command cmd = { .ctx_id = f->ctx_id, .cmd_size = e->size, .cmd = gptr(e->command) };
        int32_t ret = e->size ? rutabaga_submit_command(G.r, &cmd) : 0;
        if (ret) { LOGV("onirun: virtgpu: submit_command(%u bytes) failed: %d\n", e->size, ret); r = -EINVAL; break; }
        if (e->flags & VIRTGPU_EXECBUF_FENCE_FD_OUT) {
            uint64_t id = G.next_fence++;
            GpuFence *fe = calloc(1, sizeof *fe);
            fe->id = id;
            pthread_mutex_lock(&G.flock);
            fe->next = G.fences; G.fences = fe;
            pthread_mutex_unlock(&G.flock);
            struct rutabaga_fence rf = { .flags = RUTABAGA_FLAG_FENCE | ((e->flags & VIRTGPU_EXECBUF_RING_IDX) ? RUTABAGA_FLAG_INFO_RING_IDX : 0),
                                         .fence_id = id, .ctx_id = f->ctx_id, .ring_idx = e->ring_idx };
            ret = rutabaga_create_fence(G.r, &rf);
            if (ret) { pthread_mutex_lock(&G.flock); fe->done = true; pthread_mutex_unlock(&G.flock); }
            int sfd = sync_create(c->proc, &id, 1, "virtgpu");
            if (sfd < 0) { r = sfd; break; }
            e->fence_fd = sfd;
        }
        break;
    }
    case DRM_IOCTL_VIRTGPU_WAIT: break;                      // nothing is ever busy from the guest's view
    case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST: {
        struct drm_virtgpu_3d_transfer_to_host *t = a;
        GpuRes *res = handle_get(f, t->bo_handle);
        if (!res) { r = -ENOENT; break; }
        struct rutabaga_transfer tr = { t->box.x, t->box.y, t->box.z, t->box.w, t->box.h, t->box.d, t->level, t->stride, t->layer_stride, t->offset };
        if (rutabaga_resource_transfer_write(G.r, f->ctx_id, res->res_id, &tr)) r = -EINVAL;
        break;
    }
    case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST: {
        struct drm_virtgpu_3d_transfer_from_host *t = a;
        GpuRes *res = handle_get(f, t->bo_handle);
        if (!res) { r = -ENOENT; break; }
        struct rutabaga_transfer tr = { t->box.x, t->box.y, t->box.z, t->box.w, t->box.h, t->box.d, t->level, t->stride, t->layer_stride, t->offset };
        if (rutabaga_resource_transfer_read(G.r, f->ctx_id, res->res_id, &tr, NULL)) r = -EINVAL;
        break;
    }
    default:
        if (f->card) r = kms_ioctl(c, f, req, a);
        else { LOGV("onirun: virtgpu: unknown ioctl %llx\n", (unsigned long long)req); r = -ENOTTY; }
    }
    UNLOCK();
    return r;
}

static int64_t gpu_mmap(FdObj *o, GuestCtx *c, uint64_t addr, uint64_t len, int prot, int lflags, uint64_t off) {
    (void)lflags;
    GpuFile *f = o->data;
    LOCK();
    GpuRes *res = handle_get(f, (uint32_t)(off >> 20));
    int64_t r = res ? map_res(c, res, addr, len, prot, off & ((1u << 20) - 1)) : -EINVAL;
    UNLOCK();
    return r;
}

static const FdOps gpu_ops = { "drm-render", gpu_read, NULL, gpu_ioctl, NULL, gpu_release, gpu_mmap };
static const FdOps card_ops = { "drm-card", gpu_read, NULL, gpu_ioctl, NULL, gpu_release, gpu_mmap };

int gpu_open(Proc *p, bool card, int lflags) {
    LOCK();
    bool ok = gpu_init_locked();
    UNLOCK();
    if (!ok) return -ENODEV;
    GpuFile *f = calloc(1, sizeof *f);
    f->card = card;
    FdObj *o = obj_new(card ? &card_ops : &gpu_ops, f, true);
    f->o = o;
    if (lflags & 04000) fcntl(o->rd, F_SETFL, O_NONBLOCK);
    fd_set_path(o->rd, card ? "/dev/dri/card0" : "/dev/dri/renderD128");
    return fd_install_obj(p, o->rd, o, lflags & 02000000);
}
