#!/bin/bash
# SPDX-License-Identifier: MIT
# Run the board tests from the PC, over SSH.
#
#   AI64_SSH=USER@BOARD tests/run.sh <test> [args]
#
# test is check, soak [runs], or routes (tests/board/<test>.sh). This copies
# tests/board/ to the board and runs the test there as root, with sudo if
# USER is not root, so it may ask for the board's sudo password. The board
# needs the demo installed (install-beagle.sh, or the Yocto image).
# AI64_SSH_OPTS adds options to ssh and scp, for example
# "-o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no" for a board
# whose card was just written and has new host keys.
set -euo pipefail

if [[ -z ${AI64_SSH:-} || $# -lt 1 ]]; then
	echo "usage: AI64_SSH=USER@BOARD $0 check | soak [runs] | routes" >&2
	exit 2
fi
test=$1
shift
here=$(cd "$(dirname "$0")" && pwd)
if [[ ! -f $here/board/$test.sh ]]; then
	echo "no test $test: tests/board/ has $(cd "$here/board" && ls -- *.sh | grep -v lib.sh | tr '\n' ' ')" >&2
	exit 2
fi
dest=/tmp/ai64-tests
read -r -a opts <<<"${AI64_SSH_OPTS:-}"
ssh "${opts[@]}" "$AI64_SSH" "rm -rf $dest && mkdir -p $dest"
scp -q "${opts[@]}" "$here"/board/*.sh "$AI64_SSH:$dest/"
sudo=
[[ ${AI64_SSH%%@*} != root ]] && sudo=sudo
ssh -t "${opts[@]}" "$AI64_SSH" "$sudo sh $dest/$test.sh $*"
