#!/bin/sh
# SPDX-License-Identifier: MIT
# Fabric routes with actions, through cores of every kind: each sets a PWM
# generator, and ai64 pwm shows it running what was asked. Leaves the
# generators stepping and the pipeline following again. The demo must be
# running.
. "$(dirname "$0")/lib.sh"

# route WHAT EXPECT ARGS...: the route says EXPECT on its hop.
route() {
	local what=$1 expect=$2 out
	shift 2
	out=$("$AI64" fabric route "$@" 2>&1)
	echo "$out" | grep -q "$expect" && echo "$out" | grep -q 'value ok' &&
		ok "$what" || { bad "$what:"; echo "$out" | sed 's/^/        /'; }
}

# pwm_shows PINS EXPECT: ai64 pwm's lines for that generator say EXPECT.
pwm_shows() {
	"$AI64" pwm once | grep -A3 "^$1 " | grep -q "$2"
}

echo "routes: fabric actions on every PWM generator"
route "C66x 0 -> R5F0_1 sets P9.25 to 2 kHz" "r5f0_1 set its PWM" c66x0 r5f0_1 g0rtu0 pwm 500000 125000
sleep 0.5
check "ai64 pwm shows P9.25 fixed at 500000 ns" pwm_shows P9.25 'fixed: period 500000 ns'
route "C7x -> R5F1_1 -> RTU1 sets slice 1 to 50 kHz" "g0rtu1 set its PWM" c7x r5f1_1 g0rtu1 c66x1 pwm 20000 5000
sleep 0.5
check "ai64 pwm shows P9.12 fixed at 20000 ns" pwm_shows 'P9.12 + P9.15' 'fixed: period 20000 ns'
route "R5F1_0 -> RTU0 sets slice 0 sweeping" "g0rtu0 set its" r5f1_0 g0rtu0 sweep 100
sleep 0.5
check "ai64 pwm shows P8.11 sweeping" pwm_shows 'P8.11 + P8.12' 'sweeping'

echo "putting the steps back"
route "R5F0_1 steps again" "r5f0_1 set its steps" r5f0_1 steps 5
route "RTU1 steps again" "g0rtu1 set its" g0rtu1 steps 5
route "RTU0 steps again" "g0rtu0 set its" g0rtu0 steps 5
sleep 0.5
check "ai64 pwm shows slice 0 stepping" pwm_shows 'P8.11 + P8.12' 'stepping'
check "the pipeline starts again" "$AI64" pipe start >/dev/null 2>&1
finish
