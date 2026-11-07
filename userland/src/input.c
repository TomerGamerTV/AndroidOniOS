// /dev/input/event0: a virtual multitouch screen (evdev, protocol B) plus a few navigation keys,
// fed by the --window display (display.m). EventHub finds it by scanning /dev/input, reads the
// capabilities with the EVIOC* ioctls and classifies it as the touchscreen (INPUT_PROP_DIRECT with
// ABS_MT_POSITION_X/Y); events are struct input_event records with CLOCK_MONOTONIC timestamps.
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sys.h"

enum { EV_SYN = 0, EV_KEY = 1, EV_ABS = 3, SYN_REPORT = 0, BTN_TOUCH = 0x14a,
       ABS_MT_SLOT = 0x2f, ABS_MT_POSITION_X = 0x35, ABS_MT_POSITION_Y = 0x36, ABS_MT_TRACKING_ID = 0x39 };
static const int nav_keys[] = { 1 /* ESC */, 102 /* HOME */, 116 /* POWER */, 114 /* VOLUMEDOWN */, 115 /* VOLUMEUP */,
                                139 /* MENU */, 158 /* BACK */, 172 /* HOMEPAGE */, 580 /* APPSELECT */ };

typedef struct { int64_t sec, usec; uint16_t type, code; int32_t value; } InputEvent;   // 24 bytes on 64-bit
_Static_assert(sizeof(InputEvent) == 24, "input_event");

typedef struct Reader { FdObj *o; InputEvent *q; size_t n, cap; struct Reader *next; } Reader;
static pthread_mutex_t in_lock = PTHREAD_MUTEX_INITIALIZER;
static Reader *readers;
static uint32_t scr_w = 1080, scr_h = 2400;
static int next_tracking_id;

static void bit(uint8_t *m, size_t len, int b) { if ((size_t)(b / 8) < len) m[b / 8] |= (uint8_t)(1 << (b % 8)); }

// Queues one packet (events ending in SYN_REPORT) to every open reader.
static void emit(const InputEvent *ev, int n) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    pthread_mutex_lock(&in_lock);
    for (Reader *r = readers; r; r = r->next) {
        pthread_mutex_lock(&r->o->lock);
        if (r->n + (size_t)n > r->cap) { r->cap = (r->n + (size_t)n) * 2; r->q = realloc(r->q, r->cap * sizeof *r->q); }
        for (int i = 0; i < n; i++) {
            r->q[r->n] = ev[i];
            r->q[r->n].sec = ts.tv_sec;
            r->q[r->n].usec = ts.tv_nsec / 1000;
            r->n++;
        }
        obj_set_ready(r->o, true);
        pthread_mutex_unlock(&r->o->lock);
    }
    pthread_mutex_unlock(&in_lock);
}

// phase: 0 down, 1 move, 2 up; x/y in screen pixels.
void input_touch(int phase, int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x >= scr_w) x = (int)scr_w - 1;
    if ((uint32_t)y >= scr_h) y = (int)scr_h - 1;
    InputEvent ev[6];
    int n = 0;
    ev[n++] = (InputEvent){ .type = EV_ABS, .code = ABS_MT_SLOT, .value = 0 };
    if (phase == 0) ev[n++] = (InputEvent){ .type = EV_ABS, .code = ABS_MT_TRACKING_ID, .value = next_tracking_id++ & 0xffff };
    if (phase == 2) ev[n++] = (InputEvent){ .type = EV_ABS, .code = ABS_MT_TRACKING_ID, .value = -1 };
    else {
        ev[n++] = (InputEvent){ .type = EV_ABS, .code = ABS_MT_POSITION_X, .value = x };
        ev[n++] = (InputEvent){ .type = EV_ABS, .code = ABS_MT_POSITION_Y, .value = y };
    }
    if (phase != 1) ev[n++] = (InputEvent){ .type = EV_KEY, .code = BTN_TOUCH, .value = phase == 0 };
    ev[n++] = (InputEvent){ .type = EV_SYN, .code = SYN_REPORT };
    emit(ev, n);
}

void input_key(int code, bool down) {
    InputEvent ev[2] = { { .type = EV_KEY, .code = (uint16_t)code, .value = down }, { .type = EV_SYN, .code = SYN_REPORT } };
    emit(ev, 2);
}

static int64_t in_read(FdObj *o, GuestCtx *c, void *buf, size_t n, bool nonblock) {
    (void)c;
    Reader *r = o->data;
    if (n < sizeof(InputEvent)) return -EINVAL;
    for (;;) {
        pthread_mutex_lock(&o->lock);
        if (r->n) {
            size_t k = n / sizeof(InputEvent);
            if (k > r->n) k = r->n;
            memcpy(buf, r->q, k * sizeof(InputEvent));
            memmove(r->q, r->q + k, (r->n - k) * sizeof(InputEvent));
            r->n -= k;
            obj_set_ready(o, r->n != 0);
            pthread_mutex_unlock(&o->lock);
            return (int64_t)(k * sizeof(InputEvent));
        }
        pthread_mutex_unlock(&o->lock);
        int e = obj_wait(o->rd, POLLIN, nonblock);
        if (e) return e;
    }
}

