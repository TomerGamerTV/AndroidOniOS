#!/bin/bash
# Wraps the iOS QEMU dylibs (JIT + TCTI) and MoltenVK as frameworks under build/frameworks
# so the app target can embed and sign them.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/frameworks"
Q="$ROOT/build/src/qemu-10.0.12-utm"
mkdir -p "$OUT"

make_fw() { # name dylib
    local name="$1" lib="$2" fw="$OUT/$1.framework"
    rm -rf "$fw"; mkdir -p "$fw"
    cp "$lib" "$fw/$name"
    install_name_tool -id "@rpath/$name.framework/$name" "$fw/$name"
    strip -x "$fw/$name" 2>/dev/null || true
    cat > "$fw/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>$name</string>
<key>CFBundleIdentifier</key><string>com.tomer.androidonios.$name</string>
<key>CFBundleName</key><string>$name</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleShortVersionString</key><string>10.0.12</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
<key>MinimumOSVersion</key><string>26.0</string>
</dict></plist>
EOF
    echo "packaged $name ($(du -sh "$fw" | cut -f1))"
}

make_fw QEMUJIT "$Q/build-ios-jit/libqemu-aarch64-softmmu.dylib"
make_fw QEMUTCTI "$Q/build-ios-tcti/libqemu-aarch64-softmmu.dylib"
rm -rf "$OUT/MoltenVK.framework"
cp -R "$ROOT/build/utm/ipa/Payload/UTM.app/Frameworks/MoltenVK.framework" "$OUT/"
rm -rf "$OUT/MoltenVK.framework/_CodeSignature"
echo "packaged MoltenVK"
