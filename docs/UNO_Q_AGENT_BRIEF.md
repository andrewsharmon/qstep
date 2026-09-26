# Arduino UNO Q: brief for coding agents

What was learned bringing up QStep (LinuxCNC + PREEMPT_RT + custom STM32
firmware) on an Arduino UNO Q, written for the next agent that works on this
board. Most of it applies to any UNO Q project, not only QStep. The evidence and
measurements are in [BRINGUP_LOG.md](BRINGUP_LOG.md), and the pre-hardware study
is in [PORTING_ASSESSMENT.md](PORTING_ASSESSMENT.md). Facts here were measured on
one board (2 GB / 16 GB, Arduino image 20250807-136) between 2026-09-23 and
2026-09-25, unless marked otherwise.

## 1. Rules of engagement (read first)

1. **Assume someone is using the board.** Ask before running anything against it.
   If the owner says it's in use, run nothing, not even read-only `adb shell`
   checks. Write out the commands you want to run and why, then wait for
   approval. Anything that changes state (flash, install, reboot, start/stop
   services, write GPIOs) always needs a check-in first.
2. **Motion needs explicit approval every time.** Someone may be jogging the
   machine in AXIS over VNC. Two sessions driving it at once have already sent Y
   about 20 revolutions after a scripted absolute `G0 Y0.1` (see the bring-up
   log). Use tiny **relative** moves (`G91`) only, never absolute targets.
3. **Physical steps need a person:** power cycles, the EDL jumper, cabling,
   moving the board to a direct USB port. Ask, then wait for them to confirm.
4. **Never set or change the `arduino` account's password**, and never store
   passwords (VNC or otherwise) in the repo.
5. **Prefer Arduino's own tools** where they exist (flasher CLI, `arduino-cli
   burn-bootloader`) over custom ones.
6. **Back up before you overwrite.** Dump the STM32 flash before the first flash.
   Arduino firmware dumps and Arduino/Qualcomm images stay out of this public repo
   (they live in the local-only `../qstep-private/`).
7. **Check the board's state before assuming anything.** `uname -r` tells you
   what it runs: `6.16.0-geffa8626771a` is stock Arduino, `6.16.0-rt-qstep1` is QStep.
8. **Keep bench-specific notes out of this file.** Things like the current board
   state, local VM names and attached equipment belong in local, untracked notes.

## 2. The board in one table

| | |
|---|---|
| MPU | Qualcomm QRB2210, 4x Cortex-A53 @ 2.0 GHz, Adreno 702, 2 or 4 GB RAM, 16 or 32 GB eMMC (root partition ~9.3 GB on 16 GB) |
| MCU | STM32U585, Cortex-M33 @ 160 MHz, 2 MB flash (dual bank), 786 KB SRAM. RDP 0, TrustZone off |
| Header pins | **All UNO header pins belong to the STM32**, not Linux. 3.3 V logic |
| Stock OS | Debian 13 arm64, kernel `6.16.0-geffa8626771a` (`CONFIG_PREEMPT`, **not RT**), HZ=250, schedutil. Newer Arduino images move to a 7.0 kernel (untested here) |
| MPU to MCU, UART | LPUART1 to Linux `/dev/ttyHS1`, 115200. Stock use: `arduino-router` (msgpack-RPC "Bridge", Go daemon, `/var/run/arduino-router.sock`) |
| MPU to MCU, SPI | Linux `spi5` to `/dev/spidev0.0` (DT `compatible = "arduino,unoq-mcu"`), GENI SE at `4a94000`. MCU side: SPI3 slave, SCK PG9, MISO PG10, MOSI PB5, NSS PG12, "SPI RDY" PG13. Controller default max 50 MHz |
| MCU BOOT0 | Driven by Linux: **gpiochip1 line 37**. Must be low, or the next MCU reset boots the ST ROM bootloader (PC = 0x0BF9xxxx). FLASH_OPTR = 0x1FEFF8AA (nSWBOOT0=1: the pin decides) |
| MCU debug | SWD bit-banged from Linux GPIOs. OpenOCD ships on the board in `/opt/openocd` (`openocd_gpiod.cfg`). No external probe needed |
| Stock MCU flash | Arduino Zephyr loader at 0x08000000 (no MCUboot), boot animation at 0x080D0000, sketch LLEXT at 0x080F0000 |
| LED matrix | 13x8, charlieplexed on PF0-PF10 (pin pairs in `ArduinoCore-zephyr/loader/matrix.inc`) |
| USB gadget | adb + ACM (`ttyGS0`) only. **No USB network function** |
| Clock | **No RTC backup**, and no NTP without internet. The clock is wrong after every power cycle |

## 3. Getting in from the Mac

Local tools (gitignored downloads): `tools/platform-tools/adb`,
`tools/bin/limactl`, `tools/flasher/arduino-flasher-cli`.

- **adb shell is root**, on the stock image too.
- **Without Wi-Fi, apt can reach the internet through the Mac.** The Lima VM
  runs tinyproxy on 3128 (`Port 3128`, `Allow 127.0.0.1`), Lima forwards it to
  the Mac's localhost, and adb reverses it onto the board:

  ```bash
  adb reverse tcp:3128 tcp:3128        # again after every reconnect
  adb shell "date -u -s '$(date -u '+%Y-%m-%d %H:%M:%S')'"   # $(...) expands on the Mac
  # /etc/apt/apt.conf.d/90qstep-proxy on the board:
  #   Acquire::http::Proxy "http://127.0.0.1:3128";
  #   Acquire::https::Proxy "http://127.0.0.1:3128";
  ```
  A wrong clock breaks TLS and apt, so set it first. For large files, `adb push` is
  simpler than proxying curl.
- **apt:** never a blind `apt upgrade`. The sources include Debian `experimental`
  and Arduino's repo. Install with `-t trixie --no-install-recommends` and keep the
  kernel packages on `apt-mark hold`.
- **VNC/GUI:** `adb forward tcp:5900 tcp:5900`, then `open vnc://127.0.0.1:5900`.
  Use IPv4, because adb's forward doesn't listen on `::1`. macOS Screen Sharing
  refuses a VNC server without a password.

