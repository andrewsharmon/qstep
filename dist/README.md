# QStep prebuilt files

Everything a UNO Q needs, already built. Installing QStep never requires
building the kernel, the STM32 firmware or the HAL driver, and never requires
build tools on your computer.

| File | What | Built from |
|---|---|---|
| `kernel/linux-image-6.16.0-rt-qstep1_1_arm64.deb` | PREEMPT_RT kernel for the UNO Q (Arduino's 6.16 `effa862` + `kernel/rt.config` + `kernel/patches/`) | `kernel/build-rt-kernel.sh` (`KVER_SUFFIX=1 KDEB_PKGVERSION=1`) |
| `kernel/linux-headers-6.16.0-rt-qstep1_1_arm64.deb` | Matching headers (only needed to build out-of-tree modules) | same |
| `hal/unoq_spi.so` | LinuxCNC HAL driver for **linuxcnc-uspace 2.9.4 (Debian 13, arm64)** | `hal/unoq_spi.c`, `hal/Makefile` |
| `firmware/qstep-fw.elf` / `.bin` | STM32U585 firmware (flash at 0x08000000), CNC Shield v3 pinout; the ELF has debug info stripped | `firmware/qstep-fw` (Zephyr, board `arduino_uno_q`) |
| `SHA256SUMS` | checksums, verified by every installer | |

All binaries are built with neutral names (`qstep@qstep`, relative source
paths), so they don't carry any builder's user name or directories.

## Install (two steps)

Only tested with one axis and one stepper, a generic "CNC SHIELD" Ver 3.00 and a
DRV8825 module (see [TODO.md](../TODO.md)).

**Step 1: flash Arduino's official image** with Arduino's own
[Flasher CLI](https://docs.arduino.cc/software/app-lab/configure/flash/). QStep's
RT kernel is built for exactly this release:

```bash
arduino-flasher-cli flash unoq --version 20250807-136
```

The flasher needs the board in **EDL mode** (Qualcomm's download mode). Short the
two **EDL pins** while plugging in the USB-C cable. Board seen from the component
side, laid out like the photo in
[Arduino's flashing guide](https://docs.arduino.cc/software/app-lab/configure/flash/):

```
  USB-C on the LEFT edge, LED matrix bottom-right

  +------------------------------------------------------------------+
  | [POWER]  JCTL           :::::::::::::::::::::::  (top header)    |
  |  button  o o o (o) <--+                                          |
  |          o o o (o) <--+-- EDL pins: right-most column of JCTL,   |
  | +-----+                   one pin in each row. Short these two.  |
  | |USB-C|                                                          |
  | |     |                                                QWIIC     |
  | +-----+                                                          |
  |                                                                  |
  |    ARDUINO                      +--------------------+           |
  |     UNO Q                       |  LED matrix  13x8  |           |
  |                                 +--------------------+           |
  |            :::::::::::::::::::::::::  (bottom header)            |
  +------------------------------------------------------------------+
```

1. Unplug the board. Put a jumper cap or a female-female jumper wire on the two EDL
   pins, then plug the USB-C cable in. On a Mac the board then shows up as
   `QUSB_BULK…` instead of `Uno Q`, and the flasher starts writing.
2. Leave the jumper on until flashing finishes, then remove it and unplug/replug
   the board to boot the new image.
3. Connect the board **directly to a USB port on the computer**, not through a hub
   or dock. Flashing through a hub can fail part-way with
   `qdl: bulk write failed: Input/Output Error`. If that happens, stop the flasher and
   repeat from 1. The EDL loader lives in the chip's ROM, so a failed flash can
   always be retried.

Go through Arduino's first-boot setup so the board has Wi-Fi (internet is needed
for the Debian packages in step 2). So far the install has only been tested with
the board online through a proxy over USB (see the bring-up log), not over Wi-Fi.

**Step 2: install QStep on the board.** Get a root shell on the board (from your
computer: `adb shell`, or `sudo -i` in a terminal on the board), download the
bootstrap script from the latest release, and run it:

```bash
curl -fsSLO https://github.com/andrewsharmon/qstep/releases/latest/download/qstep-bootstrap.sh
sh qstep-bootstrap.sh
```

For a specific release, use `.../releases/download/v<version>/qstep-bootstrap.sh`
instead. Each bootstrap script installs the release it came with.

It checks that the board runs the right Arduino image, then downloads and verifies
the release bundle and installs everything:
- Debian packages (LinuxCNC 2.9.4, Xvfb, x11vnc, rt-tests);
- the RT kernel;
- the services and the HAL driver;
- the machine config.

It also backs up the STM32's flash to `/root/qstep/mcu-flash-backup.bin` and
flashes the QStep firmware. Arduino's flasher doesn't touch the STM32, so on a
board that has run QStep before, that backup holds the older QStep firmware
rather than Arduino's.

Then **power-cycle** the board (unplug and replug; a warm reboot doesn't pick up
the new kernel on the UNO Q). LinuxCNC + AXIS start automatically on display :1.

AXIS is shown over VNC, which **stays off until you set a VNC password**. There's
no no-password mode. As root on the board, run this and type the password twice
(VNC only uses the first 8 characters):

```bash
qstep-vnc-passwd
```

The VNC server only listens on the board's localhost. Connect from your computer
through adb (or an SSH tunnel to port 5900):

```bash
adb forward tcp:5900 tcp:5900
```

```bash
open vnc://127.0.0.1:5900
```

If VNC doesn't come up, `journalctl -u qstep-vnc` says why.

Offline alternative: copy `qstep-<version>.tar.gz` (and its `.sha256`) to the board
and run `sh qstep-bootstrap.sh --bundle qstep-<version>.tar.gz`.

## Developers

- `tools/deploy.sh` pushes a working tree to a board over adb and runs `board/install.sh`.
- `tools/make-release.sh <version>` builds the release bundle and bootstrap script
  for upload.
- `image/build-image.sh` can build a single flashable image, but only **for local or
  private use**. That image contains Arduino's full software image and Qualcomm
  firmware, so QStep doesn't publish it; the two-step install is the public path.
