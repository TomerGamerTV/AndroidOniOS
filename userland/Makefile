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