### Lima VM (the build machine)

- Debian 13 arm64. The repo scripts take the VM name from `QSTEP_VM` (default
  `qstep`).
- Mount the repo **writable at the same path** inside the VM, so scripts can
  pass Mac paths straight through (`limactl shell "$QSTEP_VM" -- bash -lc '...'`).
- Builds that happen there: the RT kernel (`kernel/build-rt-kernel.sh`, ~7 min),
  Zephyr firmware (`west`), the HAL module (against `linuxcnc-uspace-dev` 2.9.4),
  and the private flashable image (`image/build-image.sh`).

## 4. adb and shell traps

- **`pkill -f <pattern>` inside `adb shell '...'` kills the shell itself**, because
  the pattern is in its own command line. Use `pkill -x <name>`, a PID, or
  `systemctl stop`.
- adb exports **`TMPDIR=/data/local/tmp`**, an Android path that doesn't exist
  here. apt helpers and `arduino-cli` fail on it. Set `TMPDIR=/tmp`.
- **`adb push` over an existing file drops its exec bit.** `chmod` again.
- **The OOM killer takes `adbd` with it**, and then the board looks dead over USB.
  With 2 GB that's easy to trigger. It happened with `grep -a` over the raw eMMC.
  Never run memory-hungry tools over raw block devices.
- **Long jobs:** launch them as transient units (`systemd-run --unit=... --collect`)
  so they survive adb disconnects. Bind helper loggers with `-p BindsTo=`. See
  `board/soak-start`.
- After a reboot, adbd comes up before systemd has finished. Wait for
  `systemctl is-system-running` to report `running` or `degraded`.

## 5. Running things as `arduino`

The `arduino` account has a **forced password change pending**. That's the
owner's to do, so don't touch it. Anything that goes through PAM (`su`, `sudo
-u`, login) can stop on it. `setpriv` skips PAM:

