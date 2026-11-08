import Foundation

/// Guest tweaks applied over ADB each time Android finishes booting.
enum GuestSetup {
    /// Android's clock stands still while the VM is paused or saved; set it to the real time.
    /// (The time detector takes this like a network-provided time, so automatic time keeps working.)
    static func syncClock() {
        DispatchQueue.global(qos: .utility).async {
            guard ADBClient.shared.connect(timeout: 60) else { return }
            let now = Int64(Date().timeIntervalSince1970 * 1000)
            let cmd = "er=$(awk '{printf \"%d\", $1*1000}' /proc/uptime); "
                + "cmd time_detector suggest_telephony_time --slot_index 0 "
                + "--elapsed_realtime $er --unix_epoch_time \(now) >/dev/null 2>&1; date"
            let out = (try? ADBClient.shared.shell(cmd)) ?? "(adb error)"
            Log.info("guest clock: \(out.trimmingCharacters(in: .whitespacesAndNewlines))")
        }
    }

    static func apply(_ settings: VMSettings) {
        DispatchQueue.global(qos: .utility).async {
            // iOS keeps the bottom-edge swipe for its own home gesture, so Android's on-screen
            // buttons are the reliable default; gesture navigation is available in Settings.
            let nav = settings.navButtons
                ? "com.android.internal.systemui.navbar.threebutton"
                : "com.android.internal.systemui.navbar.gestural"
            let commands = [
                "cmd overlay enable-exclusive --category \(nav)",
                // The VM has no Bluetooth or UWB hardware. Leaving them on makes their HALs retry
                // forever (constant CPU use and log spam), so switch them off. (NFC idles; the
                // shell user isn't allowed to switch it off anyway.)
                "cmd bluetooth_manager disable",
                "settings put global ble_scan_always_enabled 0",
                "cmd uwb disable-uwb",
                // One-time performance defaults (the user can change them back in Android
                // Settings): shorter animations, and no Digital Wellbeing or setup-only restore
                // tool, which keep the emulated CPUs busy in the background.
                "[ -f /data/local/tmp/.droid-tuned-1 ] || { "
                    + "settings put global window_animation_scale 0.5; "
                    + "settings put global transition_animation_scale 0.5; "
                    + "settings put global animator_duration_scale 0.5; "
                    + "pm disable-user --user 0 com.google.android.apps.wellbeing; "
                    + "pm disable-user --user 0 com.google.android.apps.restore; "
                    + "touch /data/local/tmp/.droid-tuned-1; }",
            ]
            for c in commands {
                let out = (try? ADBClient.shared.shell(c + " 2>&1")) ?? "(adb error)"
                let short = out.trimmingCharacters(in: .whitespacesAndNewlines).prefix(120)
                Log.info("guest setup: \(c)\(short.isEmpty ? "" : " -> \(short)")")
            }
        }
    }
}
