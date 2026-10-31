#!/bin/sh
# SPDX-License-Identifier: MIT
# Restart the demo N times (5 unless you say), every core each time, and
# check each run; then the kernel log for stops that timed out.
. "$(dirname "$0")/lib.sh"

RUNS=${1:-5}
DMESG_FROM=$(dmesg | wc -l)
i=1
while [ "$i" -le "$RUNS" ]; do
	echo "run $i of $RUNS"
	start_demo && check_demo
	i=$((i + 1))
done
echo "the kernel log since the first run"
n=$(dmesg | tail -n +"$((DMESG_FROM + 1))" | grep -c -i -E "can't stop|timed out|oops|BUG:|-ETIMEDOUT")
[ "$n" -eq 0 ] && ok "no stop timeouts, oopses, or BUGs" || {
	bad "$n kernel lines:"
	dmesg | tail -n +"$((DMESG_FROM + 1))" | grep -i -E "can't stop|timed out|oops|BUG:|-ETIMEDOUT" | tail -n 5
}
finish
