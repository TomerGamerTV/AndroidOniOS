# onirun: Android userland without a virtual machine (prototype)

`onirun` runs Android's own arm64 programs directly on an Apple arm64 host, with no virtual machine and no Linux kernel. It's the first step toward running Oni this way on iOS. Today it runs on macOS.

## How it works

- **Block translator** (`src/translate.c`, `src/cpu.S`). Android code is translated in blocks into a JIT cache. Nearly every instruction is copied unchanged. Only these are rewritten:
  - branches, chained directly between blocks, with an inline lookup table for returns and other indirect branches
  - PC-relative address loads
  - `svc`, which exits to the system-call layer
  - the TLS register `TPIDR_EL0` and `x18`. XNU overwrites TPIDR_EL0 with the CPU number and zeroes x18 on every context switch, so both are kept in the thread context instead.
  - EL1 system registers that macOS doesn't let user code read
- **Linux system calls** (`src/syscall.c`):
  - **Filesystem:** a guest filesystem rooted at `--root`, with Android's absolute symlinks resolved inside it, plus `/proc/self/exe` and `/proc/self/fd`.
  - **Memory:** `mmap` that never maps guest code executable, because the translator only reads it.
  - **Processes:** `fork`/`exec`/`wait`.
  - **Signals:** Linux arm64 signal frames and `rt_sigreturn`.
  - **Translation:** Linux flags, errno values and structs are converted to and from Darwin's.
- **Loader** (`src/loader.c`). It maps the program and Android's dynamic linker (`linker64`) the way Linux `execve` does, with the same stack layout and auxv.

## Status

Android 17's `/system/bin/sh` (mksh) and toybox work: scripts, pipes, subshells, background jobs, and external commands with Android's libc, libcrypto and libselinux.

Speed on an M1 Mac:

| Test | onirun | Same binary native (HVF VM) |
|---|---|---|
| `sha256sum` of 100 MB | 0.12 s | 0.09 s |
| Shell loop, 200k iterations | 0.92 s | 0.43 s |

Under QEMU TCG (how the iPhone runs Android today), the same two tests used 2.3 s and 61 s of CPU time. Android was still busy after booting during that measurement, so treat it as approximate; the gap is still 20–60×.

Not done yet:
- **Threads.** `clone` with `CLONE_THREAD` isn't supported.
- **Several Android processes in one host process.** `fork` currently uses the host's `fork()`, which iOS doesn't allow. This is the next big step: each Android process gets its own sandboxed memory region, and memory accesses are rewritten to stay inside it.
- **Binder, ashmem and properties**, then ART and the Android framework.
- **iOS.** Executable memory there comes from the JIT region StikJIT enables.

## Try it

```sh
make -C userland
# Root filesystem: copy /system and the runtime APEX out of a running Android 17 image, e.g.
#   adb exec-out 'tar -cf - -C / system/bin system/lib64' | tar -xf - -C build/userland/rootfs
#   plus /apex/com.android.runtime and /linkerconfig/ld.config.txt
build/userland/onirun --root build/userland/rootfs /system/bin/sh -c 'uname -a; ls /system/bin | wc -l'
```
