/*
 * AndroidOniOS: C API exported by the QEMU library to the host app.
 *
 * All functions are safe to call from any thread unless noted.  Display
 * callbacks are invoked on QEMU's main-loop thread; they must copy what they
 * need and return quickly.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DROID_QEMU_H
#define DROID_QEMU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DROID_EXPORT __attribute__((visibility("default")))

/* New framebuffer: 32bpp pixels, B,G,R,A byte order when bgra is true (x8r8g8b8), R,G,B,A
 * otherwise (x8b8g8r8). pixels may be NULL (no scanout). */
typedef void (*droid_surface_fn)(void *opaque, void *pixels, int width, int height, int stride,
                                 bool bgra);
/* Region of the current framebuffer changed. */
typedef void (*droid_update_fn)(void *opaque, int x, int y, int w, int h);

/* Attach to console 0. Call after qemu_init(). */
DROID_EXPORT void droid_display_attach(void *opaque, droid_surface_fn on_surface,
                                       droid_update_fn on_update);

enum { DROID_TOUCH_BEGIN = 0, DROID_TOUCH_MOVE = 1, DROID_TOUCH_END = 2 };
/* x/y in framebuffer pixels (0..width-1 / 0..height-1). slot 0..9. */
DROID_EXPORT void droid_touch(int slot, int phase, int x, int y, int width, int height);
/* QKeyCode values (see qapi/ui.json) */
DROID_EXPORT void droid_key(int qcode, bool down);

DROID_EXPORT void droid_powerdown(void);   /* ACPI/PSCI power button */
DROID_EXPORT void droid_reset(void);
DROID_EXPORT void droid_set_running(bool running); /* pause/resume vCPUs */
DROID_EXPORT bool droid_is_running(void);

/* ---- save state ----
 * droid_save_state() pauses the VM and writes its complete state (RAM, devices, GPU renderer)
 * to `path`; `done` runs on QEMU's main-loop thread with 0 on success or a negative value. The VM
 * stays paused; droid_set_running(true) continues it (the saved file is then stale, because the
 * disks change). Start QEMU with "-incoming file:PATH" to restore; droid_restore_status() is 1
 * while QEMU starts and loads, and 0 after, when the VM is paused at the saved point. A failed
 * load exits QEMU.
 */
typedef void (*droid_save_done_fn)(void *opaque, int status);
DROID_EXPORT void droid_save_state(const char *path, droid_save_done_fn done, void *opaque);
DROID_EXPORT int droid_restore_status(void);

#ifdef __cplusplus
}
#endif
#endif
