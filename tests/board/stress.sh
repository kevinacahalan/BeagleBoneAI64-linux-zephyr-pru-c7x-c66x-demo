#!/bin/sh
# SPDX-License-Identifier: MIT
# The logic analyzer against a fast test signal: R5F0_1's EHRPWM4 on P9.25,
# jumpered to ch12 (PRU1), at 50% duty and ever shorter periods, from the
# demo's rates to past what PRU1 can record. At each, ai64 la rate: RTU1
# copies every record PRU1 writes, and a reader takes every one. Then
# EHRPWM4 steps again, and the pipeline follows again. The demo must be
# running.
. "$(dirname "$0")/lib.sh"

# rate PERIOD_NS MIN_RATE: EHRPWM4 at that period; PRU1's records a second
# as RTU1 copies them at least MIN_RATE, RTU1 with no new gaps, and the
# reader missing none.
rate() {
	local p=$1 want=$2 out copied missed gaps0 gaps
	gaps0=$("$AI64" la rate 0.1 | sed -n 's/^PRU1:.* RTU gaps \([0-9]*\) .*/\1/p')
	"$AI64" fabric route r5f0_1 pwm "$p" $((p / 2)) >/dev/null 2>&1
	sleep 1
	out=$("$AI64" la rate 1 | grep '^PRU1:')
	copied=$(echo "$out" | sed -n 's/.* copied \([0-9]*\) records.*/\1/p')
	missed=$(echo "$out" | sed -n 's/.* missed \([0-9]*\)\/s.*/\1/p')
	gaps=$(echo "$out" | sed -n 's/.* RTU gaps \([0-9]*\) .*/\1/p')
	[ -n "$copied" ] && [ -n "$gaps" ] && [ "$copied" -ge "$want" ] && [ "$missed" = 0 ] && [ "$gaps" = "$gaps0" ] &&
		ok "EHRPWM4 at $p ns: PRU1 $copied records/s, none lost" ||
		bad "EHRPWM4 at $p ns: $out"
	echo "$out" | sed -n 's/.* closest records \(-*[0-9]*\) ns.*/\1/p' >"$HERE/closest"
}

echo "stress: the logic analyzer against EHRPWM4 on ch12"
rate 2000 1000000
rate 400 5000000
rate 200 10000000
# Past PRU1: it records each change it sees, no faster than its loop.
rate 40 12000000
n=$(cat "$HERE/closest")
[ "$n" -ge 40 ] && [ "$n" -le 52 ] && ok "PRU1's closest records: $n ns apart" ||
	bad "PRU1's closest records: $n ns apart, not 40 to 52"

echo "putting the steps back"
check "R5F0_1 steps again" "$AI64" fabric route r5f0_1 steps 5
check "the pipeline starts again" "$AI64" pipe start
finish
