#!/bin/sh
# SPDX-License-Identifier: MIT
# The logic analyzer against a fast test signal: R5F0_1's EHRPWM4 on P9.25,
# jumpered to ch12 (PRU1), at 50% duty and ever shorter periods, from the
# demo's rates to past what PRU1 can record. At each, ai64 la rate: RTU1
# copies every record PRU1 writes, and a reader takes every one. Then raw
# mode (ai64 la mode): PRU1 sampling ch12 every 8 ns, PRU0 finding the
# changes, at 10 million a second with none lost; and slice 1's hardware
# dead band, 24 ns, read exactly. Then edge mode, EHRPWM4 steps again, and
# the pipeline follows again. Past what edge mode keeps up with, it says so
# with late edges; past raw mode, with holes, which no measurement spans.
# The demo must be running.
. "$(dirname "$0")/lib.sh"

# rate PERIOD_NS MIN_RATE: EHRPWM4 at that period; PRU1's records a second
# as RTU1 copies them at least MIN_RATE, RTU1 with no new gaps, and the
# reader missing none.
rate() {
	local p=$1 want=$2 out copied missed gaps0 gaps stream
	gaps0=$("$AI64" la rate 0.1 | sed -n 's/^PRU1:.* RTU gaps \([0-9]*\) .*/\1/p')
	"$AI64" fabric route r5f0_1 pwm "$p" $((p / 2)) >/dev/null 2>&1
	sleep 1
	out=$("$AI64" la rate 1 | grep '^PRU1:')
	copied=$(echo "$out" | sed -n 's/.* copied \([0-9]*\) records.*/\1/p')
	missed=$(echo "$out" | sed -n 's/.* missed \([0-9]*\)\/s.*/\1/p')
	gaps=$(echo "$out" | sed -n 's/.* RTU gaps \([0-9]*\) .*/\1/p')
	stream=$(echo "$out" | sed -n 's/.* gaps in the stream \([0-9]*\);.*/\1/p')
	[ -n "$copied" ] && [ -n "$gaps" ] && [ "$copied" -ge "$want" ] && [ "$missed" = 0 ] &&
		[ "$gaps" = "$gaps0" ] && [ "$stream" = 0 ] &&
		ok "EHRPWM4 at $p ns: PRU1 $copied records/s, none lost" ||
		bad "EHRPWM4 at $p ns: $out"
	echo "$out" | sed -n 's/.* closest records \(-*[0-9]*\) ns.*/\1/p' >"$HERE/closest"
	echo "$out" | sed -n 's/.* late \([0-9]*\)\/s.*/\1/p' >"$HERE/late"
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
n=$(cat "$HERE/late")
[ -n "$n" ] && [ "$n" -ge 1000000 ] && ok "edge mode says it is overloaded: $n late edges/s" ||
	bad "edge mode overloaded, but only ${n:-no} late edges/s"

echo "stress: raw mode, ch12 every 8 ns"
check "ai64 la mode raw 12" "$AI64" la mode raw 12
"$AI64" la mode | grep -q '^PRU1: raw' && ok "PRU1 is raw, PRU0 converts" || bad "ai64 la mode: $("$AI64" la mode)"
rate 400 4950000
rate 200 9900000

echo "stress: raw mode past what it keeps up with: EHRPWM4 at 62.5 MHz, its Nyquist rate"
"$AI64" fabric route r5f0_1 pwm 16 8 >/dev/null 2>&1
sleep 1
out=$("$AI64" la measure 0.2 2>&1)
echo "$out" | grep -q '^PRU1: [0-9]* holes' && ok "the holes are marked: $(echo "$out" | grep '^PRU1: [0-9]* holes' | cut -d, -f1)" ||
	bad "no holes marked at 62.5 MHz"
n=$(echo "$out" | grep ' P8.35 ' | tr -s ' ')
echo "$n" | grep -q ' MHz 16 16 16 8 8 8 ' && ok "ch12 between them: a period of 16 ns, high 8, every one" ||
	bad "ch12 at 62.5 MHz: $n"

echo "stress: raw mode, slice 1's dead band at 24 ns"
"$AI64" la mode raw 9 10 >/dev/null 2>&1
"$AI64" fabric route g0rtu1 pwm 50000 25000 >/dev/null 2>&1
"$AI64" fabric route g0rtu1 dead 24 >/dev/null 2>&1
sleep 1
n=$("$AI64" la measure 0.5 9:10 2>&1 | grep -A2 'ch9 S1_A -> ch10 S1_B' | tr -s ' ' | tr '\n' ' ')
echo "$n" | grep -q ' 24 24 24 *$' && ok "ch9 to ch10: 24 ns, every one" || bad "ch9 to ch10: $n"
"$AI64" fabric route g0rtu1 dead 1000 >/dev/null 2>&1
check "ai64 la mode edge" "$AI64" la mode edge
"$AI64" la mode | grep -q '^PRU0: edge' && "$AI64" la mode | grep -q '^PRU1: edge' &&
	ok "both PRUs in edge mode again" || bad "ai64 la mode: $("$AI64" la mode)"

echo "putting the steps back"
check "R5F0_1 steps again" "$AI64" fabric route r5f0_1 steps 5
check "RTU1 steps again" "$AI64" fabric route g0rtu1 steps 5
check "the pipeline starts again" "$AI64" pipe start
finish
