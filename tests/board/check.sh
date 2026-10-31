#!/bin/sh
# SPDX-License-Identifier: MIT
# Check the demo as it runs now. Start it first (ai64-demo --tmux), or use
# soak.sh, which starts it.
. "$(dirname "$0")/lib.sh"

echo "check: the demo as it runs"
check_demo
finish
