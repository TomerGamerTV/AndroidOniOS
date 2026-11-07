# Compiles every app once, after the first boot under onirun (installed as /oni/dexopt.sh, run by
# init-lite). The image's app odex files were built against a boot image it doesn't ship, so apps
# start interpreted and miss their startup deadlines (ANRs) until ART compiles them. The results
# live in /data and survive later boots; the marker keeps it from running twice.
until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 10; done
[ -e /data/misc/oni-dexopt-done ] && exit 0
sleep 60                                    # let the first round of app starts settle
pm art dexopt-packages -r bg-dexopt && touch /data/misc/oni-dexopt-done
