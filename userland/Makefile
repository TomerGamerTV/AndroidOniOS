# onirun: macOS (Apple silicon) prototype of the user-mode Android runtime.
OUT := ../build/userland
SRC := src/main.c src/loader.c src/translate.c src/syscall.c src/cpu.S
CFLAGS := -O2 -g -Wall -Wno-unused-function -std=gnu11

$(OUT)/onirun: $(SRC) src/onx.h
	@mkdir -p $(OUT)
	clang $(CFLAGS) -o $@ $(SRC)
	codesign -s - -f $@

clean:
	rm -f $(OUT)/onirun

# Android test programs (needs the NDK toolchain in ../build/ndk-dl)
NDK_CC := ../build/ndk-dl/android-ndk-r27d/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang
TEST_OUT := ../build/userland/rootfs/data/local/tmp
TESTS := $(patsubst tests/%.c,$(TEST_OUT)/%,$(wildcard tests/*.c))
tests: $(TESTS)
$(TEST_OUT)/%: tests/%.c
	@mkdir -p $(TEST_OUT)
	$(NDK_CC) -O2 -Wall -Wl,-z,max-page-size=16384 -o $@ $<
