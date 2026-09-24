#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 andrewsharmon
# Build a flashable QStep image for the Arduino UNO Q.
#
# FOR LOCAL / PRIVATE USE ONLY. The result contains Arduino's complete software
# image and Qualcomm boot firmware, which QStep doesn't redistribute. The public
# install path is the two-step install in dist/README.md (Arduino's own image, then
# board/bootstrap.sh).
#
# Takes Arduino's official Debian image (the release whose kernel our RT kernel
# is built from), installs everything from dist/ into it with
# `IMAGE=1 board/install.sh` in an arm64 chroot, and repackages it in the same
# layout, so it can be flashed with Arduino's own tool:
#
#     arduino-flasher-cli flash unoq ./qstep-unoq-image-<version>.tar.zst
#
# Runs in the Debian 13 arm64 Lima VM (native chroot, needs sudo and internet):
#     limactl shell qstep -- /Users/.../qstep/image/build-image.sh
set -euo pipefail

PROJ=$(cd "$(dirname "$0")/.." && pwd)
BASE_VER=${BASE_VER:-20250807-136}
BASE_URL=https://downloads.arduino.cc/debian-im/Stable/$BASE_VER/arduino-unoq-debian-image-$BASE_VER.tar.zst
BASE_SHA=${BASE_SHA:-9269521730db68752323a73b290041ffb33a1c4e24ec4dcee7c3d365bc8424fc}
VERSION=${VERSION:-$(date +%Y%m%d)-$(git -C "$PROJ" rev-parse --short HEAD 2>/dev/null || echo local)}
NAME=qstep-unoq-image-$VERSION
WORK=${WORK:-$HOME/img}
OUT=${OUT:-$PROJ/dist/image}
R=/mnt/qstep-root

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq zstd e2fsprogs zerofree >/dev/null

# ---- base image ------------------------------------------------------------
mkdir -p "$WORK"
cd "$WORK"
if [ ! -f base-$BASE_VER.tar.zst ]; then
	echo "== downloading $BASE_URL"
	curl -fSLo base-$BASE_VER.tar.zst.part "$BASE_URL"
	mv base-$BASE_VER.tar.zst.part base-$BASE_VER.tar.zst
fi
echo "$BASE_SHA  base-$BASE_VER.tar.zst" | sha256sum -c -
BASE_DIR=$WORK/base-$BASE_VER/arduino-unoq-debian-image-$BASE_VER
if [ ! -d "$BASE_DIR" ]; then
	mkdir -p base-$BASE_VER
	tar -I zstd -xf base-$BASE_VER.tar.zst -C base-$BASE_VER
fi

# ---- working copy ----------------------------------------------------------
B=$WORK/$NAME
rm -rf "$B"
mkdir -p "$B"
cp -r "$BASE_DIR/flash_UnoQ" "$B/"
cp --sparse=always "$BASE_DIR/disk-sdcard.img1" "$B/"
cp --sparse=always "$BASE_DIR/disk-sdcard.img2" "$B/"
cp --sparse=always "$BASE_DIR/disk-sdcard.img3" "$B/"

# Grow the root filesystem to its full partition size (from rawprogram0.xml):
# the stock image leaves <1 GB free, LinuxCNC needs more.
root_kb=$(grep -oE 'size_in_KB="[0-9.]+"[^>]*label="rootfs"' "$B/flash_UnoQ/rawprogram0.xml" \
	| grep -oE '[0-9]+' | head -1)
truncate -s $((root_kb * 1024)) "$B/disk-sdcard.img2"
e2fsck -fy "$B/disk-sdcard.img2" >/dev/null || true
resize2fs "$B/disk-sdcard.img2" >/dev/null

# ---- chroot install ----------------------------------------------------------
cleanup() {
	set +e
	for m in run sys/firmware/efi/efivars sys proc dev/pts dev boot/efi; do
		mountpoint -q "$R/$m" && sudo umount -l "$R/$m"
	done
	mountpoint -q "$R" && sudo umount "$R"
}
trap cleanup EXIT

sudo mkdir -p $R
sudo mount -o loop "$B/disk-sdcard.img2" $R
sudo mount -o loop "$B/disk-sdcard.img1" $R/boot/efi
sudo mount --bind /dev $R/dev
sudo mount --bind /dev/pts $R/dev/pts
sudo mount -t proc proc $R/proc
sudo mount -t sysfs sys $R/sys
sudo mount -t tmpfs tmpfs $R/run

