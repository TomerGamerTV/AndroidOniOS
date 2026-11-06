// System properties. Readers are Android's own libc, which maps the files in /dev/__properties__
// (copied out of a running Android). onirun plays init's part: it serves the property_service
// socket and writes updates into those files using bionic's prop_area format, then wakes waiters.
#include <errno.h>
#include <fcntl.h>
#include <os/os_sync_wait_on_address.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sys.h"

#define PA_SIZE (128 * 1024)
#define PA_HDR 128
#define PA_DATA (PA_SIZE - PA_HDR)
#define PROP_AREA_MAGIC 0x504f5250u
#define PROP_AREA_VERSION 0xfc6ed0abu
#define PROP_VALUE_MAX 92
#define PROP_NAME_MAX 32   // only the legacy message is limited to this

enum { PROP_SUCCESS = 0, PROP_ERROR_READ_ONLY_PROPERTY = 0xB, PROP_ERROR_INVALID_NAME = 0x11,
       PROP_ERROR_INVALID_VALUE = 0x12, PROP_ERROR_PERMISSION_DENIED = 0x13, PROP_ERROR_SET_FAILED = 0x14 };

typedef struct { uint32_t bytes_used, serial, magic, version, reserved[28]; uint8_t data[]; } PropArea;
// prop_bt: namelen, prop, left, right, children (all uint32), then name. prop_info: serial, value[92], name.
enum { BT_NAMELEN, BT_PROP, BT_LEFT, BT_RIGHT, BT_CHILDREN, BT_WORDS };

typedef struct Ctx { char name[128]; PropArea *area; struct Ctx *next; } Ctx;
typedef struct Rule { char prefix[128]; bool exact; char ctx[128]; struct Rule *next; } Rule;
static Ctx *ctxs;
static Rule *rules;
static PropArea *serial_area;    // /dev/__properties__/properties_serial
static pthread_mutex_t props_lock = PTHREAD_MUTEX_INITIALIZER;

