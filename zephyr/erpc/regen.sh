#!/bin/bash
# SPDX-License-Identifier: MIT
# Regenerate zephyr/erpc/shim/ from ai64.erpc with erpcgen 1.14.0. The shim
# is checked in, so the normal build does not run this. Run it after you
# change ai64.erpc. erpcgen is built from third_party/erpc in the Linux image.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
"$ROOT/beagle-linux/scripts/build.sh" erpcgen
