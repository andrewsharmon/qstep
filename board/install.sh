#!/bin/sh
# ArduCNC installer, run ON the UNO Q as root, from a staged copy of this repo
# (tools/deploy.sh stages it to /root/arducnc-stage and runs this script).
#
#   install.sh [--dry-run] [--packages] [--kernel] [--system] [--hal] [--config] [--firmware] | --all
#
#   --packages  apt: linuxcnc-uspace, rt-tests, xvfb, x11vnc (needs network, e.g. the adb proxy)
#   --kernel    PREEMPT_RT kernel .deb from dist/kernel, CPU isolation cmdline, boot counting
#               (takes effect after a PHYSICAL power cycle; adb reboot boots a stale ESP)
#   --system    udev rules, boot/RT tuning service, VNC display services, LinuxCNC service,
#               helper scripts; disables arduino-router/arduino-app-cli (they own the MCU link)
#   --hal       unoq_spi.so from dist/hal into LinuxCNC's module directory
#   --config    LinuxCNC config into /home/arduino/arducnc-config (keeps linuxcnc.var)
#   --firmware  back up the STM32 flash (once), then flash dist/firmware/arducnc-fw.elf
#   --all       everything above, in that order
#
# Nothing here needs a build: all binaries come from dist/.
set -eu

STAGE=$(cd "$(dirname "$0")/.." && pwd)
DIST=$STAGE/dist
CFG_DST=/home/arduino/arducnc-config
KVER=6.16.0-rt-arducnc2
ISOL="isolcpus=3 nohz_full=3 rcu_nocbs=3 irqaffinity=0-2"
DRY=0
STEPS=""

for a in "$@"; do
	case "$a" in
	--dry-run) DRY=1 ;;
	--all) STEPS="packages kernel system hal config firmware" ;;
	--packages|--kernel|--system|--hal|--config|--firmware) STEPS="$STEPS ${a#--}" ;;
	*) sed -n '2,20p' "$0"; exit 2 ;;
	esac
done
[ -n "$STEPS" ] || { sed -n '2,20p' "$0"; exit 2; }
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

run() {
	echo "+ $*"
	[ "$DRY" = 1 ] || sh -c "$*"
}

step_packages() {
	echo "== packages"
	run "apt-get update -qq"
	run "DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends -t trixie linuxcnc-uspace rt-tests xvfb x11vnc"
	run "apt-get clean"
}

step_kernel() {
	echo "== kernel $KVER"
	deb=$DIST/kernel/linux-image-${KVER}_1_arm64.deb
	[ -f "$deb" ] || { echo "missing $deb"; exit 1; }
	# Keep the stock kernel and meta package from being replaced by updates.
	run "apt-mark hold linux-image-arm64 \$(dpkg-query -W -f='\${Package}\n' 'linux-image-6.16.0-g*' 2>/dev/null) >/dev/null || true"
	# New entries get 2 tries: a kernel that fails to boot twice falls back to the previous one.
	run "echo 2 > /etc/kernel/tries"
	# Kernel command line for new entries = current one (minus the initrd= prefix) + CPU isolation.
	cmdline=$(sed -e 's/^initrd=[^ ]* //' -e 's/ isolcpus=[^ ]*//; s/ nohz_full=[^ ]*//; s/ rcu_nocbs=[^ ]*//; s/ irqaffinity=[^ ]*//' /proc/cmdline)
	run "echo '$cmdline $ISOL' > /etc/kernel/cmdline"
	run "dpkg -i '$deb'"
	run "sync"
	echo "   kernel installed; POWER-CYCLE the board (unplug/replug) to boot it"
}