```bash
setpriv --reuid=arduino --regid=arduino --init-groups \
    env HOME=/home/arduino USER=arduino TMPDIR=/tmp <cmd>
```

You need this for:
- **`arduino-cli`**: the Zephyr core lives in `/home/arduino/.arduino15`, and
  root's `arduino-cli` has none.
- **LinuxCNC**: it refuses to run as root, and the `RTAPI_UID` workaround keeps
  root's group list, so it can't open `gpiod`-group devices.

## 6. Boot, kernel and the ESP

- The board boots with systemd-boot from the ESP at `/boot/efi`.
- **A warm reboot (`adb reboot`) boots a stale view of the ESP.** New entries and
  `loader.conf` edits are ignored: it reported systemd-boot 257.7 while the disk
  had 257.13. A **physical power cycle** (unplug and replug USB-C) reads it
  correctly. After any change under `/boot/efi`, ask the owner to power-cycle. The
  cause is unknown (suspected firmware/U-Boot caching on the warm-reset path).
- **EFI variables are read-only** from Linux, so `bootctl set-oneshot` and
  `set-default` don't work. Use boot counting (`echo 2 > /etc/kernel/tries` before
  installing a kernel) and `loader.conf` (`timeout 3` shows the menu over
  HDMI + keyboard). A kernel that fails to boot twice falls back to the previous one.
- The kernel command line for new entries comes from `/etc/kernel/cmdline`.
- **Building a kernel:** use `arduino/linux-qcom` at the exact commit the image
  shipped (`effa8626771ad31536aafd3a5aa94cad7e528e23` for 20250807-136), the
  board's own config (`kernel/config-6.16.0-geffa8626771a.stock`), plus a fragment
  (`kernel/rt.config`), then `make bindeb-pkg`. The installed DTB came out
  byte-identical to stock.
- Set `KBUILD_BUILD_USER`/`HOST`/`TIMESTAMP`, and use `-ffile-prefix-map` for
  modules, so binaries don't carry local user names or paths.

## 7. Real-time on the QRB2210

| Measurement (cyclictest `-m -p90 -i1000`) | max latency |
|---|---|
| stock PREEMPT, idle / hackbench + eMMC dd | 376 / **2804 µs** |
| PREEMPT_RT, idle / loaded | 215 / 236 µs |
| PREEMPT_RT + `isolcpus=3 nohz_full=3 rcu_nocbs=3 irqaffinity=0-2`, load on CPUs 0-2 | **132 µs** (avg 23 µs, likely shared L2/memory bus) |

- RT fragment: `PREEMPT_RT`, `HZ_1000`, `NO_HZ_FULL`, `RCU_NOCB_CPU`, performance
  governor, plus ftrace/osnoise/timerlat for diagnosis.
- **Hold `/dev/cpu_dma_latency` at 0** in RT processes. Deep-idle exit alone
  cost milliseconds on the isolated CPU.
- **GENI SPI quirk (the big one):** `spi-geni-qcom` set the clock to
  `max_speed_hz` (50 MHz) in `prepare_message`, then to the transfer speed. Every
  change re-votes interconnect bandwidth through the RPM, which costs **~2.3 ms per
  message**. Only exactly 50 MHz avoided it. `kernel/patches/0001-...` drops the
  redundant call. After that, any speed costs ~150-180 µs of fixed overhead per
  transfer, because the controller runs in FIFO mode with 5 IRQs per 64-byte frame
  (GPI DMA mode is untried). 20 MHz is the chosen speed.
- Pin the SPI controller's IRQ to CPU3, and give its IRQ thread and the `spi0`
  kthread FIFO 85. Turn off runtime PM on
  `/sys/devices/platform/soc@0/4ac0000.geniqup/4a94000.spi`. Look up the IRQ
  number in `/proc/interrupts`; don't hard-code it. See `board/qstep-rt-tune`.
