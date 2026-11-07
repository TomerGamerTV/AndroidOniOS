# onirun: macOS (Apple silicon) prototype of the user-mode Android runtime.
OUT := ../build/userland
SRC := src/main.c src/loader.c src/translate.c src/syscall.c src/proc.c src/mm.c src/vfs.c src/sock.c src/props.c src/binder.c src/procfs.c src/init.c src/virtgpu.c src/goldfish.c src/bpf.c src/rootfs_install.c src/input.c src/display.m src/cpu.S
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

# iOS: Onirun.framework for the Droid app (loaded like its QEMU frameworks; entry points in
# src/ios_api.c). The GPU is the iOS gfxstream/rutabaga build (build/sysroot-ios).
IOS_SDK = $(shell xcrun --sdk iphoneos --show-sdk-path)
IOS_SRC := $(filter-out src/main.c src/display.m,$(SRC)) src/main.c src/ios_api.c
IOS_FW := ../build/frameworks/Onirun.framework
IOS_LIBS := ../build/sysroot-ios/lib/librutabaga_gfx_ffi.a ../build/sysroot-ios/lib/libgfxstream_backend.a \
            -framework Foundation -framework CoreFoundation -framework Metal -framework IOSurface -framework QuartzCore \
            -framework IOKit -lc++ -lz
ios: $(IOS_FW)/Onirun
$(IOS_FW)/Onirun: $(IOS_SRC) src/onx.h src/sys.h
	@mkdir -p $(IOS_FW)
	clang -target arm64-apple-ios26.0 -isysroot $(IOS_SDK) -O2 -g -Wall -Wno-unused-function -std=gnu11 -fblocks \
	    -DONIRUN_LIB -I../build/sysroot-ios/include -Isrc/uapi -dynamiclib -install_name @rpath/Onirun.framework/Onirun \
	    -o $@ $(IOS_SRC) $(IOS_LIBS)
	rm -rf $(OUT)/Onirun.dSYM; mv $(IOS_FW)/Onirun.dSYM $(OUT)/ 2>/dev/null || true   # symbols stay out of the app
	sed -e 's/@NAME@/Onirun/g' data/framework-Info.plist > $(IOS_FW)/Info.plist

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

# Guest-side stand-in services (userland/stubs), built with the NDK against the image's libraries
# and installed into the rootfs by mkrootfs.py (or by hand: cp to rootfs/system/bin/).
STUB_OUT := ../build/userland/stubs
ROOTFS ?= ../build/userland/rootfs
stubs: $(STUB_OUT)/onistub
$(STUB_OUT)/onistub: stubs/stubsvc.c
	@mkdir -p $(STUB_OUT)
	$(NDK_CC) -O2 -Wall -Wl,-z,max-page-size=16384 -L$(ROOTFS)/system/lib64 -Wl,--allow-shlib-undefined -o $@ $< -lbinder_ndk -llog
	cp $@ $(ROOTFS)/system/bin/onistub
