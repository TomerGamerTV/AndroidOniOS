# Shell test for onirun: fork, exec, pipes, command substitution, wait, kill and traps in mksh.
fail=0
check() { if [ "$2" != "$3" ]; then echo "FAIL $1: got '$2' want '$3'"; fail=1; fi; }
check cmdsub "$(echo sub; echo stitution)" "sub
stitution"
check subshell "$( (echo inner) )" inner
check child-ppid "$(sh -c 'echo $PPID')" "$$"
check pipeline "$(ls /system/bin | head -3 | wc -l | tr -d ' ')" 3
check sort "$(seq 1 100000 | sort -rn | head -1)" 100000
check while-read "$(echo a b c | tr ' ' '\n' | while read l; do printf "<%s>" $l; done)" "<a><b><c>"
sleep 0.2 & bg=$!; wait $bg; check wait "$?" 0
sh -c 'exit 7'; check exit-status "$?" 7
sh -c 'kill -TERM $$'; check killed-status "$?" 143
false || ok=yes; check or "$ok" yes
f() { echo "func $1"; }; check function-pipe "$(f one | cat)" "func one"
trap 'got=usr1' USR1; kill -USR1 $$; check trap "$got" usr1
check sigpipe "$(yes | head -2 | tr -d '\n')" yy
check background-jobs "$( (sleep 0.1; echo b) & (echo a); wait )" "a
b"
[ $fail = 0 ] && echo "SHELL OK" || echo "SHELL FAILED"
