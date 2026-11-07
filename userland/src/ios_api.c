// Onirun.framework: the entry points the Droid app loads from its engine frameworks (the same names
// QEMUJIT/QEMUTCTI export, see app/Droid/Bridge/Droid-Bridging-Header.h), so the app runs onirun
// the way it runs QEMU: qemu_init with a command line, qemu_main_loop on its own thread, frames
// through droid_display_attach's callbacks, touches and keys into the virtual touchscreen.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "onx.h"
#include "sys.h"

#define API __attribute__((visibility("default")))

static int run_argc;
static char **run_argv;

// The framework's environment (classpaths etc.) comes from the rootfs, as run-init.sh sources it on
// a Mac: "export K=V K2=V2" lines in <root>/data/local/tmp/android-env.sh.
static void load_android_env(const char *root) {
    char path[1200], line[8192];
    snprintf(path, sizeof path, "%s/data/local/tmp/android-env.sh", root);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "onirun: %s: %s\n", path, strerror(errno)); return; }
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "export ", 7)) continue;
        char *save = NULL;
        for (char *t = strtok_r(line + 7, " \t\n", &save); t; t = strtok_r(NULL, " \t\n", &save)) {
            char *eq = strchr(t, '=');
            if (eq) { *eq = 0; setenv(t, eq + 1, 1); }
        }
    }
    fclose(f);
}

API void qemu_init(int argc, char **argv) {
    for (int i = 0; i + 1 < argc; i++) if (!strcmp(argv[i], "--root")) load_android_env(argv[i + 1]);
    run_argc = argc;
    run_argv = calloc((size_t)argc + 1, sizeof *run_argv);
    for (int i = 0; i < argc; i++) run_argv[i] = strdup(argv[i]);
}
API int qemu_main_loop(void) { return onirun_main(run_argc, run_argv); }   // runs Android; does not return
API void qemu_cleanup(int status) { (void)status; }

API void droid_display_attach(void *opaque, void (*surface)(void *, void *, int, int, int, bool),
                              void (*update)(void *, int, int, int, int)) {
    gpu_display_attach(opaque, surface, update);
}

// x, y in a w x h space (the app's idea of the screen); slots are fingers 0..9.
API void droid_touch(int slot, int phase, int x, int y, int w, int h) {
    uint32_t sw = 1080, sh = 2400;
    gpu_display_size(&sw, &sh);
    if (w > 0 && h > 0) { x = (int)((int64_t)x * sw / w); y = (int)((int64_t)y * sh / h); }
    input_touch_slot(slot, phase, x, y);
}

// QEMU QKeyCode (qapi/ui.json order, as the app's KeyMap produces) -> Linux KEY_*.
static const struct { short q, k; } keymap[] = {
    {1, 42}, {2, 54}, {3, 56}, {4, 100}, {5, 29}, {6, 97}, {7, 139}, {8, 1},
    {9, 2}, {10, 3}, {11, 4}, {12, 5}, {13, 6}, {14, 7}, {15, 8}, {16, 9}, {17, 10}, {18, 11},
    {19, 12}, {20, 13}, {21, 14}, {22, 15},
    {23, 16}, {24, 17}, {25, 18}, {26, 19}, {27, 20}, {28, 21}, {29, 22}, {30, 23}, {31, 24}, {32, 25},
    {33, 26}, {34, 27}, {35, 28},
    {36, 30}, {37, 31}, {38, 32}, {39, 33}, {40, 34}, {41, 35}, {42, 36}, {43, 37}, {44, 38},
    {45, 39}, {46, 40}, {47, 41}, {48, 43},
    {49, 44}, {50, 45}, {51, 46}, {52, 47}, {53, 48}, {54, 49}, {55, 50}, {56, 51}, {57, 52}, {58, 53},
    {59, 55}, {60, 57}, {61, 58},
    {62, 59}, {63, 60}, {64, 61}, {65, 62}, {66, 63}, {67, 64}, {68, 65}, {69, 66}, {70, 67}, {71, 68},
    {92, 87}, {93, 88},
    {95, 102}, {96, 104}, {97, 109}, {98, 107}, {99, 105}, {100, 103}, {101, 108}, {102, 106},
    {103, 110}, {104, 111}, {117, 125}, {118, 126}, {129, 116},
    {132, 163}, {133, 165}, {134, 166}, {135, 164}, {136, 113}, {137, 115}, {138, 114},
    {143, 172}, {144, 158},
    {1000, 580},   // not a QKeyCode: the app's Recents button (KEY_APPSELECT)
};
API void droid_key(int qcode, bool down) {
    for (size_t i = 0; i < sizeof keymap / sizeof *keymap; i++)
        if (keymap[i].q == qcode) { input_key(keymap[i].k, down); return; }
}

// Power: a press of the power key (Android shuts down or sleeps by itself). Reset and pause aren't
// offered by this engine.
API void droid_powerdown(void) { input_key(116, true); input_key(116, false); }
API void droid_reset(void) {}
API void droid_set_running(bool running) { (void)running; }
API bool droid_is_running(void) { return true; }
