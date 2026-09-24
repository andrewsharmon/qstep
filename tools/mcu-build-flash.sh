#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 andrewsharmon
# Build a Zephyr app for the UNO Q STM32 in the Lima VM and flash it over the
# board's on-board SWD (OpenOCD + linuxgpiod on the Linux side), via adb.
# usage: tools/mcu-build-flash.sh firmware/<app>
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP=$(cd "$1" && pwd)
NAME=$(basename "$APP")
ADB=$ROOT/tools/platform-tools/adb
export PATH=$ROOT/tools/bin:$PATH

limactl shell qstep -- bash -lc ". ~/zvenv/bin/activate && cd ~/zp && \
    west build -b arduino_uno_q '$APP' -d ~/build-$NAME 2>&1 | grep -E 'error|warning:|FLASH:|RAM:' ; \
    cp ~/build-$NAME/zephyr/zephyr.elf '$APP/$NAME.elf'"
$ADB push "$APP/$NAME.elf" /root/qstep/ >/dev/null
$ADB shell "cd /opt/openocd && ./bin/openocd -s /opt/openocd -f openocd_gpiod.cfg \
    -c 'program /root/qstep/$NAME.elf verify reset exit' 2>&1 | grep 'Verified OK'" \
    || { echo "flash failed"; exit 1; }
