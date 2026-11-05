/*
 * AndroidOniOS display/input bridge.
 *
 * Exposes QEMU's console 0 framebuffer to the embedding app through plain C
 * callbacks and lets the app inject multitouch and keyboard events.  Input
 * requests are marshalled onto the main loop with one-shot bottom halves, so
 * the UI thread never blocks on the big QEMU lock.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "block/aio.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/surface.h"
#include "ui/droid.h"
#include "system/runstate.h"
#include "system/cpus.h"

typedef struct DroidDisplay {
    DisplayChangeListener dcl;
    void *opaque;
    droid_surface_fn on_surface;
    droid_update_fn on_update;
} DroidDisplay;

static DroidDisplay droid_dpy;
static bool droid_attached;

static void droid_gfx_update(DisplayChangeListener *dcl, int x, int y, int w, int h)
{
    DisplaySurface *s = qemu_console_surface(dcl->con);
    if (s && (s->flags & QEMU_PLACEHOLDER_FLAG)) {
        return;
    }
    if (droid_dpy.on_update) {
        droid_dpy.on_update(droid_dpy.opaque, x, y, w, h);
    }
}

static void droid_gfx_switch(DisplayChangeListener *dcl, DisplaySurface *surface)
{
    if (!droid_dpy.on_surface) {
        return;
    }
    /* QEMU's "Display output is not active" placeholder is not guest output. */
    if (!surface || (surface->flags & QEMU_PLACEHOLDER_FLAG)) {
        droid_dpy.on_surface(droid_dpy.opaque, NULL, 0, 0, 0, true);
        return;
    }
    /* virtio-gpu scanouts use whatever format the guest picked (gfxstream: RGBA). */
    pixman_format_code_t fmt = surface_format(surface);
    bool bgra = !(fmt == PIXMAN_x8b8g8r8 || fmt == PIXMAN_a8b8g8r8);
    static pixman_format_code_t last_fmt;
    if (fmt != last_fmt) {
        last_fmt = fmt;
        fprintf(stderr, "droid: surface %dx%d pixman format 0x%x (%s)\n", surface_width(surface),
                surface_height(surface), fmt, bgra ? "BGRA" : "RGBA");
    }
    droid_dpy.on_surface(droid_dpy.opaque, surface_data(surface),
                         surface_width(surface), surface_height(surface),
                         surface_stride(surface), bgra);
}

static bool droid_check_format(DisplayChangeListener *dcl, pixman_format_code_t format)
{
    return format == PIXMAN_x8r8g8b8 || format == PIXMAN_a8r8g8b8 ||
           format == PIXMAN_x8b8g8r8 || format == PIXMAN_a8b8g8r8;
}

static void droid_refresh(DisplayChangeListener *dcl)
{
    graphic_hw_update(dcl->con);
}

static const DisplayChangeListenerOps droid_dcl_ops = {
    .dpy_name = "droid",
    .dpy_gfx_update = droid_gfx_update,
    .dpy_gfx_switch = droid_gfx_switch,
    .dpy_gfx_check_format = droid_check_format,
    .dpy_refresh = droid_refresh,
};

typedef struct DroidAttach {
    void *opaque;
    droid_surface_fn on_surface;
    droid_update_fn on_update;
} DroidAttach;

static void droid_attach_bh(void *opaque)
{
    DroidAttach *a = opaque;
    droid_dpy.opaque = a->opaque;
    droid_dpy.on_surface = a->on_surface;
    droid_dpy.on_update = a->on_update;
    g_free(a);
    if (droid_attached) {
        return;
    }
    droid_attached = true;
    droid_dpy.dcl.ops = &droid_dcl_ops;
    droid_dpy.dcl.con = qemu_console_lookup_by_index(0);
    register_displaychangelistener(&droid_dpy.dcl);
}

void droid_display_attach(void *opaque, droid_surface_fn on_surface, droid_update_fn on_update)
{
    if (!qemu_get_aio_context()) {
        return; /* QEMU not initialized yet */
    }
    DroidAttach *a = g_new0(DroidAttach, 1);
    a->opaque = opaque;
    a->on_surface = on_surface;
    a->on_update = on_update;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), droid_attach_bh, a);
}