- **Don't do blocking SPI in the 1 kHz servo thread.** Under GUI memory contention
  (AXIS, llvmpipe) one `SPI_IOC_MESSAGE` took up to ~700 µs. A SCHED_FIFO 97
  worker on the same CPU does the transfer, and the servo thread posts a frame and
  reads the previous reply. Servo tmax went from ~760 µs to ~234 µs.
- Proven: 10 h soak, ~36 M frames, 0 link errors, servo tmax ≤ 483 µs of
  1000 µs, 49-55 °C.

## 8. The STM32 side

### Flash, back up, restore (all from the board, over adb)

```bash
cd /opt/openocd
OCD="./bin/openocd -s /opt/openocd -f openocd_gpiod.cfg"
$OCD -c 'init; reset halt; dump_image /root/backup.bin 0x08000000 0x200000; reset run; shutdown'        # full 2 MB, ~2.5 min
$OCD -c 'program /root/app.elf verify reset exit'                                                  # flash
$OCD -c 'init; reset halt; flash write_image erase /root/backup.bin 0x08000000 bin; reset run; shutdown' # restore dump
```
- **Only `Verified OK` means success.** A looser grep that also matched `Error:
  flash` once reported a failed flash as OK. OpenOCD also prints `Verified OK` for
  an empty image, so check checksums before flashing.
- "reset-init failed" and "Translation from khz" messages are harmless
  (linuxgpiod has no speed setting).
- **Arduino's stock restore:** `arduino-cli burn-bootloader -b arduino:zephyr:unoq
  -P jlink`, run as `arduino` (section 5). Despite the `jlink` name, it uses the
  on-board OpenOCD (`remoteocd`). It writes the loader without a mass erase. The
  QStep firmware only overwrote 0x08000000-0x08009FFF, so this alone gave a
  byte-identical stock flash.
- **Arduino's Linux flasher never touches the STM32.** After reflashing Linux,
  whatever firmware was on the MCU is still there.
- **BOOT0:** stock `arduino-router` drives line 37 low in its `ExecStartPre`. If
  you disable the router, drive it yourself: `gpioset -c /dev/gpiochip1 -t0
  37=0`. **Never `gpioget` it**, because that turns the line into an input.
- `arduino-router` and `arduino-app-cli` own the MCU link. To use `/dev/ttyHS1` or
  your own MCU protocol: `systemctl disable --now arduino-router arduino-app-cli`
  (re-enable with `enable --now`). The router logs `invalid packet, expected
  array` around MCU resets and stays up.

### Zephyr build environment (in the VM)

```bash
sudo apt-get install -y python3-venv python3-pip cmake ninja-build gperf ccache \
    dfu-util device-tree-compiler xz-utils file wget
