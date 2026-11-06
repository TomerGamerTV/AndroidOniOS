# onirun: Android userland without a virtual machine (prototype)

`onirun` runs Android's own arm64 programs directly on an Apple arm64 host, with no virtual machine and no Linux kernel. It's the first step toward running Oni this way on iOS. Today it runs on macOS.

## How it works

- **Block translator** (`src/translate.c`, `src/cpu.S`). Android code is translated in blocks into a JIT cache. Most instructions are copied unchanged. The rewrites:
  - **Branches** are chained directly between blocks. Returns and other indirect branches go through an inline lookup table.
  - **Memory accesses** get the process's window base added (see below): `ldr x0, [x1, #8]` becomes `add x17, x16, x1; ldr x0, [x17, #8]`. Accesses through `sp` run unchanged, because the host `sp` holds the guest stack pointer as a host address.
  - **Instructions that read or write `sp` as a value** subtract or add the window base.
  - **Registers the host takes over**, so the guest's copies live in the thread context:
    - `x16` holds the window base.
    - `x17` is scratch.
    - `x18`: XNU zeroes it on every context switch.

    Instructions that name these registers are rewritten around a substitute register.
  - **The TLS register `TPIDR_EL0`**: XNU overwrites it with the CPU number, so its value is kept in the context too.
  - **EL1 system registers** that macOS doesn't let user code read.
  - **PC-relative loads, `svc`, `dc zva`**.
- **Address spaces** (`src/mm.c`).
  - **Windows:** each Android process gets its own 64 GiB window of host address space. Guest address `g` is host address `base + g`, so every process sees the same addresses a Linux process would. The window is reserved `PROT_NONE`, which makes null and wild pointers fault.
  - **`fork`** copies the window with `mach_vm_remap`. Private memory becomes copy-on-write and `MAP_SHARED` memory stays shared. Forking a 64 MiB process takes about 1 ms.
- **Processes** (`src/proc.c`). Every guest process and thread lives inside the one onirun host process. iOS has no `fork()`, and onirun never calls it.
  - **Threads:** guest threads are host pthreads.
  - **Per-process state:** each process has its own pid, file-descriptor table, signal actions, working directory and translation cache.
  - **Lifecycle:** `exec` replaces the window, and `wait`, `kill`, `exit_group` and `vfork` are implemented here.
  - **Stopping threads:** a thread that must stop is pulled out of translated code with a signal, even if it's in a tight loop.
- **Linux system calls** (`src/syscall.c`).
  - **Paths:** a guest filesystem rooted at `--root`, with Android's absolute symlinks resolved inside it, plus `/proc/self/exe` and `/proc/self/fd`.
  - **Signals:** Linux arm64 signal frames and `rt_sigreturn`.
  - **Futexes:** built on `os_sync_wait_on_address`, which iOS also has.
  - **Translation:** Linux flags, errno values and structs are converted to and from Darwin's.
- **Loader** (`src/loader.c`). It maps the program and Android's dynamic linker (`linker64`) into the window the way Linux `execve` does, with the same stack layout and auxv.

## Status

Android 17's `/system/bin/sh` (mksh) and toybox work:
- scripts, pipes, subshells, command substitution, background jobs, `wait`, `trap`
- external commands using Android's libc, libcrypto and libselinux
- multithreaded programs: mutexes, condition variables, thread-directed signals

They all run as many guest processes inside one host process.

Speed on an M1 Mac:

| Test | onirun | Same binary native (HVF VM) |
|---|---|---|
| `sha256sum` of 100 MB | 0.10 s | 0.09 s |
| Shell loop, 200k iterations | 0.82 s | 0.43 s |
| 1,000 × `$(echo \| cat)` (2,000 processes) | 13.3 s | — |

Rebasing every memory access costs about 8% on the branch-heavy shell loop. Process creation is faster than the earlier version that used the host's `fork()` (13.3 s vs 20.8 s for the last test). Most of the remaining process cost is translating `linker64` and libc again in every new program.

Under QEMU TCG (how the iPhone runs Android today), the first two tests used 2.3 s and 61 s of CPU time. Android was still busy after booting during that measurement, so treat it as approximate; the gap is still 20–60×.

Limits:
- **Memory isolation:** windows stop correct programs from seeing each other, but a wild 64-bit pointer isn't masked. One process could still corrupt another, as in a single address space.
- **Signals and faults:**
  - A signal with a handler reaches a thread at its next system call or block exit. A thread that never makes another call won't get it. Default-fatal signals and `SIGKILL` work regardless.
  - Guest `SIGSEGV` handlers aren't called yet: a crashing process is killed. ART will need this.

Not done yet:
- **Binder, ashmem and properties**, then ART and the Android framework.
- **Sharing translations between processes**, so each new program doesn't translate libc and the linker again.
- **iOS.** Executable memory there comes from the JIT region StikJIT enables. The window size must also fit iOS's address-space limit.

## Try it

```sh
make -C userland
# Root filesystem: copy /system and the runtime APEX out of a running Android 17 image, e.g.
#   adb exec-out 'tar -cf - -C / system/bin system/lib64' | tar -xf - -C build/userland/rootfs
#   plus /apex/com.android.runtime and /linkerconfig/ld.config.txt
build/userland/onirun --root build/userland/rootfs /system/bin/sh -c 'uname -a; ls /system/bin | wc -l'
# Tests (needs the Android NDK in build/ndk-dl): each prints one "... OK" line
make -C userland check
```
