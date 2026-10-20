#!/bin/bash
# SPDX-License-Identifier: MIT
# One-time setup of the demo on the BeagleBoard Debian image. Run it on the board.
#
#   sudo ./ai64-beagle/install-beagle.sh                    from a bundle
#   sudo ./beagle-linux/scripts/install-beagle.sh [dir]     from a repo checkout
#
# Installs the demo and makes the parts that were lost at every reboot stick:
#   - ai64 and ai64-demo on PATH, the ICSSG0, R5F, and DSP firmwares in
#     /lib/firmware
#   - the patched pru_rproc and ti_k3_r5_remoteproc, so the kernel loads them
#     instead of the stock modules
#   - the demo overlay on the default extlinux label, in place of the stock
#     P9.25 PWM overlay (both mux ball W26)
#   - a udev rule that makes /dev/rpmsg* usable by the dialout group
#   - ai64-icssg0.service, which starts RTU0, PRU0, and Tx_PRU0 at boot
#   - tmux, for ai64-demo --tmux, from apt when it is missing
# The R5F images are installed as /lib/firmware/ai64_r5f0_0.elf and so on,
# not under the kernel's default names (j7-main-r5f0_0-fw), so the R5F cores
# stay off until ai64-demo starts them.
#
# Artifacts come from dir when it is given (flat, by file name), else from
# the directory this script is in when it is a bundle, else from the build
# outputs in this repo. Reboot when it finishes. Pads, the modules, and the
# rpmsg permissions only change at boot.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC=${1:-}
# Run from a bundle (./beagle-linux/scripts/build.sh bundle), the artifacts
# sit next to this script.
if [[ -z $SRC && -f $HERE/ai64 ]]; then
	SRC=$HERE
fi
EXTLINUX=/boot/firmware/extlinux/extlinux.conf
OVERLAY_DIR=/boot/firmware/overlays
BASE_DTB=/boot/firmware/ti/k3-j721e-beagleboneai64.dtb
OVERLAY=k3-j721e-beagleboneai64-ai64-demo.dtbo
# Stock overlays that mux a ball this demo uses. They come off the default label.
CONFLICTS=(
	k3-j721e-beagleboneai64-pwm-epwm4-p9_25.dtbo
)

if [[ $(id -u) -ne 0 ]]; then
	exec sudo "$0" "$@"
fi

# find NAME REPO-PATH: the file in artifact-dir, else the repo build output.
find_artifact() {
	local name=$1 repo=$2
	if [[ -n $SRC && -f $SRC/$name ]]; then
		echo "$SRC/$name"
	elif [[ -f $ROOT/$repo ]]; then
		echo "$ROOT/$repo"
	else
		echo "missing $name (looked in ${SRC:-<no artifact-dir>} and $ROOT/$repo)" >&2
		return 1
	fi
}

KREL=$(uname -r)
AI64=$(find_artifact ai64 linux-app/ai64)
DEMO=$(find_artifact ai64-demo beagle-linux/scripts/ai64-demo)
RTU0=$(find_artifact ai64-icssg0-rtu0.elf pru/build/ai64-icssg0-rtu0.elf)
PRU0=$(find_artifact ai64-icssg0-pru0.elf pru/build/ai64-icssg0-pru0.elf)
TXPRU0=$(find_artifact ai64-icssg0-txpru0.elf pru/build/ai64-icssg0-txpru0.elf)
# The ring images for ICSSG0 slice 1 and both ICSSG1 slices.
declare -A RING
for f in ai64-icssg0-{pru,rtu,txpru}1.elf ai64-icssg1-{pru,rtu,txpru}{0,1}.elf; do
	RING[$f]=$(find_artifact "$f" "pru/build/$f")
done
declare -A R5
for core in r5f0_0 r5f0_1 r5f1_0 r5f1_1; do
	R5[$core]=$(find_artifact "ai64_$core.elf" "build/zephyr-main_$core/zephyr/ai64_$core.elf")
done
PRU_KO=$(find_artifact pru_rproc.ko beagle-linux/kernel/pru_rproc/pru_rproc.ko)
R5_KO=$(find_artifact ti_k3_r5_remoteproc.ko beagle-linux/kernel/k3_r5_rproc/ti_k3_r5_remoteproc.ko)
DTBO=$(find_artifact "$OVERLAY" "beagle-linux/overlays/$OVERLAY")
C66X=$(find_artifact ai64-c66x.elf dsp/build/ai64-c66x.elf)
C7X=$(find_artifact ai64-c7x.elf dsp/build/ai64-c7x.elf)

# The modules have to be built for this kernel.
for ko in "$PRU_KO" "$R5_KO"; do
	if ! grep -aq "vermagic=$KREL " "$ko"; then
		echo "$ko was not built for $KREL." >&2
		echo "On the PC: AI64_SSH=USER@BOARD ./beagle-linux/scripts/build.sh kmod && ./beagle-linux/scripts/build.sh bundle," >&2
		echo "then copy the bundle over and run this again." >&2
		exit 1
	fi
done

# The overlay has to apply to the board's own DTB.
if command -v fdtoverlay >/dev/null 2>&1 && [[ -f $BASE_DTB ]]; then
	trial=$(mktemp)
	if ! fdtoverlay -i "$BASE_DTB" -o "$trial" "$DTBO"; then
		rm -f "$trial"
		echo "The overlay does not apply to $BASE_DTB." >&2
		exit 1
	fi
	rm -f "$trial"
fi

