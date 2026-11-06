# Android's real servicemanager under onirun, queried by the stock `service` tool over binder.
/system/bin/servicemanager &
sm=$!
fail=0
i=0; while [ $i -lt 50 ] && ! service check manager >/dev/null 2>&1; do sleep 0.1; i=$((i+1)); done
list=$(service list 2>/dev/null)
case "$list" in *"manager: [android.os.IServiceManager]"*) ;; *) echo "FAIL list: $list"; fail=1;; esac
# IServiceManager::getService("manager") as a raw transaction: a reply parcel carrying a binder.
call=$(service call manager 1 s16 manager 2>/dev/null)
case "$call" in "Result: Parcel("*) ;; *) echo "FAIL call: $call"; fail=1;; esac
case "$(service check nothing 2>/dev/null)" in *"not found"*) ;; *) echo "FAIL check"; fail=1;; esac
kill $sm
wait $sm 2>/dev/null
[ $fail = 0 ] && echo "SERVICE OK" || echo "SERVICE FAILED"
