#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 andrewsharmon
# Build a Zephyr app for the UNO Q STM32 in the Lima VM and flash it over the
# board's on-board SWD (OpenOCD + linuxgpiod on the Linux side), via adb.
# usage: tools/mcu-build-flash.sh firmware/<app>
# QSTEP_VM=<name> selects the Lima VM (default: qstep).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP=$(cd "$1" && pwd)
NAME=$(basename "$APP")
ADB=$ROOT/tools/platform-tools/adb
VM=${QSTEP_VM:-qstep}
export PATH=$ROOT/tools/bin:$PATH

# Only a successful build is copied out and flashed (never a stale ELF).
limactl shell "$VM" -- bash -lc "set -o pipefail; . ~/zvenv/bin/activate && cd ~/zp && \
    west build -b arduino_uno_q '$APP' -d ~/build-$NAME 2>&1 | { grep -E 'error|warning:|FLASH:|RAM:' || true; } && \
    cp ~/build-$NAME/zephyr/zephyr.elf '$APP/$NAME.elf'" || { echo "build failed"; exit 1; }
$ADB push "$APP/$NAME.elf" /root/qstep/ >/dev/null
$ADB shell "cd /opt/openocd && ./bin/openocd -s /opt/openocd -f openocd_gpiod.cfg \
    -c 'program /root/qstep/$NAME.elf verify reset exit' 2>&1 | grep 'Verified OK'" \
    || { echo "flash failed"; exit 1; }
