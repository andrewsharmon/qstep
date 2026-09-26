#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 andrewsharmon
# Put an UNO Q back to Arduino's stock software with Arduino's own tools,
# removing QStep completely:
#   1. Linux: Arduino's official Debian image, flashed with arduino-flasher-cli.
#      This wipes the eMMC and needs the EDL jumper (see dist/README.md).
#   2. STM32: Arduino's sketch loader, written back with the stock image's
#      `arduino-cli burn-bootloader -b arduino:zephyr:unoq -P jlink`. The Linux
#      flasher never touches the STM32, so the QStep firmware would stay on it.
#
#   tools/restore-stock.sh [--version VER | --image FILE] [--mcu-only] [--yes]
#
#   --version VER  Arduino image to flash (default 20250807-136, the image the
#                  UNO Q shipped with; "latest" for Arduino's newest)
#   --image FILE   flash a local image archive instead of downloading one
#   --mcu-only     only restore the STM32 loader; Linux is left as it is
#   --yes          don't ask before wiping the board
#
# Run it on the computer the board is plugged into (macOS or Linux). Images are
# cached in ~/.cache/qstep/arduino-images, so a failed flash can be retried
# without downloading again. Before the wipe, QStep's files on the board
# (/root/qstep with its STM32 backup, and the LinuxCNC config) are copied to
# ~/.cache/qstep/board-backups/.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/qstep
VERSION=20250807-136
IMAGE=""
MCU_ONLY=0
YES=0

while [ $# -gt 0 ]; do
	case "$1" in
	--version) VERSION=$2; shift ;;
	--image) IMAGE=$2; shift ;;
	--mcu-only) MCU_ONLY=1 ;;
	--yes) YES=1 ;;
	*) sed -n '4,24p' "$0"; exit 2 ;;
	esac
	shift
done

die() { echo "!! $*" >&2; exit 1; }

ADB=$ROOT/tools/platform-tools/adb
[ -x "$ADB" ] || ADB=$(command -v adb) || die "adb not found (Android platform-tools)"
FLASHER=$ROOT/tools/flasher/arduino-flasher-cli
[ -x "$FLASHER" ] || FLASHER=$(command -v arduino-flasher-cli) ||
	[ "$MCU_ONLY" = 1 ] || die "arduino-flasher-cli not found: https://docs.arduino.cc/software/app-lab/configure/flash/"

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
board_up() { [ "$("$ADB" get-state 2>/dev/null)" = device ]; }

# "<version> <sha256>" of an UNO Q image from Arduino's index ("latest" resolves).
index_lookup() {
	"$FLASHER" list --format json | awk -F'"' -v want="$1" '
		/"version":/ { v = $4 } /"board":/ { b = $4 } /"latest":/ { l = /true/ }
		/"sha256":/ { if (b == "unoq" && (v == want || (want == "latest" && l))) { print v, $4; exit } }'
}

