# ArduCNC prebuilt files

Everything a UNO Q needs, already built. Nothing here requires rebuilding the
kernel, the STM32 firmware or the HAL driver.

| File | What | Built from |
|---|---|---|
| `kernel/linux-image-6.16.0-rt-arducnc2_1_arm64.deb` | PREEMPT_RT kernel for the UNO Q (Arduino's 6.16 `effa862` + `kernel/rt.config` + `kernel/patches/`) | `kernel/build-rt-kernel.sh` (`KVER_SUFFIX=2`) |
| `kernel/linux-headers-6.16.0-rt-arducnc2_1_arm64.deb` | Matching headers (only needed to build out-of-tree modules) | same |
| `hal/unoq_spi.so` | LinuxCNC HAL driver for **linuxcnc-uspace 2.9.4 (Debian 13, arm64)** | `hal/unoq_spi.c`, `hal/Makefile` |
| `firmware/arducnc-fw.elf` / `.bin` | STM32U585 firmware (flash at 0x08000000), CNC Shield v3 pinout | `firmware/arducnc-fw` (Zephyr, board `arduino_uno_q`) |
| `SHA256SUMS` | checksums, verified by `tools/deploy.sh` | |

## Flash a complete image (easiest)

`image/build-image.sh` produces `dist/image/arducnc-unoq-image-<date>-<commit>.tar.zst`
(~2.6 GB, not stored in git; publish it as a release asset). It is Arduino's
official UNO Q image **20250807-136** with everything below preinstalled.
Flash it with Arduino's own tool; no build tools needed:

```bash
arduino-flasher-cli flash unoq ./arducnc-unoq-image-<date>-<commit>.tar.zst
```

Then unplug/replug. On first boot `arducnc-firstboot` copies the LinuxCNC config
to `/home/arduino/arducnc-config`, backs up the STM32 flash to
`/var/lib/arducnc/mcu-flash-backup.bin` and flashes the ArduCNC firmware;
LinuxCNC + AXIS then start on display :1 (VNC, see below) and restart when closed.

## Install onto an existing board

With the board on USB (adb) and the Lima VM proxy running for `--packages`:

```bash
tools/deploy.sh --dry-run --all   # show every step
tools/deploy.sh --all             # packages, kernel, system services, HAL, config, firmware
```

Then **power-cycle** the board (unplug/replug; `adb reboot` boots a stale EFI view)
to start the RT kernel, and set a VNC password on the board:
`x11vnc -storepasswd /etc/arducnc/vnc.pass && systemctl restart arducnc-vnc`.

Individual steps: `--packages --kernel --system --hal --config --firmware` (see `board/install.sh`).
The first `--firmware` run saves the MCU's original flash to `/root/arducnc/mcu-flash-backup.bin`.
