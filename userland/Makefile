# onirun: macOS (Apple silicon) prototype of the user-mode Android runtime.
OUT := ../build/userland
SRC := src/main.c src/loader.c src/translate.c src/syscall.c src/proc.c src/mm.c src/vfs.c src/sock.c src/props.c src/binder.c src/procfs.c src/init.c src/virtgpu.c src/cpu.S
CFLAGS := -O2 -g -Wall -Wno-unused-function -std=gnu11 -fblocks -I../build/sysroot-macos/include -Isrc/uapi
# The GPU is the host's gfxstream/rutabaga (the QEMU app's libraries, built into build/sysroot-macos).
GFXLIBS := ../build/sysroot-macos/lib/librutabaga_gfx_ffi.a ../build/sysroot-macos/lib/libgfxstream_backend.a
FRAMEWORKS := -framework Foundation -framework CoreFoundation -framework AppKit -framework Cocoa -framework CoreVideo \
              -framework IOKit -framework IOSurface -framework Metal -framework QuartzCore -framework OpenGL
LDLIBS := $(GFXLIBS) $(FRAMEWORKS) -lc++ -lz

$(OUT)/onirun: $(SRC) src/onx.h src/sys.h
	@mkdir -p $(OUT)
	clang $(CFLAGS) -o $@ $(SRC) $(LDLIBS)
	codesign -s - -f $@

clean:
	rm -f $(OUT)/onirun

# Android test programs (needs the NDK toolchain in ../build/ndk-dl)
NDK_CC := ../build/ndk-dl/android-ndk-r27d/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang
TEST_OUT := ../build/userland/rootfs/data/local/tmp
TESTS := $(patsubst tests/%.c,$(TEST_OUT)/%,$(wildcard tests/*.c)) \
         $(patsubst tests/%.sh,$(TEST_OUT)/%.sh,$(wildcard tests/*.sh)) \
         $(TEST_OUT)/hello.dex $(TEST_OUT)/bench.dex $(TEST_OUT)/android-env.sh
tests: $(TESTS)
$(TEST_OUT)/hello.dex: ../tools/mkdex.py
	python3 -I $< $@ hello
$(TEST_OUT)/bench.dex: ../tools/mkdex.py
	python3 -I $< $@ bench
$(TEST_OUT)/android-env.sh: data/android-env.sh
	cp $< $@
$(TEST_OUT)/%: tests/%.c
	@mkdir -p $(TEST_OUT)
	$(NDK_CC) -O2 -Wall -Wno-inline-asm -Wl,-z,max-page-size=16384 -o $@ $<
$(TEST_OUT)/%.sh: tests/%.sh
	@mkdir -p $(TEST_OUT)
	cp $< $@

# Runs every test in the rootfs; each prints one "... OK" line.
RUN := $(OUT)/onirun --root $(OUT)/rootfs
check: $(OUT)/onirun tests
	@for t in threads procs blocked rewrite ipc binder; do $(RUN) /data/local/tmp/$$t 2>/dev/null | tail -1; done
	@$(RUN) /system/bin/sh /data/local/tmp/shell.sh 2>/dev/null | tail -3
	@$(RUN) /system/bin/sh /data/local/tmp/service.sh 2>/dev/null | tail -3
	@$(RUN) /system/bin/sh /data/local/tmp/art.sh 2>/dev/null | tail -3