echo "userspace and firmware"
install -m 0755 "$AI64" /usr/bin/ai64
install -m 0755 "$DEMO" /usr/bin/ai64-demo
install -m 0644 "$RTU0" /lib/firmware/ai64-icssg0-rtu0.elf
install -m 0644 "$PRU0" /lib/firmware/ai64-icssg0-pru0.elf
install -m 0644 "$TXPRU0" /lib/firmware/ai64-icssg0-txpru0.elf
for f in "${!RING[@]}"; do
	install -m 0644 "${RING[$f]}" "/lib/firmware/$f"
done
for core in "${!R5[@]}"; do
	install -m 0644 "${R5[$core]}" "/lib/firmware/ai64_$core.elf"
done
install -m 0644 "$C66X" /lib/firmware/ai64-c66x.elf
install -m 0644 "$C7X" /lib/firmware/ai64-c7x.elf
if command -v setcap >/dev/null 2>&1 || [[ -x /usr/sbin/setcap ]]; then
	"$(command -v setcap || echo /usr/sbin/setcap)" cap_sys_rawio+ep /usr/bin/ai64
fi

if ! command -v tmux >/dev/null 2>&1; then
	echo "tmux (for ai64-demo --tmux)"
	if ! { apt-get update -qq && apt-get install -y -qq tmux; } >/dev/null 2>&1; then
		echo "Could not install tmux (does the board have internet?). Plain ai64-demo still works." >&2
	fi
fi

echo "pru_rproc and ti_k3_r5_remoteproc for $KREL"
install -D -m 0644 "$PRU_KO" "/lib/modules/$KREL/updates/pru_rproc.ko"
install -D -m 0644 "$R5_KO" "/lib/modules/$KREL/updates/ti_k3_r5_remoteproc.ko"
depmod "$KREL"

echo "overlay"
install -m 0755 "$DTBO" "$OVERLAY_DIR/$OVERLAY"
[[ -f $EXTLINUX.ai64-bak ]] || cp "$EXTLINUX" "$EXTLINUX.ai64-bak"
OVERLAY="$OVERLAY" CONFLICTS="${CONFLICTS[*]}" python3 - "$EXTLINUX" <<'EOF'
import os, sys

path = sys.argv[1]
want = "/overlays/" + os.environ["OVERLAY"]
drop = {"/overlays/" + o for o in os.environ["CONFLICTS"].split()}
lines = open(path).read().split("\n")

default = next(l.split(None, 1)[1].strip() for l in lines if l.startswith("default "))
start = next(i for i, l in enumerate(lines) if l.strip() == "label " + default)
end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("label ")), len(lines))

for i in range(start, end):
    if lines[i].strip().startswith("fdtoverlays"):
        have = [o for o in lines[i].split()[1:] if o not in drop]
        for o in lines[i].split()[1:]:
            if o in drop:
                print("removed " + o + " from the default label (it muxes a demo pin)")
        if want not in have:
            have.append(want)
        lines[i] = "    fdtoverlays " + " ".join(have)
        break
else:
    at = next(i for i in range(start, end) if lines[i].strip().startswith("fdtdir"))
    lines.insert(at + 1, "    fdtoverlays " + want)

open(path, "w").write("\n".join(lines))
EOF

echo "rpmsg permissions and ICSSG0 start"
cat > /etc/udev/rules.d/99-ai64-rpmsg.rules <<'EOF'
KERNEL=="rpmsg[0-9]*", GROUP="dialout", MODE="0660"
EOF

cat > /usr/libexec/ai64-icssg0-start <<'EOF'
#!/bin/bash
# Start RTU0, then PRU0 and Tx_PRU0, from /lib/firmware. Leaves a running
# core alone. RTU0 goes first: it clears the shared-RAM records that the other
# two read, and they adopt whatever seq they find when they start.
set -eu
checked=0
for name in b004000.rtu:ai64-icssg0-rtu0.elf b034000.pru:ai64-icssg0-pru0.elf \
	b00a000.txpru:ai64-icssg0-txpru0.elf; do
	want=${name%%:*}
	fw=${name##*:}
	for _ in $(seq 1 50); do
		for d in /sys/class/remoteproc/remoteproc*; do
			[[ -e $d/name && $(cat "$d/name") == "$want" ]] || continue
			# The PRU remoteproc exists, so pru_rproc is loaded by now.
			if [[ $checked -eq 0 ]] && ! grep -q pru_rproc_kick /proc/kallsyms; then
				echo "pru_rproc has no virtio kick. Was the kernel upgraded? Rebuild the" >&2
				echo "module for $(uname -r) and run install-beagle.sh again. RTU0 RPMsg will fail." >&2
			fi
			checked=1
			if [[ $(cat "$d/state") == offline ]]; then
				printf "%s\n" "$fw" > "$d/firmware"
				printf "start\n" > "$d/state"
			fi
			break 2
		done
		sleep 0.2
	done
	if [[ $want == b004000.rtu ]]; then
		for _ in $(seq 1 25); do
			ls /sys/bus/rpmsg/devices/ 2>/dev/null | grep -q 'rpmsg-raw.-1.30$' && break
			sleep 0.2
		done
	fi
done
EOF
chmod 0755 /usr/libexec/ai64-icssg0-start

cat > /etc/systemd/system/ai64-icssg0.service <<'EOF'
[Unit]
Description=Start the BeagleBone AI-64 demo ICSSG0 cores
After=systemd-modules-load.service local-fs.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/libexec/ai64-icssg0-start

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable ai64-icssg0.service
udevadm control --reload

echo
echo "Done. Reboot to apply the overlay and the modules."
if command -v tmux >/dev/null 2>&1; then
	echo "After the reboot: ai64-demo --tmux"
else
	echo "After the reboot: ai64-demo"
fi
echo "Original extlinux.conf: $EXTLINUX.ai64-bak"
echo "The modules only fit kernel $KREL. After a kernel upgrade, rebuild them and run this again."
