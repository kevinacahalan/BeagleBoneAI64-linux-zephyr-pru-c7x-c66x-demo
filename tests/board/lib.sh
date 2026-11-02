# SPDX-License-Identifier: MIT
# What the board tests share. tests/run.sh copies this directory to the
# board and runs one test from it, as root. Each test prints a line per
# check, "ok" or "FAIL", and exits 1 if any failed.

AI64=$(command -v ai64 || echo /root/ai64)
DEMO=$(command -v ai64-demo || echo /root/ai64-demo)
HERE=$(cd "$(dirname "$0")" && pwd)
FAILS=0

ok() { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }

# check WHAT COMMAND...: ok if the command succeeds.
check() {
	local what=$1
	shift
	if "$@" >/dev/null 2>&1; then ok "$what"; else bad "$what"; fi
}

finish() {
	if [ "$FAILS" -eq 0 ]; then
		echo "PASS"
		exit 0
	fi
	echo "$FAILS FAILED"
	exit 1
}

# Restart every core and open the tmux session, then give the startup
# script and the pipeline's training time to finish.
start_demo() {
	if ! "$DEMO" --tmux </dev/null >"$HERE/demo.out" 2>&1; then
		bad "ai64-demo --tmux failed:"
		tail -n 5 "$HERE/demo.out"
		return 1
	fi
	sleep 40
}

# rproc_attr NAME ATTR: a remoteproc attribute (state, firmware) of the
# core called NAME.
rproc_attr() {
	local d
	for d in /sys/class/remoteproc/remoteproc*; do
		if [ "$(cat "$d/name")" = "$1" ]; then
			cat "$d/$2"
			return
		fi
	done
}