sudo cp $R/etc/resolv.conf $WORK/resolv.conf.orig 2>/dev/null || true
sudo rm -f $R/etc/resolv.conf
sudo cp /etc/resolv.conf $R/etc/resolv.conf
# Don't start services inside the chroot.
printf '#!/bin/sh\nexit 101\n' | sudo tee $R/usr/sbin/policy-rc.d >/dev/null
sudo chmod 755 $R/usr/sbin/policy-rc.d

sudo mkdir -p $R/opt/qstep/src
sudo cp -r "$PROJ/dist" "$PROJ/board" "$PROJ/configs" $R/opt/qstep/src/
sudo rm -rf $R/opt/qstep/src/dist/image

# kernel-install cannot probe a loop-mounted ESP; tell it where it is.
# (u-boot-efi-dtb still prints "EFI partition not found" because /proc/mounts
# shows the host path; harmless: the image's ESP already holds the identical
# DTBs, which the checks below verify.)
sudo chroot $R /usr/bin/env IMAGE=1 SYSTEMD_ESP_PATH=/boot/efi BOOT_ROOT=/boot/efi \
	SYSTEMD_RELAX_ESP_CHECKS=1 DEBIAN_FRONTEND=noninteractive \
	sh /opt/qstep/src/board/install.sh --packages --kernel --system --hal --config --firmware

# Record what went in.
{
	echo "QStep image $VERSION"
	echo "base: Arduino UNO Q Debian image $BASE_VER ($BASE_SHA)"
	echo "source: $(git -C "$PROJ" rev-parse HEAD 2>/dev/null || echo unknown)"
	grep -E '^[0-9a-f]{64}' "$PROJ/dist/SHA256SUMS"
} | sudo tee $R/opt/qstep/VERSION >/dev/null

# ---- checks ------------------------------------------------------------------
echo "== checks"
entry=$(ls $R/boot/efi/loader/entries/ | grep rt-qstep1)
echo "boot entry: $entry"
grep '^options' "$R/boot/efi/loader/entries/$entry"
grep -q 'isolcpus=3' "$R/boot/efi/loader/entries/$entry"
grep -q '^timeout 3' $R/boot/efi/loader/loader.conf
test -f $R/usr/lib/linuxcnc/modules/unoq_spi.so
test -f $R/opt/qstep/firmware/qstep-fw.elf
test -d $R/opt/qstep/config
for u in qstep-rt-tune qstep-xvfb qstep-vnc qstep-linuxcnc qstep-firstboot; do
	test -L $R/etc/systemd/system/multi-user.target.wants/$u.service || { echo "$u not enabled"; exit 1; }
done
! test -e $R/etc/systemd/system/multi-user.target.wants/arduino-router.service
sudo chroot $R dpkg-query -W linuxcnc-uspace xvfb x11vnc rt-tests "linux-image-6.16.0-rt-qstep1"
kdtb=$(ls $R/usr/lib/linux-image-6.16.0-rt-qstep1/qcom/qrb2210-arduino-imola.dtb)
cmp "$kdtb" $R/boot/efi/dtb/qcom/qrb2210-arduino-imola.dtb && echo "ESP DTB matches the RT kernel's DTB"

# ---- tidy ----------------------------------------------------------------------
sudo rm -f $R/usr/sbin/policy-rc.d
sudo rm -f $R/etc/resolv.conf
if [ -e $WORK/resolv.conf.orig ] || [ -L $WORK/resolv.conf.orig ]; then
	sudo cp -a $WORK/resolv.conf.orig $R/etc/resolv.conf
fi
sudo rm -rf $R/opt/qstep/src/dist/kernel
sudo chroot $R apt-get clean
sudo rm -rf $R/var/lib/apt/lists/* $R/tmp/* $R/var/tmp/*
cleanup
trap - EXIT

# Zero free blocks so the archive compresses well.
e2fsck -fy "$B/disk-sdcard.img2" >/dev/null || true
sudo zerofree "$B/disk-sdcard.img2"

# ---- package -----------------------------------------------------------------
mkdir -p "$OUT"
echo "== packaging $OUT/$NAME.tar.zst"
tar -C "$WORK" --sparse -cf - "$NAME" | zstd -T0 -12 -q -o "$OUT/$NAME.tar.zst" -f
(cd "$OUT" && sha256sum "$NAME.tar.zst" > "$NAME.tar.zst.sha256")
ls -la "$OUT"
