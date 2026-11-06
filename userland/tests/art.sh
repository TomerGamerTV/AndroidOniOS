# ART (Android's Java runtime) under onirun: two hand-written dex programs, interpreter and JIT.
. /data/local/tmp/android-env.sh
fail=0
VM="/apex/com.android.art/bin/dalvikvm64 -Xnoimage-dex2oat"
out=$($VM -Xusejit:false -cp /data/local/tmp/hello.dex Hello 2>/dev/null)
want="Hello from ART under onirun
strings and classes resolve
caught the NullPointerException"
[ "$out" = "$want" ] || { echo "FAIL hello (interpreter): $out"; fail=1; }
out=$($VM -cp /data/local/tmp/hello.dex Hello 2>/dev/null)
[ "$out" = "$want" ] || { echo "FAIL hello (jit): $out"; fail=1; }
out=$($VM -cp /data/local/tmp/bench.dex Bench 2>/dev/null)
[ "$out" = "78927" ] || { echo "FAIL bench (jit): $out"; fail=1; }
[ $fail = 0 ] && echo "ART OK" || echo "ART FAILED"