# The demo, as ai64-demo started it: each check looks at what one window
# or one ai64 subcommand shows.
check_demo() {
	local linux pipe fabric la core n want right total state fw c hz lo hi

	linux=$(tmux capture-pane -p -J -S -300 -t ai64:linux 2>/dev/null)
	if [ -z "$linux" ]; then
		bad "no tmux session ai64: is the demo running?"
		return
	fi
	# The startup script's answers (README: What you should see).
	echo "$linux" | grep -q 'Linux sent "ping" to ICSSG0 RTU0, and it echoed it' &&
		ok "RTU0 echoed ping over RPMsg" || bad "RTU0 did not echo ping"
	for core in R5F0_0 R5F0_1 R5F1_0 R5F1_1; do
		echo "$linux" | grep -q "it replied \"Zephyr on $core got: hello from linux\"" &&
			ok "$core answered say over eRPC" || bad "$core did not answer say"
	done
	echo "$linux" | grep -q 'chunk: the bulk buffer matched' &&
		ok "R5F0_0 checked the bulk buffer" || bad "the bulk buffer did not match"
	echo "$linux" | grep -q 'spi wrote 5 bytes' &&
		ok "R5F0_0 wrote SPI7" || bad "R5F0_0 did not write SPI7"
	echo "$linux" | grep -q 'passed spi to R5F0_0, which wrote' &&
		ok "R5F1_0 passed spi to R5F0_0 over ipc_service" || bad "R5F1_0 did not pass spi on"
	n=$(echo "$linux" | grep -c -E ': round trip .* 0 of [0-9]+ lost')
	[ "$n" -eq 3 ] && ok "the monitor lost no pings to the other three" ||
		bad "the monitor's table: $n of 3 cores with no pings lost"
	echo "$linux" | grep -q 'dma: its UDMA copied .*all match the A72' &&
		ok "R5F0_0's UDMA fed the C7x's MMA, all match the A72" || bad "r5f0_0 dma failed"
	want=$(echo "$linux" | grep -o 'checked here on the same windows: [0-9]* of [0-9]* right')
	right=$(echo "$want" | awk '{print $7}')
	total=$(echo "$want" | awk '{print $9}')
	[ -n "$right" ] && [ "$right" = "$total" ] && ok "the pipeline trained: $right of $total right" ||
		bad "the pipeline's training: ${want:-no result}"
	n=$(echo "$linux" | grep -c -E 'failed|link down|did not')
	[ "$n" -eq 0 ] && ok "no failures in the linux window" || bad "$n lines in the linux window say failed, link down, or did not"

	# Every demo core (ai64 cores) running its firmware.
	n=0
	for core in $("$AI64" cores | awk '!/^#/ {print $3 ":" $4}'); do
		state=$(rproc_attr "${core%%:*}" state)
		fw=$(rproc_attr "${core%%:*}" firmware)
		if [ "$state" != running ] || [ "$fw" != "${core#*:}" ]; then
			bad "${core%%:*} is ${state:-missing}, with ${fw:-no firmware}"
			n=$((n + 1))
		fi
	done
	[ "$n" -eq 0 ] && ok "all $("$AI64" cores | grep -c -v '^#') demo cores run their firmware"

	# The pipeline: nothing lost, nothing wrong.
	pipe=$("$AI64" pipe once 2>&1)
	echo "$pipe" | grep -q -E 'since training [0-9]+ right, 0 wrong' &&
		ok "the pipeline: $(echo "$pipe" | grep -o 'since training [0-9]* right, [0-9]* wrong')" ||
		bad "the pipeline's score: $(echo "$pipe" | grep -o 'since training.*')"
	n=$(echo "$pipe" | awk '/^  [A-Z]/ && NF > 8 { s += $(NF - 1) + $NF } END { print s + 0 }')
	[ "$n" -eq 0 ] && ok "no pipeline stage lost a window or failed" || bad "pipeline stages lost or failed $n windows"

	# The fabric, from its window: Linux can be node 0 only once.
	fabric=$(tmux capture-pane -p -J -t ai64:fabric 2>/dev/null)
	echo "$fabric" | grep -q -E 'and the 12 counts sum to' &&
		ok "the fabric's spinlock tally agrees" || bad "the fabric's tally: $(echo "$fabric" | grep tally)"
	n=$(echo "$fabric" | awk '$1 ~ /^(linux|r5f|c66x|c7x|g0rtu)/ && NF >= 10 && $3 ~ /^[0-9]+$/ { s += $9 + $10 } END { print s + 0 }')
	[ "$n" -eq 0 ] && ok "no fabric timeouts or errors in the last second" || bad "$n fabric timeouts or errors in the last second"

	# Linux's jobs on each DSP, while the pipeline runs on them too.
	for core in c66x0 c66x1 c7x; do
		"$AI64" dsp "$core" 4096 32 5 2>&1 | grep -q 'all match the A72' &&
			ok "FIR jobs on $core match the A72" || bad "FIR jobs on $core"
	done
	"$AI64" mma 256 5 2>&1 | grep -q 'all match the A72' &&
		ok "the C7x's MMA layer matches the A72" || bad "the C7x's MMA layer"

	# The logic analyzer on ICSSG1: both streams flowing with nothing lost,
	# and what it measures on the jumpered signals (pru/logic_analyzer/).
	la=$("$AI64" la once 2>&1)
	n=$(echo "$la" | grep -c -E 'PRU[01] +[0-9]+ edges/s, .* gaps 0 \(0 records\), reader missed 0')
	[ "$n" -eq 2 ] && ok "the logic analyzer's two streams lost nothing" ||
		bad "the logic analyzer's streams: $(echo "$la" | grep -E '^ +PRU' | tr -s ' ')"
	# ch7, 9, 11, 12, 13: the PWM generators, 15 kHz on average over the steps;
	# ch6: the 1 kHz square wave.
	for c in 6 7 9 11 12 13; do
		hz=$(echo "$la" | awk -v c=$c '$1 == c && $2 ~ /^P[89]\./ {
			f = $(NF - 2); if ($(NF - 1) == "kHz") f *= 1000; print int(f) }')
		lo=14000 hi=16000
		[ "$c" -eq 6 ] && lo=990 hi=1010
		[ -n "$hz" ] && [ "$hz" -ge $lo ] && [ "$hz" -le $hi ] &&
			ok "the analyzer's ch$c reads $hz Hz" || bad "the analyzer's ch$c reads ${hz:-nothing} Hz, not $lo-$hi"
	done
	# Slice 1's dead band, from A falling to B rising: 1000 ns, exact in hardware.
	n=$("$AI64" la measure 1 9:10 2>&1 | awk '/ch9 S1_A -> ch10 S1_B/ { print $(NF - 1) }')
	[ -n "$n" ] && [ "$n" -ge 992 ] && [ "$n" -le 1008 ] &&
		ok "la measure: slice 1's dead band $n ns" || bad "la measure: slice 1's dead band ${n:-not measured} ns"
	# A trigger on the square wave's rising edge, matched by Tx_PRU0: about 1000 hits a second.
	"$AI64" la trigger 6 rise >/dev/null
	n=$("$AI64" la once 2>&1 | awk '/^ +PRU0/ { print $NF }')
	"$AI64" la trigger off >/dev/null
	[ -n "$n" ] && [ "$n" -ge 900 ] && [ "$n" -le 1100 ] &&
		ok "Tx_PRU0's trigger on the square wave: $n hits" || bad "the trigger: ${n:-no} hits, not about 1000"
	# A mark through the fabric, from Linux to ICSSG1 RTU0, fabric node 10.
	"$AI64" la mark 1 2>&1 | grep -q 'g1rtu0 marked its stream' &&
		ok "a mark reached the analyzer's stream through the fabric" || bad "ai64 la mark"

	# The MCP server (ai64 mcp): la_measure through JSON-RPC, slice 1's dead band again.
	n=$(wget -q -O - --header "Content-Type: application/json" --post-data \
		'{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"la_measure","arguments":{"seconds":0.5,"pairs":["9:10"]}}}' \
		http://127.0.0.1:8065/mcp 2>/dev/null | grep -o '"after_a_falls_ns":{"min":[0-9-]*,"median":[0-9]*' | grep -o '[0-9]*$')
	[ -n "$n" ] && [ "$n" -ge 992 ] && [ "$n" -le 1008 ] &&
		ok "the MCP server's la_measure: slice 1's dead band $n ns" || bad "the MCP server's la_measure: ${n:-no answer}"

	# The pads, and the web page's server.
	check "every demo pad is muxed as the overlay and Zephyr ask" "$AI64" pads
	n=$(for f in /proc/[0-9]*/cmdline; do tr '\0' ' ' <"$f" 2>/dev/null; echo; done |
		grep -c '^[^ ]*ai64 web')
	[ "$n" -eq 1 ] && ok "one web server" || bad "$n web servers"
}