static int64_t in_write(FdObj *o, GuestCtx *c, const void *buf, size_t n, bool nonblock) { (void)o; (void)c; (void)buf; (void)nonblock; return (int64_t)n; }

static int64_t in_ioctl(FdObj *o, GuestCtx *c, uint64_t req, uint64_t arg) {
    (void)o;
    if (((req >> 8) & 0xff) != 'E') return -ENOTTY;
    uint32_t nr = req & 0xff, len = (req >> 16) & 0x3fff;
    uint8_t *p = arg ? (uint8_t *)(uintptr_t)(c->base + UNTAG(arg)) : NULL;
    if (!p && nr != 0x90 && nr != 0xa0) return -EFAULT;
    if (nr == 0x01) { *(int32_t *)p = 0x010001; return 0; }                       // EVIOCGVERSION
    if (nr == 0x02) { uint16_t id[4] = { 0x06, 0x0001, 0x0001, 1 }; memcpy(p, id, 8); return 0; }   // EVIOCGID: BUS_VIRTUAL
    if (nr == 0x03) { memset(p, 0, 8); return 0; }                                // EVIOCGREP
    if (nr == 0x06) {                                                             // EVIOCGNAME
        static const char name[] = "onirun touchscreen";
        size_t k = len < sizeof name ? len : sizeof name;
        memcpy(p, name, k);
        if (k) p[k - 1] = 0;
        return (int64_t)k;
    }
    if (nr == 0x07 || nr == 0x08) return -ENOENT;                                 // EVIOCGPHYS, EVIOCGUNIQ
    if (nr == 0x09) { memset(p, 0, len); bit(p, len, 1 /* INPUT_PROP_DIRECT */); return len; }
    if (nr == 0x18 || nr == 0x19 || nr == 0x1a || nr == 0x1b) { memset(p, 0, len); return len; }   // key/led/snd/sw state
    if (nr >= 0x20 && nr < 0x40) {                                                // EVIOCGBIT(ev)
        memset(p, 0, len);
        int ev = (int)nr - 0x20;
        if (ev == 0) { bit(p, len, EV_SYN); bit(p, len, EV_KEY); bit(p, len, EV_ABS); }
        else if (ev == EV_KEY) { bit(p, len, BTN_TOUCH); for (unsigned i = 0; i < sizeof nav_keys / sizeof *nav_keys; i++) bit(p, len, nav_keys[i]); }
        else if (ev == EV_ABS) { bit(p, len, ABS_MT_SLOT); bit(p, len, ABS_MT_POSITION_X); bit(p, len, ABS_MT_POSITION_Y); bit(p, len, ABS_MT_TRACKING_ID); }
        return len;
    }
    if (nr >= 0x40 && nr < 0x80) {                                                // EVIOCGABS(axis): struct input_absinfo
        int axis = (int)nr - 0x40;
        int32_t ai[6] = {0};
        if (axis == ABS_MT_POSITION_X) ai[2] = (int32_t)scr_w - 1;
        else if (axis == ABS_MT_POSITION_Y) ai[2] = (int32_t)scr_h - 1;
        else if (axis == ABS_MT_TRACKING_ID) ai[2] = 0xffff;
        else if (axis == ABS_MT_SLOT) ai[2] = 0;                                  // one finger
        else return -EINVAL;
        memcpy(p, ai, sizeof ai);
        return 0;
    }
    if (nr == 0x90 || nr == 0xa0 || nr == 0x84 || nr == 0x80 || nr == 0x81) return 0;   // GRAB, SCLOCKID, GEFFECTS, SFF, RMFF
    return -EINVAL;
}

static void in_release(FdObj *o) {
    Reader *r = o->data;
    pthread_mutex_lock(&in_lock);
    for (Reader **pp = &readers; *pp; pp = &(*pp)->next) if (*pp == r) { *pp = r->next; break; }
    pthread_mutex_unlock(&in_lock);
    free(r->q);
    free(r);
}
static const FdOps in_ops = { "evdev", in_read, in_write, in_ioctl, NULL, in_release };

int input_open(Proc *p, const char *path, int lflags) {
    gpu_display_size(&scr_w, &scr_h);
    Reader *r = calloc(1, sizeof *r);
    FdObj *o = obj_new(&in_ops, r, true);
    if (!o) { free(r); return -ENFILE; }
    r->o = o;
    fd_set_path(o->rd, path);
    pthread_mutex_lock(&in_lock);
    r->next = readers;
    readers = r;
    pthread_mutex_unlock(&in_lock);
    return fd_install_obj(p, o->rd, o, lflags & 02000000);
}
