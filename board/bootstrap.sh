#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 andrewsharmon
# QStep step 2: install onto a UNO Q that runs Arduino's official image.
#
# Step 1 (on your computer, Arduino's own tool, no build tools needed):
#     arduino-flasher-cli flash unoq --version 20250807-136
# Step 2 (on the board, as root: `adb shell`, or `sudo` in a board terminal/SSH;
#         the board needs internet, e.g. Wi-Fi set up in Arduino's first-boot setup):
#     sh qstep-bootstrap.sh                       # download the release bundle and install
#     sh qstep-bootstrap.sh --bundle FILE.tar.gz  # install from a bundle already on the board
#
# Options: --release-url URL (where qstep-<version>.tar.gz and .sha256 live),
#          --version V, --force (skip the base image check), and any board/install.sh
#          step flags (default --all). Then power-cycle the board (unplug/replug).
set -eu

QSTEP_VERSION=${QSTEP_VERSION:-@VERSION@}
RELEASE_URL=${QSTEP_RELEASE_URL:-@RELEASE_URL@}
# The RT kernel is built from the kernel of this Arduino image release.
BASE_KERNEL_PKG=linux-image-6.16.0-geffa8626771a
BASE_IMAGE=20250807-136

BUNDLE=""
FORCE=0
STEPS=""
while [ $# -gt 0 ]; do
	case "$1" in
	--bundle) BUNDLE=$2; shift ;;
	--release-url) RELEASE_URL=$2; shift ;;
	--version) QSTEP_VERSION=$2; shift ;;
	--force) FORCE=1 ;;
	-h|--help) sed -n '4,15p' "$0"; exit 0 ;;
	*) STEPS="$STEPS $1" ;;
	esac
	shift
done
STEPS=${STEPS:-" --all"}

[ "$(id -u)" = 0 ] || { echo "Run as root (adb shell, or: sudo sh $0)"; exit 1; }

# `adb shell` exports TMPDIR=/data/local/tmp (an Android path) which doesn't
# exist on the UNO Q; apt/dpkg helpers then fail to create temp files.
if [ -n "${TMPDIR:-}" ] && [ ! -d "$TMPDIR" ]; then
	export TMPDIR=/tmp
fi

# ---- is this the Arduino image the kernel was built for? ----------------------
if ! dpkg-query -W -f='${Status}' "$BASE_KERNEL_PKG" 2>/dev/null | grep -q " ok installed"; then
	echo "This board does not run Arduino's UNO Q image $BASE_IMAGE ($BASE_KERNEL_PKG not installed)."
	echo "Flash it first:  arduino-flasher-cli flash unoq --version $BASE_IMAGE"
	[ "$FORCE" = 1 ] || exit 1
	echo "--force given: continuing anyway."
fi

# ---- get the bundle -------------------------------------------------------------
WORK=/opt/qstep
mkdir -p $WORK
if [ -z "$BUNDLE" ]; then
	case "$RELEASE_URL" in @*) echo "No release URL built in; use --release-url or --bundle"; exit 1 ;; esac
	# The board has no RTC backup battery; a wrong clock breaks TLS and apt.
	if [ "$(date +%Y)" -lt 2026 ]; then
		echo "System clock looks wrong ($(date)); syncing via NTP..."
		timedatectl set-ntp true 2>/dev/null || true
		sleep 5
	fi
	BUNDLE=$WORK/qstep-$QSTEP_VERSION.tar.gz
	echo "Downloading QStep $QSTEP_VERSION from $RELEASE_URL"
	curl -fSL -o "$BUNDLE" "$RELEASE_URL/qstep-$QSTEP_VERSION.tar.gz"
	curl -fsSL -o "$BUNDLE.sha256" "$RELEASE_URL/qstep-$QSTEP_VERSION.tar.gz.sha256"
fi
if [ -f "$BUNDLE.sha256" ]; then
	(cd "$(dirname "$BUNDLE")" && sha256sum -c "$(basename "$BUNDLE").sha256")
else
	echo "warning: no $BUNDLE.sha256 next to the bundle; skipping its checksum"
fi

SRC=$WORK/src
rm -rf $SRC
mkdir -p $SRC
tar -xzf "$BUNDLE" -C $SRC
(cd $SRC && sha256sum -c --quiet dist/SHA256SUMS)
echo "Bundle OK: $(cat $SRC/VERSION 2>/dev/null | head -1)"

# ---- install --------------------------------------------------------------------
# shellcheck disable=SC2086
sh $SRC/board/install.sh $STEPS

cat <<EOF

QStep is installed. Next:
  1. Power-cycle the board (unplug and replug USB-C) to boot the real-time kernel.
     (A warm "reboot" is not enough on the UNO Q.)
  2. AXIS is shown over VNC, which stays off until you set a password
     (it never runs without one). As root on the board:
       qstep-vnc-passwd
     then on your computer: adb forward tcp:5900 tcp:5900 ; open vnc://127.0.0.1:5900
  3. Machine config: /home/arduino/qstep-config/unoq-shield.ini (units: motor revolutions,
     SCALE 6400 steps/rev at 1/32 microstepping; set SCALE = steps-per-rev / mm-per-rev
     for your mechanics).
The STM32's original firmware was saved to /root/qstep/mcu-flash-backup.bin.
EOF
