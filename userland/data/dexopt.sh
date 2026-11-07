# Compiles the apps once, after the first boot under onirun (installed as /oni/dexopt.sh, run by
# init-lite). The image's prebuilt app odex files are rejected (built for the userfaultfd GC; ART
# here runs the read-barrier collector), so apps start interpreted and miss their startup
# deadlines (ANRs). bg-dexopt verifies everything; the packages that run from boot are compiled
# fully ("speed": no profiles exist yet, and speed-profile without one only verifies). The
# results live in /data and survive later boots; the marker keeps this from running twice.
until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 10; done
[ -e /data/misc/oni-dexopt-done ] && exit 0
sleep 60                                    # let the first round of app starts settle
pm art dexopt-packages -r bg-dexopt
for p in com.android.systemui com.google.android.apps.nexuslauncher com.android.phone com.google.android.gms \
         com.google.android.inputmethod.latin com.google.android.permissioncontroller com.google.android.ext.services \
         com.google.android.networkstack com.android.settings com.google.android.as com.google.android.nfc \
         com.android.providers.contacts com.google.android.bluetooth com.android.vending com.google.android.googlequicksearchbox \
         com.android.se com.android.keychain com.android.printspooler com.android.localtransport; do
    pm compile -m speed "$p"          # already-compiled packages are skipped (no -f)
done
touch /data/misc/oni-dexopt-done
