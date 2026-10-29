#!/bin/bash
cd "$(dirname "$0")"
fails=0
check() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails+1)); fi; }
OUT=/tmp/verify_out.txt; rm -f server_controller.log "$OUT"

./server_controller > "$OUT" 2>&1 & pid=$!
sleep 1
check "startup banner shows state NORMAL and the real PID"  "grep -q \"Started. PID=$pid, state=NORMAL\" $OUT"
kill -SIGUSR1 $pid; sleep 0.3
check "SIGUSR1 -> status report with state NORMAL and PID $pid"  "grep -q \"STATUS REPORT: state=NORMAL, PID=$pid\" $OUT"
kill -SIGUSR2 $pid; sleep 0.3
check "SIGUSR2 -> NORMAL -> MAINTENANCE"  "grep -q 'NORMAL -> MAINTENANCE' $OUT"
kill -SIGUSR1 $pid; sleep 0.3
check "status now reports MAINTENANCE"  "grep -q \"STATUS REPORT: state=MAINTENANCE, PID=$pid\" $OUT"
sleep 4.5
check "periodic message reflects MAINTENANCE state"  "grep -q 'Service in MAINTENANCE mode' $OUT"
kill -SIGUSR2 $pid; sleep 0.3
check "second SIGUSR2 -> MAINTENANCE -> NORMAL"  "grep -q 'MAINTENANCE -> NORMAL' $OUT"
kill -SIGUSR1 $pid; sleep 0.3
check "status back to NORMAL"  "[ \$(grep -c \"STATUS REPORT: state=NORMAL, PID=$pid\" $OUT) -eq 2 ]"
sleep 4.5
check "two periodic ticks so far"  "[ \$(grep -c -E 'Service running normally|Service in MAINTENANCE' $OUT) -eq 2 ]"
kill -SIGINT $pid; wait $pid; rc=$?
check "SIGINT -> exit status 0"  "[ $rc -eq 0 ]"
check "SIGINT -> shutdown message, summary and cleanup printed"  "grep -q 'Graceful shutdown' $OUT && grep -q 'Summary:' $OUT && grep -q 'Cleanup complete' $OUT"
check "process no longer exists"  "! kill -0 $pid 2>/dev/null"
gap=$(grep -E 'Service running normally|Service in MAINTENANCE' server_controller.log | awk '{split($1,t,":"); s=t[1]*3600+t[2]*60+t[3]; if (NR>1) printf "%.3f", s-prev; prev=s}')
check "tick spacing is 5.0 s +/- 0.1 s (measured: ${gap}s)"  "awk -v g=$gap 'BEGIN{exit !(g>4.9 && g<5.1)}'"

: > "$OUT"
./server_controller > "$OUT" 2>&1 & pid=$!
sleep 1; kill -SIGTERM $pid; wait $pid; rc=$?
check "SIGTERM -> exit status 143"  "[ $rc -eq 143 ]"
check "SIGTERM -> emergency message printed"  "grep -q 'EMERGENCY TERMINATION' $OUT"
check "SIGTERM -> no graceful summary / cleanup message"  "! grep -q -E 'Summary:|Cleanup complete' $OUT"

echo; [ $fails -eq 0 ] && echo "ALL CHECKS PASSED" || echo "$fails CHECK(S) FAILED"
exit $fails