python3 -m venv ~/zvenv && . ~/zvenv/bin/activate && pip install west
mkdir ~/zp && cd ~/zp
west init -m https://github.com/arduino/zephyr --mr "zephyr-arduino-v4.4.1+" .
west config manifest.project-filter -- "-.*,+cmsis,+cmsis_6,+hal_stm32"   # trims the workspace
west update --narrow -o=--depth=1
pip install -r zephyr/scripts/requirements-base.txt
# Zephyr SDK 1.0.1: zephyr-sdk-1.0.1_linux-aarch64_minimal.tar.xz, then ./setup.sh -t arm-zephyr-eabi -c
west build -b arduino_uno_q <app> -d ~/build-<app>
```
`tools/mcu-build-flash.sh firmware/<app>` builds, pushes and flashes in one go.
It copies the ELF out only if the build succeeded.

### Firmware patterns that worked

- **Write a native Zephyr app, not an Arduino sketch.** Sketches load as LLEXT
  modules behind Arduino's loader, and the Bridge claims SPI3 and LPUART1.
- **Console on LPUART1** (`zephyr,console = &lpuart1`) comes out on Linux at
  `/dev/ttyHS1`, 115200, once the router is stopped. It's the best debugging tool
  available. Read it with `stty -F /dev/ttyHS1 115200 raw -echo; cat /dev/ttyHS1`.
- **Keep `printk` out of hot paths.** The UART is polled, and each line blocked
  ~5 ms. That alone broke the SPI echo test. Print from a lowest-priority thread.
- **SPI3 slave with GPDMA** (U5 request lines: SPI3_RX = 10, SPI3_TX = 11,
  `CONFIG_SPI_SLAVE`, `CONFIG_SPI_STM32_DMA`) ran a 50 MHz SCLK with zero errors
  over more than 120k frames. The driver validates `.frequency` even in slave mode,
  so don't set it to 0.
- **Hard-timing ISRs:** use `ISR_DIRECT_DECLARE` + `IRQ_DIRECT_CONNECT` at
  priority 0, no kernel calls, and write GPIOs through `BSRR`. TIM6 at 100 kHz used
  ~1.3 µs per tick while stepping (worst 2.24 µs). Measure with the DWT cycle
  counter (`CONFIG_CORTEX_M_DWT`) and report the numbers to the host.
- **LED matrix:** scan it from TIM17 at the lowest IRQ priority (7), one LED at a
  time, so it can never delay real-time work.
- **Link watchdog on the MCU:** no valid frame for 20 ms → drivers off, latched
  until the host sends a frame with enable cleared. The host side drops
  `connected` and e-stops on its own when frames stop arriving.
- Keep the wire protocol in one header shared by the firmware and the host
  (`firmware/common/qstep_proto.h`): fixed 64-byte frames, CRC-16, sequence echo,
  protocol version. The host rejects any other version.

### UNO header to STM32 pins (from `firmware/qstep-fw/src/pins.h`)

| D2 | D3 | D4 | D5 | D6 | D7 | D8 | D9 | D10 | D11 | D12 | D13 | A0 | A1 | A2 | A3 | A5 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| PB3 | PB0 | PA12 | PA11 | PB1 | PB2 | PB4 | PB8 | PB9 | PB15 | PB14 | PB13 | PA4 | PA5 | PA6 | PA7 | PC0 |

LPUART1 also has RTS/CTS wired (PG5/PG6).

## 9. Reflashing Linux (EDL)

- `arduino-flasher-cli` (0.5.4 was used): `list --format json`, `download unoq
  --version V --dest-dir D`, `flash unoq <image.tar.zst | --version V> --yes`. The
  image is ~2.4 GB, so cache it (`~/.cache/qstep/arduino-images`) and check its
  sha256 against Arduino's index.
- **EDL mode:** jumper the two EDL pins (right-most column of JCTL, one pin in
  each row, next to USB-C; diagram in [dist/README.md](../dist/README.md)), then
  plug in. On a Mac the board shows up as `QUSB_BULK` (`ioreg -p IOUSB -w0 | grep
  QUSB`); on Linux as `05c6:9008`. This is an owner step.
- **Direct port, not a hub.** Otherwise it fails with `qdl: bulk write failed`.
  That error also came from a jumper on the wrong pins. A failed flash can always
  be retried, because the EDL loader is in ROM.
- Treat it as wiping the eMMC (it did on this board, `/home/arduino` included).
  Copy anything you need off first (`tools/restore-stock.sh` does). The first
  boot takes a few minutes and leaves ~3.2 GB free on root.
- `tools/restore-stock.sh` wraps the whole restore (download, EDL, first boot,
  STM32 loader). `--mcu-only` does just the STM32.

## 10. LinuxCNC 2.9 on this board (if QStep is installed)

- LinuxCNC 2.9.4 decides RT from `/sys/kernel/realtime`, which mainline
  PREEMPT_RT doesn't have. Without a fix it **silently** runs non-realtime. Set
  `LINUXCNC_FORCE_REALTIME=1`.
- Task hangs on its first command (exec_state 7) with no error unless the INI has
  `[EMCIO] EMCIO = io` + `CYCLE_TIME` and `[TASK] CYCLE_TIME`.
- Step generator `maxvel`/`maxaccel` need ~1.25x the joint limits, or decelerations
  overshoot and trip the following error.
- A position loop that chases sub-step targets dithers ±1 step. Add a deadband
  (0.5 steps).
- **AXIS ignores SIGTERM.** Stop it with `axis-remote --quit`. A SIGKILLed instance
  leaves `/tmp/linuxcnc.lock`, and the next start then waits on an **invisible Tk
  dialog** on the headless display. Recover with `halrun -U` (as arduino) and
  `rm /tmp/linuxcnc.lock`. `lcnc-ctl` handles all of this.
- Display: Xvfb :1 at 1280x800 plus x11vnc on localhost. AXIS renders in software
  (llvmpipe). With no window manager, `~/.axisrc` fills the screen.
  `x11vnc -storepasswd` run as root writes a root-only file that the `arduino`
  service can't read. Use `qstep-vnc-passwd`.
- Read-only checks (`halcmd getp/show`, `linuxcnc.stat` polls, the MCU console) are
  safe. `mdi-test.py`, `hold-test.py`, `run-program.py`, `soak-*.py` and machine
  on/off are motion: ask first (section 1).

## 11. Electrical

- The STM32 GPIOs are 3.3 V. The DRV8825 is fine with that. TMC2208/2209 and A4988
  want a logic high of 0.7 x VDD (3.5 V from the shield's 5 V rail). It worked on
  the bench but is out of spec. Feed the driver logic supply from 3.3 V.
- The standalone TMC2208 with MS1+MS2 jumpered runs at 1/16 (3200 steps/rev).
  StealthChop loses torque at speed: it stalled near 5 rev/s.
- On CNC shields, GRBL 1.1 swapped the Z limit and spindle PWM between D11 and D12.
  Check which layout the shield uses.

## 12. Methods that worked

- **Assess before touching hardware.** Write the plan from source, and mark every
  unknown **MEASURE**. Take a baseline of the stock system before changing
  anything.
- **Stage the bring-up and isolate each layer:** latency baseline → RT kernel →
  CPU isolation → a minimal SPI echo firmware plus an RT test client
  (`firmware/spi-echo` + `tools/spitest`) → HAL driver on a bare HAL file → full
  LinuxCNC → GUI → soak. Each stage passed with numbers before the next started.
- **Instrument both ends:** frame counters, CRC errors, late transfers,
  ISR max cycles, and watchdog state, reported in-band and on the MCU console.
  Use `sampler` captures for following-error trips and timerlat/osnoise for kernel
  latency.
- **Build in the VM, deploy over adb, verify with checksums** (`dist/SHA256SUMS`
  on the Mac and again on the board). Install steps take `--dry-run`. The HAL
  build is reproducible (rebuilding the old source gave the old hash), which
  proves what's in `dist/`.
- **Test failure paths on purpose:** a missing ELF must fail the flash; halting the
  MCU over SWD for 1 s must drop the link and e-stop; missing or empty VNC
  passwords must be refused.
- **Tests read the config** (for example `unoq.1.position-scale`) instead of
  assuming it. Motion tests use relative moves and end with an exact return-to-start
  step check.
- **Unattended soaks** run in `systemd-run` units. They log a CSV row per block,
  abort and turn the machine off on any failure or SIGTERM, and a dry run comes
  first. The dry run caught three bugs in the soak script.
- **Log everything in `docs/BRINGUP_LOG.md`:** dated entries with measured numbers,
  gotchas, and PASS/FAIL, including your own mistakes and what changed because of
  them.

## 13. Still unknown or untested

- The cause of the stale ESP on warm reboot.
- The single ~1.1-1.3 ms SPI outlier per run (probably the first transfer).
- GENI GPI DMA mode, which could remove most of the ~150-180 µs per-transfer
  overhead.
- The 4 GB model, Arduino's newer images with the 7.0 kernel, Wi-Fi installs, and
  HDMI through a USB-C dock.
- Everything beyond one axis and one motor (see [TODO.md](../TODO.md)).
