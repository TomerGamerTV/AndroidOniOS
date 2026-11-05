#ifndef DROID_BRIDGING_HEADER_H
#define DROID_BRIDGING_HEADER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ---- process / JIT ---- */
/* true when the process has been debugged (CS_DEBUGGED): JIT memory is allowed. */
bool droid_bridge_jit_available(void);
/* true while a debugger is attached (P_TRACED). */
bool droid_bridge_debugger_attached(void);
/* Raise the jetsam memory limit as far as the entitlement allows. */
void droid_bridge_raise_memory_limit(void);

/* ---- sockets ---- */
int droid_bridge_unix_listen(const char *path);
int droid_bridge_unix_connect(const char *path);
int droid_bridge_accept(int fd);
int droid_bridge_set_nonblock(int fd, bool nonblock);

/* ---- QEMU library (loaded with dlopen) ---- */
typedef void (*droid_surface_fn)(void *opaque, void *pixels, int width, int height, int stride);
typedef void (*droid_update_fn)(void *opaque, int x, int y, int w, int h);

typedef struct DroidQEMU {
    void *handle;
    void (*qemu_init)(int argc, char **argv);
    int (*qemu_main_loop)(void);
    void (*qemu_cleanup)(int status);
    void (*display_attach)(void *opaque, droid_surface_fn, droid_update_fn);
    void (*touch)(int slot, int phase, int x, int y, int w, int h);
    void (*key)(int qcode, bool down);
    void (*powerdown)(void);
    void (*reset)(void);
    void (*set_running)(bool running);
    bool (*is_running)(void);
    /* Display callbacks to register right after qemu_init (QEMU's main loop must exist). */
    void *display_opaque;
    droid_surface_fn display_surface;
    droid_update_fn display_update;
} DroidQEMU;

/* Loads the given QEMU framework binary. Returns 0 or -1 (error in errbuf). */
int droid_qemu_load(const char *path, DroidQEMU *out, char *errbuf, size_t errlen);
/* Runs qemu_init + main loop + cleanup. Blocks until QEMU exits. */
int droid_qemu_run(DroidQEMU *q, int argc, const char *const *argv);

/* ---- modem simulator (modem.c) ---- */
int droid_modem_start(const char *socket_path, const char *log_path);

#endif
