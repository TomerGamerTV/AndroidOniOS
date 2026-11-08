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
#include "qapi/error.h"
#include "qapi/qapi-commands-migration.h"
#include "qapi/qapi-commands-misc.h"
#include "qemu/timer.h"

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
static int droid_active_slots;

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
        if (!droid_tracking_ids[slot]) {
            droid_active_slots++;
        }
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
        /* Android's evdev mapper treats a BTN_TOUCH device without the button down as hovering */
        if (t->phase == DROID_TOUCH_BEGIN && droid_active_slots == 1) {
            qemu_input_queue_btn(NULL, INPUT_BUTTON_TOUCH, true);
        }
        qemu_input_event_sync();
        break;
    }
    case DROID_TOUCH_END:
        if (droid_tracking_ids[slot]) {
            qemu_input_queue_mtt(NULL, INPUT_MULTI_TOUCH_TYPE_END, slot, -1);
            if (--droid_active_slots == 0) {
                qemu_input_queue_btn(NULL, INPUT_BUTTON_TOUCH, false);
            }
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
        if (runstate_check(RUN_STATE_INMIGRATE) || runstate_check(RUN_STATE_FINISH_MIGRATE)) {
            return; /* loading or saving state; the app resumes once that is done */
        }
        /* Like the "cont" command: also takes the disks back after a completed save. */
        Error *err = NULL;
        qmp_cont(&err);
        if (err) {
            error_report_err(err);
        }
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

/* ---- save state ---- */

typedef struct DroidSave {
    char *path;
    droid_save_done_fn done;
    void *opaque;
    QEMUTimer *timer;
} DroidSave;

static void droid_save_finish(DroidSave *s, int status)
{
    if (s->timer) {
        timer_free(s->timer);
    }
    s->done(s->opaque, status);
    g_free(s->path);
    g_free(s);
}

static void droid_save_poll(void *opaque)
{
    DroidSave *s = opaque;
    MigrationInfo *info = qmp_query_migrate(NULL);
    int status = 1;

    if (info->has_status) {
        switch (info->status) {
        case MIGRATION_STATUS_COMPLETED:
            status = 0;
            break;
        case MIGRATION_STATUS_FAILED:
        case MIGRATION_STATUS_CANCELLED:
            status = -EIO;
            fprintf(stderr, "droid: save state failed: %s\n",
                    info->error_desc ? info->error_desc : "unknown error");
            break;
        default:
            break;
        }
    }
    qapi_free_MigrationInfo(info);
    if (status == 1) {
        timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 20);
        return;
    }
    droid_save_finish(s, status);
}

static void droid_save_bh(void *opaque)
{
    DroidSave *s = opaque;
    Error *err = NULL;

    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    }
    /* The VM is stopped, so nothing is gained by rate limiting the write (default 128 MiB/s). */
    MigrateSetParameters params = {
        .has_max_bandwidth = true,
        .max_bandwidth = 100ULL << 30,
    };
    qmp_migrate_set_parameters(&params, &err);
    if (err) {
        error_report_err(err);
        err = NULL;
    }
    g_autofree char *uri = g_strdup_printf("file:%s", s->path);
    qmp_migrate(uri, false, NULL, false, false, false, false, &err);
    if (err) {
        fprintf(stderr, "droid: save state: %s\n", error_get_pretty(err));
        error_free(err);
        droid_save_finish(s, -EIO);
        return;
    }
    s->timer = timer_new_ms(QEMU_CLOCK_REALTIME, droid_save_poll, s);
    timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 20);
}

void droid_save_state(const char *path, droid_save_done_fn done, void *opaque)
{
    DroidSave *s;

    if (!qemu_get_aio_context()) {
        done(opaque, -ENODEV);
        return;
    }
    s = g_new0(DroidSave, 1);
    s->path = g_strdup(path);
    s->done = done;
    s->opaque = opaque;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), droid_save_bh, s);
}

int droid_restore_status(void)
{
    /* Still starting up or loading the saved state. */
    return runstate_check(RUN_STATE_PRELAUNCH) || runstate_check(RUN_STATE_INMIGRATE) ? 1 : 0;
}