/* ---- input ---- */

#define DROID_MAX_SLOTS 10

typedef struct DroidTouch {
    int slot, phase, x, y, w, h;
} DroidTouch;

static int droid_tracking_ids[DROID_MAX_SLOTS];
static int droid_next_tracking_id = 1;

static void droid_touch_bh(void *opaque)
{
    DroidTouch *t = opaque;
    int slot = t->slot;
    if (slot < 0 || slot >= DROID_MAX_SLOTS) {
        g_free(t);
        return;
    }
    switch (t->phase) {
    case DROID_TOUCH_BEGIN:
        droid_tracking_ids[slot] = droid_next_tracking_id++;
        if (droid_next_tracking_id > 0xffff) {
            droid_next_tracking_id = 1;
        }
        /* fall through */
    case DROID_TOUCH_MOVE: {
        int id = droid_tracking_ids[slot];
        if (!id) {
            break;
        }
        qemu_input_queue_mtt(NULL, t->phase == DROID_TOUCH_BEGIN ?
                             INPUT_MULTI_TOUCH_TYPE_BEGIN : INPUT_MULTI_TOUCH_TYPE_UPDATE,
                             slot, id);
        qemu_input_queue_mtt_abs(NULL, INPUT_AXIS_X, t->x, 0, t->w - 1, slot, id);
        qemu_input_queue_mtt_abs(NULL, INPUT_AXIS_Y, t->y, 0, t->h - 1, slot, id);
        qemu_input_event_sync();
        break;
    }
    case DROID_TOUCH_END:
        if (droid_tracking_ids[slot]) {
            qemu_input_queue_mtt(NULL, INPUT_MULTI_TOUCH_TYPE_END, slot, -1);
            qemu_input_event_sync();
            droid_tracking_ids[slot] = 0;
        }
        break;
    }
    g_free(t);
}

void droid_touch(int slot, int phase, int x, int y, int width, int height)
{
    if (!qemu_get_aio_context()) {
        return; /* QEMU not initialized yet */
    }
    DroidTouch *t = g_new0(DroidTouch, 1);
    t->slot = slot;
    t->phase = phase;
    t->x = x;
    t->y = y;
    t->w = width > 1 ? width : 2;
    t->h = height > 1 ? height : 2;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), droid_touch_bh, t);
}

typedef struct DroidKey {
    int qcode;
    bool down;
} DroidKey;

static void droid_key_bh(void *opaque)
{
    DroidKey *k = opaque;
    if (k->qcode > 0 && k->qcode < Q_KEY_CODE__MAX) {
        qemu_input_event_send_key_qcode(NULL, k->qcode, k->down);
    }
    g_free(k);
}

void droid_key(int qcode, bool down)
{
    if (!qemu_get_aio_context()) {
        return; /* QEMU not initialized yet */
    }
    DroidKey *k = g_new0(DroidKey, 1);
    k->qcode = qcode;
    k->down = down;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), droid_key_bh, k);
}

/* ---- power ---- */

static void droid_powerdown_bh(void *opaque)
{
    qemu_system_powerdown_request();
}

void droid_powerdown(void)
{
    if (!qemu_get_aio_context()) {
        return; /* QEMU not initialized yet */
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), droid_powerdown_bh, NULL);
}

static void droid_reset_bh(void *opaque)
{
    qemu_system_reset_request(SHUTDOWN_CAUSE_HOST_UI);
}

void droid_reset(void)
{
    if (!qemu_get_aio_context()) {
        return; /* QEMU not initialized yet */
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), droid_reset_bh, NULL);
}

static void droid_run_bh(void *opaque)
{
    bool run = (bool)(uintptr_t)opaque;
    if (run && !runstate_is_running()) {
        vm_start();
    } else if (!run && runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    }
}

void droid_set_running(bool running)
{
    if (!qemu_get_aio_context()) {
        return; /* QEMU not initialized yet */
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), droid_run_bh, (void *)(uintptr_t)running);
}

bool droid_is_running(void)
{
    return runstate_is_running();
}
