# Debug shell for the host (installed as /oni/shell.sh, run by init-lite as root): the rootfs is a
# host directory, so the host writes a script to /data/local/tmp/oni-cmd and reads the result from
# oni-cmd.out once oni-cmd is gone. userland/oni-shell.sh wraps this. Like `adb shell` without adbd.
cd /data/local/tmp
while true; do
    if [ -f oni-cmd ]; then
        mv oni-cmd oni-cmd.run
        sh oni-cmd.run > oni-cmd.tmp 2>&1
        mv oni-cmd.tmp oni-cmd.out
        rm -f oni-cmd.run
    fi
    sleep 1
done