step_system() {
	echo "== system services"
	b=$STAGE/board
	run "install -m 644 $b/60-arducnc.rules /etc/udev/rules.d/"
	run "udevadm control --reload && udevadm trigger --name-match=spidev0.0 && udevadm trigger --name-match=cpu_dma_latency"
	run "install -m 755 $b/arducnc-rt-tune /usr/local/sbin/"
	run "install -m 755 $b/lcnc-ctl $b/soak-start /usr/local/bin/"
	run "install -m 644 $b/arducnc-rt-tune.service $b/arducnc-xvfb.service $b/arducnc-vnc.service $b/arducnc-linuxcnc.service /etc/systemd/system/"
	run "mkdir -p /etc/arducnc && chown arduino:arduino /etc/arducnc && chmod 700 /etc/arducnc"
	# LinuxCNC 2.9 only trusts /sys/kernel/realtime, which mainline PREEMPT_RT lacks.
	run "grep -q '^LINUXCNC_FORCE_REALTIME=1' /etc/environment || echo LINUXCNC_FORCE_REALTIME=1 >> /etc/environment"
	run "printf '# LinuxCNC 2.9 looks for /sys/kernel/realtime, which mainline PREEMPT_RT no longer provides.\nexport LINUXCNC_FORCE_REALTIME=1\n' > /etc/profile.d/linuxcnc-rt.sh"
	# arduino-router owns the MCU UART and BOOT0; arducnc-rt-tune now holds BOOT0 low instead.
	run "systemctl disable --now arduino-router.service arduino-app-cli.service 2>/dev/null || true"
	run "systemctl daemon-reload"
	run "systemctl enable --now arducnc-rt-tune.service arducnc-xvfb.service arducnc-vnc.service"
	run "systemctl enable arducnc-linuxcnc.service"
	echo "   VNC has no password until you run: x11vnc -storepasswd /etc/arducnc/vnc.pass (then systemctl restart arducnc-vnc)"
}

step_hal() {
	echo "== HAL module"
	run "install -m 644 $DIST/hal/unoq_spi.so /usr/lib/linuxcnc/modules/"
}

step_config() {
	echo "== LinuxCNC config -> $CFG_DST"
	src=$STAGE/configs/unoq-shield
	run "mkdir -p $CFG_DST/nc_files"
	for f in unoq-shield.ini unoq-shield-headless.ini unoq-shield.hal sim-loopback.ini sim-loopback.hal tool.tbl; do
		run "install -m 644 $src/$f $CFG_DST/"
	done
	for f in headless-display mdi-test.py hold-test.py run-program.py soak-test.py soak-overnight.py; do
		run "install -m 755 $src/$f $CFG_DST/"
	done
	run "install -m 644 $src/nc_files/*.ngc $CFG_DST/nc_files/"
	run "[ -f $CFG_DST/linuxcnc.var ] || install -m 664 $src/linuxcnc.var $CFG_DST/"
	run "install -m 644 $src/axisrc /home/arduino/.axisrc"
	run "chown -R arduino:arduino $CFG_DST /home/arduino/.axisrc"
}

step_firmware() {
	echo "== STM32 firmware"
	ocd="cd /opt/openocd && ./bin/openocd -s /opt/openocd -f openocd_gpiod.cfg"
	run "mkdir -p /root/arducnc"
	# One-time backup of whatever is on the MCU now (the stock Arduino loader on a fresh board).
	if [ ! -f /root/arducnc/mcu-flash-backup.bin ]; then
		run "$ocd -c 'init; reset halt; dump_image /root/arducnc/mcu-flash-backup.bin 0x08000000 0x200000; reset run; shutdown'"
	else
		echo "   (MCU backup already at /root/arducnc/mcu-flash-backup.bin)"
	fi
	# BOOT0 low, or the next reset lands in the ST ROM bootloader.
	run "gpioset -c /dev/gpiochip1 -t0 37=0"
	run "systemctl stop arducnc-linuxcnc.service 2>/dev/null || true"
	run "$ocd -c 'program $DIST/firmware/arducnc-fw.elf verify reset exit' 2>&1 | grep -E 'Verified OK|Error: (flash|Verification)'"
	echo "   firmware flashed; LinuxCNC can be started again with: lcnc-ctl start"
}

for s in $STEPS; do
	"step_$s"
done
echo "done: $STEPS"
