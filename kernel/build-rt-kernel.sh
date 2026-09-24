#!/bin/bash
# Build a PREEMPT_RT kernel .deb for the Arduino UNO Q.
# Runs inside the Debian 13 arm64 Lima VM (limactl shell qstep).
# Source: arduino/linux-qcom at the exact commit the board ships (6.16.0-geffa8626771a),
# config: the board's own /boot/config, plus kernel/rt.config on top.
set -euo pipefail

PROJ=$(cd "$(dirname "$0")/.." && pwd)
COMMIT=${COMMIT:-effa8626771ad31536aafd3a5aa94cad7e528e23}
BASECFG=${BASECFG:-$PROJ/kernel/config-6.16.0-geffa8626771a.stock}
WORK=$HOME/kbuild
OUT=$PROJ/kernel/out

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    build-essential bc bison flex libssl-dev libelf-dev libncurses-dev dwarves \
    kmod cpio rsync debhelper git python3 libdw-dev >/dev/null

mkdir -p "$WORK" "$OUT"
if [ ! -d "$WORK/linux/.git" ]; then
    git init -q "$WORK/linux"
    git -C "$WORK/linux" remote add origin https://github.com/arduino/linux-qcom.git
fi
cd "$WORK/linux"
git fetch -q --depth 1 origin "$COMMIT"
git checkout -q -f FETCH_HEAD
for p in "$PROJ"/kernel/patches/*.patch; do
    [ -e "$p" ] || continue
    echo "applying $(basename "$p")"
    git apply "$p"
done

cp "$BASECFG" .config
scripts/kconfig/merge_config.sh -m .config "$PROJ/kernel/rt.config" >/dev/null
make olddefconfig >/dev/null
# Keep the running kernel's module/DTB naming scheme but mark it as RT.
./scripts/config --set-str LOCALVERSION "-rt-qstep${KVER_SUFFIX:-}" --disable LOCALVERSION_AUTO
./scripts/config --disable DEBUG_INFO_BTF --disable DEBUG_INFO \
    --disable DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT --enable DEBUG_INFO_NONE
make olddefconfig >/dev/null

echo "== RT-relevant config =="
grep -E '^CONFIG_(PREEMPT_RT|PREEMPT|HZ|NO_HZ_FULL|RCU_NOCB_CPU|CPU_FREQ_DEFAULT_GOV_PERFORMANCE)=' .config

# Neutral build identity (no local user/host names in uname -v or the .deb).
export KBUILD_BUILD_USER=qstep KBUILD_BUILD_HOST=qstep KBUILD_BUILD_VERSION=1
export KBUILD_BUILD_TIMESTAMP="${KBUILD_BUILD_TIMESTAMP:-$(git -C "$PROJ" log -1 --format=%cd --date=rfc 2>/dev/null || date -R)}"
export DEBEMAIL="noreply@qstep.invalid" DEBFULLNAME="QStep"
time make -j"$(nproc)" LOCALVERSION= KDEB_PKGVERSION="${KDEB_PKGVERSION:-1}" bindeb-pkg
cp -v ../linux-image-*-rt-qstep*.deb ../linux-headers-*-rt-qstep*.deb "$OUT"/ 2>/dev/null || true
cp .config "$OUT/config-rt-qstep"
ls -la "$OUT"
