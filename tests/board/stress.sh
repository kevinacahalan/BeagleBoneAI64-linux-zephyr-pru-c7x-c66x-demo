#!/bin/sh
# SPDX-License-Identifier: MIT
# The logic analyzer against a fast test signal: R5F0_1's EHRPWM4 on P9.25,
# jumpered to ch7 (P9.11), at 50% duty and ever shorter periods, down to
# 16 ns, 62.5 MHz, the Nyquist rate at 125 MS/s. First ch7 alone at
# 125 MS/s, four samples a word, which the reader takes live: at each
# period every period and high time comes out exact, to the sample, and
# the reader keeps up with no block lost but PRU0's rare drops. Then every
# channel at 125 MS/s, more than the reader takes live: a capture still
# comes whole, read from the ring after (la_rec.c). The demo must be
# running.
. "$(dirname "$0")/lib.sh"

# exact PERIOD_NS: EHRPWM4 at that period; ch7's period and high time,
# min, median, and max, exactly PERIOD_NS and half of it, over 0.1 s.
exact() {
	local p=$1 out n
	"$AI64" fabric route r5f0_1 pwm "$p" $((p / 2)) >/dev/null 2>&1
	sleep 0.5
	out=$("$AI64" la measure 0.1 2>&1)
	n=$(echo "$out" | awk '$1 == 7 && $2 == "P9.11"' | tr -s ' ')
	echo "$n" | grep -q " $p $p $p $((p / 2)) $((p / 2)) $((p / 2)) " &&
		ok "EHRPWM4 at $p ns: every period $p ns, every high time $((p / 2))" ||
		bad "EHRPWM4 at $p ns: ch7 $n"
}

echo "stress: ch7 alone at 125 MS/s, which the reader takes live"
# The pipeline sets P9.25 to its class: it stops while EHRPWM4 is ours.
"$AI64" pipe stop >/dev/null 2>&1
check "ai64 la mode 125 7" "$AI64" la mode 125 7
"$AI64" la mode | grep -q '4 samples a word.*the reader keeps up' && ok "4 samples a word, live" ||
	bad "ai64 la mode: $("$AI64" la mode)"
for p in 2000 400 200 40 16; do
	exact $p
done
# Live, the reader finds every change of a 5 MHz square wave.
"$AI64" fabric route r5f0_1 pwm 200 100 >/dev/null 2>&1
sleep 0.5
out=$("$AI64" la rate 2)
lost=$(echo "$out" | sed -n 's/.*blocks lost \([0-9]*\) .*/\1/p')
behind=$(echo "$out" | sed -n 's/.*fell a ring behind \([0-9]*\);.*/\1/p')
edges=$(echo "$out" | sed -n 's/.* found \([0-9]*\) edges\/s.*/\1/p')
[ -n "$lost" ] && [ "$lost" -le 1000 ] && [ "$behind" = 0 ] && [ "$edges" -ge 9900000 ] &&
	ok "EHRPWM4 at 200 ns, live: $edges edges/s found, $lost blocks lost, never behind" ||
	bad "la rate at 200 ns: $(echo "$out" | tail -n 1)"

echo "stress: every channel at 125 MS/s, 533 MB/s, read from the ring after"
check "ai64 la mode 125" "$AI64" la mode 125
out=$("$AI64" la measure 0.04 2>&1)
n=$(echo "$out" | awk '$1 == 7 && $2 == "P9.11"' | tr -s ' ')
echo "$n" | grep -q ' 16 16 16 8 8 8 ' && ok "40 ms of every channel: ch7's every period 16 ns, high 8" ||
	bad "40 ms at 125 MS/s: ch7 $n"
n=$(echo "$out" | sed -n 's/^\([0-9]*\) holes.*/\1/p')
[ "${n:-0}" -le 3 ] && ok "40 ms of every channel: ${n:-no} holes" || bad "40 ms at 125 MS/s: $n holes"

echo "putting the steps back"
check "ai64 la mode 25" "$AI64" la mode 25
check "R5F0_1 steps again" "$AI64" fabric route r5f0_1 steps 5
check "the pipeline starts again" "$AI64" pipe start
finish
