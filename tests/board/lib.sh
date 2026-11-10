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

	# Every demo core (ai64 cores) running its firmware; one with firmware -
	# (ICSSG1's free cores) stopped.
	n=0
	for core in $("$AI64" cores | awk '!/^#/ {print $3 ":" $4}'); do
		state=$(rproc_attr "${core%%:*}" state)
		fw=$(rproc_attr "${core%%:*}" firmware)
		if [ "${core#*:}" = - ]; then
			if [ "$state" = running ]; then
				bad "${core%%:*} runs ${fw:-something}, but is free"
				n=$((n + 1))
			fi
		elif [ "$state" != running ] || [ "$fw" != "${core#*:}" ]; then
			bad "${core%%:*} is ${state:-missing}, with ${fw:-no firmware}"
			n=$((n + 1))
		fi
	done
	[ "$n" -eq 0 ] && ok "all $("$AI64" cores | awk '!/^#/ && $4 != "-"' | wc -l) demo cores run their firmware, the free ones stopped"

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

	# The logic analyzer on ICSSG1 (pru/logic_analyzer/): PRU0 sampling
	# all 10 channels at 25 MS/s, the reader keeping up, and what it
	# measures on the jumpered signals.
	check "ai64 la mode 25: every channel at 25 MS/s" "$AI64" la mode 25
	la=$("$AI64" la once 2>&1)
	n=$(echo "$la" | sed -n 's/.*lost \([0-9]*\) blocks this interval.*/\1/p')
	[ -n "$n" ] && [ "$n" -le 20 ] && ok "the analyzer's reader kept up: $n blocks lost in a second" ||
		bad "the analyzer: $(echo "$la" | grep -E 'read|lost' | tr -s ' ')"
	# ch4: the 1 kHz square wave; ch5: ICSSG0 slice 0's PWM A, 15 kHz on
	# average over its steps.
	for c in 4 5; do
		hz=$(echo "$la" | awk -v c=$c '$1 == c && $2 ~ /^P[89]\./ {
			f = $(NF - 2); if ($(NF - 1) == "kHz") f *= 1000; print int(f) }')
		lo=14000 hi=16000
		[ "$c" -eq 4 ] && lo=990 hi=1010
		[ -n "$hz" ] && [ "$hz" -ge $lo ] && [ "$hz" -le $hi ] &&
			ok "the analyzer's ch$c reads $hz Hz" || bad "the analyzer's ch$c reads ${hz:-nothing} Hz, not $lo-$hi"
	done
	# A capture around the square wave's rising edge, found by the reader,
	# with a millisecond from the ring before it.
	out=$("$AI64" la capture 0.002 /tmp/ai64-check.vcd 4 rise pre 0.001 2>&1)
	echo "$out" | grep -q 'around the trigger hit' && grep -q '^#1000000$' /tmp/ai64-check.vcd &&
		ok "la capture: triggered on ch4's rise, 1 ms of the ring before it" ||
		bad "la capture with a trigger: $out"
	# Every sample at 125 MS/s, all 10 channels: more than the reader takes
	# live, so the capture waits, PRU0 stops, and the ring is read whole.
	out=$("$AI64" la mode 125 >/dev/null && "$AI64" la measure 0.04 2>&1)
	"$AI64" la mode 25 >/dev/null
	n=$(echo "$out" | awk '$1 == 4 && $2 == "P8.10" { print $5 }')
	echo "$out" | grep -q '^ *125 MS/s' && [ -n "$n" ] && [ "$n" -ge 79 ] && [ "$n" -le 81 ] &&
		! echo "$out" | grep -q -E '^[0-9]{3,} holes' &&
		ok "la measure at 125 MS/s, every channel: whole, ch4 $n edges in 40 ms" ||
		bad "la measure at 125 MS/s: $(echo "$out" | grep -E 'holes|^ *4 ' | tr -s ' ')"

	# The MCP server (ai64 mcp): la_capture around SPI7's chip select
	# falling while R5F0_0 writes "hello", then la_decode.
	(sleep 1.5; tmux send-keys -t ai64:linux 'r5f0_0 spi hello' Enter) &
	n=$(wget -q -O - --header "Content-Type: application/json" --post-data \
		'{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"la_capture","arguments":{"seconds":0.002,"pre_seconds":0.0002,"trigger":{"channel":1,"edge":"fall"}}}}' \
		http://127.0.0.1:8065/mcp 2>/dev/null | grep -o '"trigger_at_ns":[0-9]*')
	wait
	out=$(wget -q -O - --header "Content-Type: application/json" --post-data \
		'{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"la_decode","arguments":{"protocol":"spi","cs":1,"clk":2,"mosi":3}}}' \
		http://127.0.0.1:8065/mcp 2>/dev/null)
	[ -n "$n" ] && echo "$out" | grep -q 'mosi_text\\":\\"hello' &&
		ok "the MCP server: a capture on SPI7's chip select decodes \"hello\"" ||
		bad "the MCP server's capture and decode: ${n:-no hit}, $(echo "$out" | grep -o 'mosi_text[^,]*' | head -n 1)"
	n=$(wget -q -O - --header "Content-Type: application/json" --post-data \
		'{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"la_measure","arguments":{"seconds":0.2}}}' \
		http://127.0.0.1:8065/mcp 2>/dev/null | grep -o '"ch\\":4[^}]*\\"freq_hz\\":[0-9]*' | grep -o '[0-9]*$')
	[ -n "$n" ] && [ "$n" -ge 990 ] && [ "$n" -le 1010 ] &&
		ok "the MCP server's la_measure: the square wave at $n Hz" || bad "the MCP server's la_measure: ${n:-no answer}"

	# The pads, and the web page's server.
	check "every demo pad is muxed as the overlay and Zephyr ask" "$AI64" pads
	n=$(for f in /proc/[0-9]*/cmdline; do tr '\0' ' ' <"$f" 2>/dev/null; echo; done |
		grep -c '^[^ ]*ai64 web')
	[ "$n" -eq 1 ] && ok "one web server" || bad "$n web servers"
}