static void wake(uint32_t *word) {
    os_sync_wake_by_address_all(word, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
    os_sync_wake_by_address_all(word, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

static PropArea *map_area(const char *file, bool create) {
    char path[1200];
    snprintf(path, sizeof path, "%s/dev/__properties__/%s", g_rootfs, file);
    int fd = open(path, O_RDWR);
    if (fd < 0 && create) {
        fd = open(path, O_RDWR | O_CREAT, 0644);
        if (fd < 0) return NULL;
        ftruncate(fd, PA_SIZE);
    }
    if (fd < 0) return NULL;
    PropArea *a = mmap(NULL, PA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (a == MAP_FAILED) return NULL;
    if (a->magic != PROP_AREA_MAGIC) {                 // fresh area: header plus the root node
        memset(a, 0, PA_SIZE);
        a->magic = PROP_AREA_MAGIC;
        a->version = PROP_AREA_VERSION;
        a->bytes_used = BT_WORDS * 4 + 4;              // root prop_bt with an empty name
    }
    return a;
}

static Ctx *ctx_get(const char *name) {
    for (Ctx *c = ctxs; c; c = c->next) if (!strcmp(c->name, name)) return c;
    PropArea *a = map_area(name, true);
    if (!a) return NULL;
    Ctx *c = calloc(1, sizeof *c);
    snprintf(c->name, sizeof c->name, "%s", name);
    c->area = a;
    c->next = ctxs;
    ctxs = c;
    return c;
}

// ---- property_info: the serialized trie Android's libc uses to find a property's context ----
// (system/core/property_service/libpropertyinfoserializer). Using the same file as libc means we
// always write where libc will look.
static const uint8_t *pinfo;
static uint32_t pinfo_size;
static inline uint32_t pu32(uint32_t off) { uint32_t v; memcpy(&v, pinfo + off, 4); return v; }
static const char *pstr(uint32_t off) { return off < pinfo_size ? (const char *)pinfo + off : ""; }
// TrieNodeInternal: property_entry, num_child_nodes, child_nodes, num_prefixes, prefix_entries, num_exact_matches, exact_match_entries
// PropertyEntry: name_offset, namelen, context_index, type_index
static uint32_t entry_ctx(uint32_t entry) { return entry ? pu32(entry + 8) : ~0u; }
static const char *entry_name(uint32_t entry) { return pstr(pu32(entry)); }

static bool pinfo_load(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/dev/__properties__/property_info", g_rootfs);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 32) { close(fd); return false; }
    pinfo = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (pinfo == MAP_FAILED) { pinfo = NULL; return false; }
    pinfo_size = (uint32_t)st.st_size;
    // Header: current_version, minimum_supported_version, size, contexts_offset, types_offset, root_offset
    return pu32(8) == pinfo_size && pu32(12) < pinfo_size && pu32(20) < pinfo_size;
}

static const char *pinfo_ctx(const char *name) {
    if (!pinfo) return NULL;
    uint32_t node = pu32(20);                          // root
    uint32_t ctx = entry_ctx(pu32(node));
    const char *rem = name;
    for (;;) {
        const char *sep = strchr(rem, '.');
        uint32_t len = sep ? (uint32_t)(sep - rem) : (uint32_t)strlen(rem);
        // prefixes at this node
        uint32_t np = pu32(node + 12), pe = pu32(node + 16);
        for (uint32_t i = 0; i < np; i++) {
            uint32_t e = pu32(pe + 4 * i);
            if (!strncmp(rem, entry_name(e), pu32(e + 4))) { ctx = entry_ctx(e); break; }
        }
        if (!sep) break;
        // child with this component name (children are sorted; linear is fine here)
        uint32_t nc = pu32(node + 4), ca = pu32(node + 8), child = 0;
        for (uint32_t i = 0; i < nc && !child; i++) {
            uint32_t cn = pu32(ca + 4 * i), ce = pu32(cn);
            const char *n = entry_name(ce);
            if (strlen(n) == len && !strncmp(n, rem, len)) child = cn;
        }
        if (!child) break;
        node = child;
        if (entry_ctx(pu32(node)) != ~0u) ctx = entry_ctx(pu32(node));
        rem = sep + 1;
    }
    // Exact entries hang off the node of the last component and carry just that component's name
    // (property_info_parser compares them with remaining_name).
    uint32_t ne = pu32(node + 20), ea = pu32(node + 24);
    for (uint32_t i = 0; i < ne; i++) {
        uint32_t e = pu32(ea + 4 * i);
        if (!strcmp(rem, entry_name(e))) { ctx = entry_ctx(e); break; }
    }
    if (ctx == ~0u) return NULL;
    uint32_t coff = pu32(12), nctx = pu32(coff), carr = coff + 4;   // count, then string offsets
    return ctx < nctx ? pstr(pu32(carr + 4 * ctx)) : NULL;
}

static const char *ctx_for(const char *prop) {
    const char *from_info = pinfo_ctx(prop);
    if (from_info) return from_info;
    const Rule *best = NULL;
    size_t bl = 0;
    for (Rule *r = rules; r; r = r->next) {
        size_t l = strlen(r->prefix);
        bool m = r->exact ? !strcmp(prop, r->prefix) : !strncmp(prop, r->prefix, l);
        if (m && (!best || l > bl)) { best = r; bl = l; }
    }
    return best ? best->ctx : "u:object_r:default_prop:s0";
}

static void load_rules(const char *guest_file) {
    char path[1200];
    snprintf(path, sizeof path, "%s%s", g_rootfs, guest_file);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char prefix[128], ctx[128], kind[32] = "";
        if (line[0] == '#' || sscanf(line, "%127s %127s %31s", prefix, ctx, kind) < 2) continue;
        Rule *r = calloc(1, sizeof *r);
        snprintf(r->prefix, sizeof r->prefix, "%s", prefix);
        snprintf(r->ctx, sizeof r->ctx, "%s", ctx);
        r->exact = !strcmp(kind, "exact");
        r->next = rules;
        rules = r;
    }
    fclose(f);
}

// ---- prop_area trie ----
static uint32_t *bt(PropArea *a, uint32_t off) { return (uint32_t *)(void *)(a->data + off); }
static char *bt_name(PropArea *a, uint32_t off) { return (char *)(a->data + off + BT_WORDS * 4); }

static uint32_t alloc_obj(PropArea *a, uint32_t size) {
    size = (size + 3) & ~3u;
    if (a->bytes_used + size > PA_DATA) return 0;
    uint32_t off = a->bytes_used;
    a->bytes_used += size;
    memset(a->data + off, 0, size);
    return off;
}

static uint32_t new_bt(PropArea *a, const char *name, uint32_t len) {
    uint32_t off = alloc_obj(a, BT_WORDS * 4 + len + 1);
    if (!off) return 0;
    bt(a, off)[BT_NAMELEN] = len;
    memcpy(bt_name(a, off), name, len);
    return off;
}

static int cmp_name(const char *one, uint32_t one_len, const char *two, uint32_t two_len) {
    if (one_len < two_len) return -1;
    if (one_len > two_len) return 1;
    return strncmp(one, two, one_len);
}

// Finds or (alloc) creates the sibling node for a name component.
static uint32_t find_bt(PropArea *a, uint32_t node, const char *name, uint32_t len, bool alloc) {
    for (;;) {
        int c = cmp_name(name, len, bt_name(a, node), bt(a, node)[BT_NAMELEN]);
        if (c == 0) return node;
        int link = c < 0 ? BT_LEFT : BT_RIGHT;
        if (bt(a, node)[link]) { node = bt(a, node)[link]; continue; }
        if (!alloc) return 0;
        uint32_t nn = new_bt(a, name, len);
        if (nn) __atomic_store_n(&bt(a, node)[link], nn, __ATOMIC_RELEASE);
        return nn;
    }
}

// Returns the offset of the prop_info for name, creating it with value if alloc; 0 if absent/full.
static uint32_t find_prop(PropArea *a, const char *name, const char *value, bool alloc) {
    uint32_t cur = 0;                                  // root node
    const char *rem = name;
    for (;;) {
        const char *sep = strchr(rem, '.');
        uint32_t len = sep ? (uint32_t)(sep - rem) : (uint32_t)strlen(rem);
        if (!len) return 0;
        uint32_t root = bt(a, cur)[BT_CHILDREN];
        if (!root) {
            if (!alloc) return 0;
            root = new_bt(a, rem, len);
            if (!root) return 0;
            __atomic_store_n(&bt(a, cur)[BT_CHILDREN], root, __ATOMIC_RELEASE);
        }
        cur = find_bt(a, root, rem, len, alloc);
        if (!cur) return 0;
        if (!sep) break;
        rem = sep + 1;
    }
    uint32_t pi = bt(a, cur)[BT_PROP];
    if (pi || !alloc) return pi;
    uint32_t vlen = (uint32_t)strlen(value), nlen = (uint32_t)strlen(name);
    pi = alloc_obj(a, 4 + PROP_VALUE_MAX + nlen + 1);
    if (!pi) return 0;
    uint32_t *serial = (uint32_t *)(void *)(a->data + pi);
    memcpy(a->data + pi + 4, value, vlen + 1);
    memcpy(a->data + pi + 4 + PROP_VALUE_MAX, name, nlen + 1);
    *serial = vlen << 24;
    __atomic_store_n(&bt(a, cur)[BT_PROP], pi, __ATOMIC_RELEASE);
    return pi;
}

static int prop_set_locked(const char *name, const char *value) {
    size_t nl = strlen(name), vl = strlen(value);
    if (!nl || nl >= 128 || name[0] == '.' || name[nl - 1] == '.' || strstr(name, "..")) return PROP_ERROR_INVALID_NAME;
    if (vl >= PROP_VALUE_MAX && strncmp(name, "ro.", 3)) return PROP_ERROR_INVALID_VALUE;
    if (vl >= PROP_VALUE_MAX) return PROP_ERROR_INVALID_VALUE;   // long ro. values aren't supported here
    if (!strncmp(name, "ctl.", 4)) {
        LOGV("onirun: control message %s=%s\n", name, value);
        pthread_mutex_unlock(&props_lock);              // init sets init.svc.* properties itself
        init_control(name + 4, value);
        pthread_mutex_lock(&props_lock);
        return PROP_SUCCESS;
    }
    Ctx *c = ctx_get(ctx_for(name));
    if (!c) return PROP_ERROR_SET_FAILED;
    PropArea *a = c->area;
    uint32_t pi = find_prop(a, name, value, false);
    if (pi) {
        if (!strncmp(name, "ro.", 3)) return PROP_ERROR_READ_ONLY_PROPERTY;
        uint32_t *serial = (uint32_t *)(void *)(a->data + pi);
        if (*serial & (1u << 16)) return PROP_ERROR_SET_FAILED;  // a long property
        uint32_t s = *serial;
        __atomic_store_n(serial, s | 1, __ATOMIC_RELEASE);        // dirty while the value changes
        memcpy(a->data + pi + 4, value, vl + 1);
        __atomic_store_n(serial, (((s | 1) + 1) & 0xffffff) | ((uint32_t)vl << 24), __ATOMIC_RELEASE);   // even again
        wake(serial);
    } else if (!find_prop(a, name, value, true)) {
        return PROP_ERROR_SET_FAILED;
    }
    __atomic_add_fetch(&a->serial, 1, __ATOMIC_SEQ_CST);
    wake(&a->serial);
    if (serial_area) { __atomic_add_fetch(&serial_area->serial, 1, __ATOMIC_SEQ_CST); wake(&serial_area->serial); }
    return PROP_SUCCESS;
}

int props_set(const char *name, const char *value) {
    pthread_mutex_lock(&props_lock);
    int r = prop_set_locked(name, value);
    pthread_mutex_unlock(&props_lock);
    return r;
}

// ---- the property_service socket ----
static bool read_all(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool read_str(int fd, char *out, size_t cap) {
    uint32_t len;
    if (!read_all(fd, &len, 4) || len >= cap) return false;
    if (!read_all(fd, out, len)) return false;
    out[len] = 0;
    return true;
}

static void *prop_conn(void *arg) {
    int fd = (int)(intptr_t)arg;
    uint32_t cmd;
    if (read_all(fd, &cmd, 4)) {
        if (cmd == 0x00020001) {                       // PROP_MSG_SETPROP2
            char name[128], value[PROP_VALUE_MAX + 64];
            uint32_t result = PROP_ERROR_INVALID_NAME;
            if (read_str(fd, name, sizeof name) && read_str(fd, value, sizeof value)) {
                result = (uint32_t)props_set(name, value);
                LOGV("onirun: setprop %s=%s -> %u\n", name, value, result);
            }
            write(fd, &result, 4);
        } else if (cmd == 1) {                         // PROP_MSG_SETPROP: fixed layout, no reply
            char name[PROP_NAME_MAX], value[PROP_VALUE_MAX];
            if (read_all(fd, name, sizeof name) && read_all(fd, value, sizeof value)) {
                name[PROP_NAME_MAX - 1] = value[PROP_VALUE_MAX - 1] = 0;
                props_set(name, value);
            }
        }
    }
    close(fd);
    return NULL;
}

static void prop_accept(int fd, int pid, void *arg) {
    (void)pid; (void)arg;
    pthread_t t;
    if (pthread_create(&t, NULL, prop_conn, (void *)(intptr_t)fd)) close(fd);
    else pthread_detach(t);
}

void props_init(void) {
    const char *files[] = {
        "/system/etc/selinux/plat_property_contexts", "/system/system_ext/etc/selinux/system_ext_property_contexts",
        "/system/product/etc/selinux/product_property_contexts", "/vendor/etc/selinux/vendor_property_contexts",
        "/odm/etc/selinux/odm_property_contexts",
    };
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) load_rules(files[i]);
    if (!pinfo_load()) fprintf(stderr, "onirun: warning: no usable /dev/__properties__/property_info; using text rules\n");
    serial_area = map_area("properties_serial", true);
    // Every context gets a valid (possibly empty) area, so readers never map garbage.
    for (Rule *r = rules; r; r = r->next) ctx_get(r->ctx);
    sock_builtin_stream("/dev/socket/property_service", prop_accept, NULL);
    sock_builtin_stream("/dev/socket/property_service_for_system", prop_accept, NULL);
    // Defaults for properties whose areas couldn't be copied from the device: name=value lines.
    char path[1200];
    snprintf(path, sizeof path, "%s/oni/properties.txt", g_rootfs);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[1024];
        int added = 0;
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            if (line[0] == '#' || !eq) continue;
            *eq = 0;
            char *val = eq + 1;
            val[strcspn(val, "\n")] = 0;
            pthread_mutex_lock(&props_lock);
            Ctx *c = ctx_get(ctx_for(line));
            uint32_t have = c ? find_prop(c->area, line, val, false) : 0;
            if (c && !have && strlen(val) < PROP_VALUE_MAX) {
                int rc = prop_set_locked(line, val);
                if (rc == PROP_SUCCESS) added++;
                else LOGV("onirun: property %s=%s not set: %d\n", line, val, rc);
            } else if (getenv("ONIRUN_PROPDEBUG")) {
                fprintf(stderr, "prop %s: ctx %s have %u value '%s'\n", line, ctx_for(line), have,
                        have ? (char *)(c->area->data + have + 4) : "");
            }
            pthread_mutex_unlock(&props_lock);
        }
        fclose(f);
        LOGV("onirun: %d properties filled in from /oni/properties.txt\n", added);
    }
    // Properties that describe this environment rather than the Android image they came from.
    props_set("sys.use_memfd", "1");
    props_set("bpf.progs_loaded", "1");       // no bpfloader; HALs that wait for it may go on
    props_set("init.svc.zygote", "stopped");
    props_set("sys.boot_completed", "0");
}
