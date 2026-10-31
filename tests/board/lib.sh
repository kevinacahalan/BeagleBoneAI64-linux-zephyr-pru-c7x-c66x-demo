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
	local linux pipe fabric core n want right total state fw

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
	echo "$fabric" | grep -q -E 'and the 10 counts sum to' &&
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

	# The pads, and the web page's server.
	check "every demo pad is muxed as the overlay and Zephyr ask" "$AI64" pads
	n=$(for f in /proc/[0-9]*/cmdline; do tr '\0' ' ' <"$f" 2>/dev/null; echo; done |
		grep -c '^[^ ]*ai64 web')
	[ "$n" -eq 1 ] && ok "one web server" || bad "$n web servers"
}
