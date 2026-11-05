# AndroidOniOS

**Oni** runs Android 17 with Google Play inside an iOS app. It boots straight into Android, and it's built to stay usable rather than be a tech demo.

> Status: experimental. Developed and tested on an iPhone 18 Pro Max running iOS 27. Expect rough edges, high battery use and heat.

## Features

- **Android 17 with Google Play.** The official Android Emulator system image, downloaded from Google on first launch. It's never shipped with the app.
- **Fast CPU emulation with JIT.** QEMU's code translator runs with JIT, which the bundled StikJIT helper enables through a loopback VPN. Without JIT, Oni falls back to a slower interpreter (TCTI).
- **GPU acceleration.** Android's Vulkan and GLES rendering go through gfxstream to Vulkan, then MoltenVK, then Metal. App UI renders with Skia on Vulkan.
- **Quick resume.** When you leave the app, Android's whole state (memory, devices and the GPU) is saved. The next launch continues where you left off instead of booting again.
- **Sound**, multitouch and hardware keyboards.
- **iPhone sensors and GPS** passed through to Android.
- **Shared folder.** Android's shared storage is mirrored to Files › On My iPhone › Oni › Android Storage.
- **Install APKs from the share sheet.** Share an `.apk` to Oni to install it.

## How it works

```
 iOS app (Swift, UIKit/SwiftUI)
   ├─ QEMU 10 (UTM fork) as a framework, running an arm64 "virt" machine
   │    ├─ TCG with JIT (iOS 26+ JIT protocol via StikJIT), or TCTI without JIT
   │    ├─ virtio: block, net (slirp), input, sound, serial, vsock
   │    └─ virtio-gpu (rutabaga) ─ gfxstream ─ Vulkan ─ MoltenVK ─ Metal
   ├─ Host services over vsock: ADB, sensors, GPS, boot properties
   └─ Loopback VPN + StikJIT extension to enable JIT on device
```

Our changes to third-party code are kept as patch sets in `patches/` (QEMU, gfxstream, rutabaga), plus new QEMU source files in `qemu-src/`.

## Repository layout

| Path | Contents |
|---|---|
| `app/` | The iOS app, its extensions (share, JIT helper, VPN tunnel) and the XcodeGen spec |
| `qemu-src/` | New QEMU sources: the app's display/input/save-state bridge, a userspace vsock device, iOS audio |
| `patches/` | Patches for QEMU, gfxstream and rutabaga_gfx |
| `scripts/` | Build scripts, a macOS development harness, and device helper scripts |
| `config/`, `tools/` | Android boot configuration, and helpers for the macOS harness |

## Building

You need an Apple Silicon Mac with Xcode, Rust (`rustup`), Python 3, Meson and Ninja, and an Apple Developer Program membership. Oni uses the increased-memory-limit, extended-virtual-addressing and network-extension entitlements.

1. Build the dependencies for iOS, in this order:
   ```sh
   scripts/build-deps.sh ios
   scripts/build-gfxstream.sh ios
   scripts/build-rutabaga.sh ios
   scripts/build-qemu.sh ios-jit
   scripts/build-qemu.sh ios-tcti
   scripts/package-frameworks.sh
   ```
   Some inputs aren't fetched automatically yet:
   - **UTM source:** at `build/src/UTM`, for its libslirp patch.
   - **UTM release IPA:** extracted to `build/utm/ipa`, for `MoltenVK.framework`.
   - **StikJIT source:** at `build/src/StikJIT`.
   - **XcodeGen:** at `build/tools/bin/xcodegen`.
2. Generate the Xcode project with your team ID, then build:
   ```sh
   export DEVELOPMENT_TEAM=<your Apple team ID>
   cd app && ../build/tools/bin/xcodegen generate
   xcodebuild -project AndroidOniOS.xcodeproj -scheme Droid -configuration Release \
     -destination 'generic/platform=iOS' -allowProvisioningUpdates build
   ```
3. To use JIT, connect the iPhone over USB, then create the pairing file StikJIT needs and copy it into the app:
   ```sh
   scripts/make-pairing.sh <iPhone UDID>
   ```

### macOS harness

`scripts/run-macos.sh` boots the same Android image with a macOS build of QEMU (`scripts/build-deps.sh macos`, `build-gfxstream.sh macos`, `build-rutabaga.sh macos`, `build-qemu.sh macos`). It uses Hypervisor.framework, so it's the fast way to test device and graphics changes. Set `ACCEL=tcg` to use the same CPU emulation as the iPhone.

## Experimental: Android without a virtual machine

`userland/` holds **onirun**, a prototype that runs Android's own arm64 programs directly on the host. There's no virtual machine and no Linux kernel: a block translator copies Android code almost unchanged, and a Linux system-call layer runs on Darwin. It currently runs Android's shell and toybox on macOS. See [userland/README.md](userland/README.md).

## Credits and licenses

See [NOTICE.md](NOTICE.md). QEMU is GPL-2.0. gfxstream, rutabaga_gfx and MoltenVK are Apache-2.0 / BSD-3-Clause. The JIT and loopback VPN approach builds on StikJIT/StikDebug and the SideStore team's StosVPN and LocalDevVPN.

Android, Google Play and the system image are Google's. Oni downloads the image directly from Google under the Android SDK license, and this repository doesn't redistribute it.
