# Third-party components and credits

- **QEMU** (UTM fork, v10.0.12-utm) — GPL-2.0. Patched; see `patches/qemu/` and `qemu-src/`.
- **gfxstream** and **rutabaga_gfx** (Google / ChromiumOS) — Apache-2.0 / BSD-3-Clause. Patched; see `patches/gfxstream/` and `patches/rutabaga/`.
- **MoltenVK** (Khronos Group) — Apache-2.0.
- **ANGLE** (Google) — BSD-3-Clause, as shipped in UTM's frameworks.
- **StikJIT** / **StikDebug** (StephenDev0 and contributors) — hosts the on-device JIT enabler in the `DroidJIT` extension.
- **idevice** (Jackson Coxson) — RemotePairing/RSD client used by StikJIT and by `scripts/make-pairing.sh`.
- **Loopback VPN** — `app/DroidTunnel` follows the design of **StosVPN** and **LocalDevVPN** by the **SideStore Team** (https://github.com/SideStore/StosVPN, https://github.com/StephenDev0/LocalDevVPN). Credit for that approach belongs to the original authors.
- **Android Emulator system image** (Google Play, Android 17) — downloaded by the user under Google's Android SDK license terms. It is not redistributed with this project.