get_image() {
	if [ -n "$IMAGE" ]; then
		[ -e "$IMAGE" ] || die "no image at $IMAGE"
		return
	fi
	local sha dir=$CACHE/arduino-images
	read -r VERSION sha < <(index_lookup "$VERSION") ||
		die "Arduino's index has no UNO Q image '$VERSION' (see: arduino-flasher-cli list)"
	IMAGE=$dir/arduino-unoq-debian-image-$VERSION.tar.zst
	if [ -f "$IMAGE" ]; then
		echo "== checking cached image $IMAGE"
		[ "$(sha256 "$IMAGE")" = "$sha" ] && return
		echo "   checksum mismatch, downloading again"
	fi
	echo "== downloading Arduino image $VERSION"
	rm -rf "$dir/.download" && mkdir -p "$dir/.download"
	"$FLASHER" download unoq --version "$VERSION" --dest-dir "$dir/.download"
	local f
	f=$(ls "$dir"/.download/*.tar.zst | head -1)
	[ "$(sha256 "$f")" = "$sha" ] || die "downloaded image fails its checksum"
	mv "$f" "$IMAGE" && rm -rf "$dir/.download"
}

# The wipe takes QStep's STM32 backup with it; on a board that ran QStep only
# once, that backup is Arduino's original firmware.
backup_board() {
	board_up || { echo "== board not on adb, nothing to back up"; return; }
	local dir p
	dir=$CACHE/board-backups/$(date +%Y%m%d-%H%M%S)
	for p in /root/qstep /home/arduino/qstep-config; do
		if "$ADB" shell "test -d $p && echo yes" | grep -q yes; then
			mkdir -p "$dir"
			"$ADB" pull "$p" "$dir/" >/dev/null && echo "== saved $p to $dir/"
		fi
	done
}

edl_present() {
	if [ "$(uname)" = Darwin ]; then
		ioreg -p IOUSB -w0 | grep -q QUSB
	else
		lsusb -d 05c6:9008 >/dev/null 2>&1
	fi
}

flash_linux() {
	cat <<-EOF

	== Put the board in EDL mode now:
	   1. Unplug the USB-C cable.
	   2. Put a jumper on the two EDL pins (right-most column of JCTL, next to
	      the USB-C connector; diagram in dist/README.md).
	   3. Plug the board DIRECTLY into this computer (no hub or dock).
	   Waiting for the board to show up in EDL mode...
	EOF
	local t=0
	until edl_present; do
		sleep 2
		t=$((t + 2))
		[ $t -lt 1800 ] || die "no board in EDL mode after 30 minutes"
	done
	echo "== board in EDL mode, flashing $IMAGE"
	"$FLASHER" flash unoq "$IMAGE" --yes || die "flashing failed. It can always be retried: the EDL loader is in the
   chip's ROM. 'qdl: bulk write failed' usually means a hub or a loose jumper.
   Re-run this script (the image is cached)."
	cat <<-EOF

	== Linux image flashed. Now:
	   1. Remove the EDL jumper.
	   2. Unplug the USB-C cable and plug it back in.
	   Waiting for the board to boot (the first boot takes a few minutes)...
	EOF
}

wait_board() {
	local t=0
	until board_up; do
		sleep 3
		t=$((t + 3))
		[ $t -lt 1200 ] || die "board didn't come up on adb within 20 minutes; restore the STM32 later with: $0 --mcu-only"
	done
	# systemd finishes booting after adbd comes up.
	"$ADB" shell 'for i in $(seq 90); do s=$(systemctl is-system-running 2>/dev/null); case $s in running|degraded) break ;; esac; sleep 2; done'
}

restore_mcu() {
	wait_board
	echo "== restoring the STM32 sketch loader (arduino-cli burn-bootloader)"
	# The QStep HAL driver talks to the MCU over SPI; stop it if QStep is still there.
	"$ADB" shell 'systemctl stop qstep-linuxcnc.service 2>/dev/null; true'
	# arduino-cli and the Zephyr core live in the arduino user's home. adb's
	# shell is root on Arduino's images; setpriv skips PAM, which would ask for
	# the arduino account's pending password change. adb's TMPDIR doesn't exist.
	local out
	out=$("$ADB" shell 'cd /home/arduino && cmd="arduino-cli burn-bootloader -b arduino:zephyr:unoq -P jlink";
		if [ "$(id -u)" = 0 ]; then
			setpriv --reuid=arduino --regid=arduino --init-groups \
				env HOME=/home/arduino USER=arduino TMPDIR=/tmp $cmd
		else
			$cmd
		fi 2>&1; echo "exit=$?"' | tr -d '\r')
	echo "$out" | sed 's/^/   /'
	echo "$out" | grep -q '^exit=0$' || die "burn-bootloader failed"
}

report() {
	echo "== board now runs:"
	"$ADB" shell 'echo "   kernel $(uname -r)"; for s in arduino-router arduino-app-cli; do echo "   $s: $(systemctl is-active $s)"; done;
		[ -e /etc/qstep ] || [ -e /usr/lib/linuxcnc/modules/unoq_spi.so ] && echo "   (QStep is still installed on Linux)"; true'
}

if [ "$MCU_ONLY" = 1 ]; then
	restore_mcu
	report
	exit 0
fi

get_image
if [ "$YES" != 1 ]; then
	printf '\nThis ERASES everything on the UNO Q and flashes %s.\nType "yes" to continue: ' "${IMAGE##*/}"
	read -r ans
	[ "$ans" = yes ] || die "cancelled"
fi
backup_board
flash_linux
restore_mcu
report
echo "== done: Arduino's stock image and STM32 loader"
