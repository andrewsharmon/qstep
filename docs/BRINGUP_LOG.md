# UNO Q bring-up log

> **Project renamed from ArduCNC to QStep on 2026-09-24.** The entries below
> keep the names in use at the time (for example `arducnc-fw`,
> `6.16.0-rt-arducnc2`, `/home/arduino/arducnc-config`), since they record what
> was actually built and measured. Current names: `qstep-fw`, kernel
> `6.16.0-rt-qstep1`, `qstep-*` services, `/home/arduino/qstep-config`,
> `/etc/qstep`, `/opt/qstep`. The HAL driver keeps its board-based name
> `unoq_spi` (`unoq.*` pins).

## 2026-09-23/24: first session with hardware

### Board as received
- UNO Q, 2 GB RAM / 16 GB eMMC (root partition 9.3 GB). Debian 13, kernel `6.16.0-geffa8626771a` (`SMP PREEMPT`, not RT), HZ=250, schedutil governor.
- Reached over USB with `adb` (`tools/platform-tools/adb`). The adb shell runs as root. The USB gadget exposes only adb and ACM (`ttyGS0`), with no network function.
- Wi-Fi isn't configured. The `arduino` account has a forced password change pending (not done; it needs the owner).
- `/dev/spidev0.0` (link to the MCU), `/dev/ttyHS1` (Bridge UART), `/dev/gpiochip0-2`, and `/dev/dri/card0` (Adreno) are all present.

### Networking for apt without Wi-Fi
- The Lima VM `arducnc` (Debian 13 arm64) runs `tinyproxy` on port 3128. Lima forwards that port to the Mac's localhost.
- On the board: `adb reverse tcp:3128 tcp:3128`, plus `/etc/apt/apt.conf.d/90arducnc-proxy` pointing apt at `http://127.0.0.1:3128`.
- **The board has no RTC backup and no NTP.** After every power cycle, set the clock before running apt:
  `adb shell "date -u -s '$(date -u '+%Y-%m-%d %H:%M:%S')'"`
- Re-run `adb reverse` after each reconnect.
- Don't run a blind `apt upgrade`. The sources include Debian `experimental` and the Arduino repo. `linux-image-6.16.0-geffa8626771a` and `linux-image-arm64` are on `apt-mark hold`.

### Installed
- `rt-tests` (cyclictest, hackbench).
- `linuxcnc-uspace 1:2.9.4-2+deb13u1` from Debian trixie, `--no-install-recommends` (about 1.2 GB). Free space on root is now about 1.9 GB.

### PREEMPT_RT kernel
- Built in the Lima VM with `kernel/build-rt-kernel.sh` (about 7 min). Inputs: the exact commit the board shipped with (`effa862`), the board's own config (`kernel/config-6.16.0-geffa8626771a.stock`), and `kernel/rt.config` (PREEMPT_RT, HZ=1000, NO_HZ_FULL, RCU_NOCB, performance governor).
- Output: `kernel/out/linux-image-6.16.0-rt-arducnc_1_arm64.deb`. Installed with `/etc/kernel/tries` set to 2, so systemd-boot counts boot attempts. It booted, was blessed, and is now the default. The stock kernel is still in the menu, and `loader.conf` has `timeout 3` so the menu can be used with HDMI and a keyboard.
- The DTB installed by the package is byte-identical to the stock one.

### Gotchas found
1. **A warm reboot (`adb reboot`) boots an old view of the ESP.** After warm reboots, systemd-boot reported version 257.7 and saw none of the new entries or `loader.conf` edits, even though the ESP on disk had 257.13 and the new files. After a **full power cycle** (unplug and replug USB-C) it read the current ESP straight away. Until this is understood, **do a physical power cycle after any change to `/boot/efi`**. Suspect firmware/U-Boot caching on the warm-reset path (to investigate).
2. The EFI variables are read-only from Linux (`efivarfs` mounted ro, no U-Boot runtime SetVariable), so `bootctl set-oneshot` and `bootctl set-default` don't work. Use boot counting and `loader.conf` instead.
3. With 2 GB of RAM it's easy to trigger the OOM killer. The killer takes out `adbd`, and the board then looks dead over USB. (Caused by `grep -a` over the raw eMMC. Don't do that.)
4. **LinuxCNC 2.9.4 treats a kernel as RT only if `/sys/kernel/realtime` == 1.** That file came from the out-of-tree RT patchset and doesn't exist in mainline PREEMPT_RT. Without a fix it silently runs "POSIX non-realtime". Fixed with `LINUXCNC_FORCE_REALTIME=1` in `/etc/environment` and `/etc/profile.d/linuxcnc-rt.sh`. (LinuxCNC master checks `uname` only.)
5. `halrun`/`linuxcnc` refuse to run as root. From the adb root shell, use `RTAPI_UID=$(id -u arduino) RTAPI_FIFO_PATH=/tmp/.rtapi_fifo halrun ...`.

### Latency results (cyclictest `-m -p90 -i1000`, 60 s per run)
| Kernel | Condition | Max latency (µs), worst thread |
|---|---|---|
| stock 6.16 PREEMPT | idle | 376 |
| stock 6.16 PREEMPT | hackbench + eMMC dd | **2804** |
| 6.16 PREEMPT_RT | idle | 215 |
| 6.16 PREEMPT_RT | hackbench + eMMC dd | **236** |
| 6.16 PREEMPT_RT | same load kept off CPU3 at runtime (IRQ affinity + taskset), measured on CPU3, 120 s | **150** |

LinuxCNC HAL servo thread at 1 kHz (`timedelta`, 30 s, idle, RT forced): max jitter **22.8 µs**.

### Boot-time CPU isolation (after a power cycle)
`isolcpus=3 nohz_full=3 rcu_nocbs=3 irqaffinity=0-2` on the RT entry. cyclictest on CPU3 with hackbench + dd on CPUs 0-2 for 120 s: **max 132 µs** (avg 23 µs; the shared L2 and memory bus probably account for the average).

## 2026-09-24: MCU link (SPI) bring-up

### Backup
- Full 2 MB STM32 flash dump taken over on-board SWD **before** any flashing: `stock-mcu-flash-2MB.bin`, sha256 `c550fc1a…62d0`. It holds Arduino firmware, so it's kept outside this repo in the local-only `../arducnc-private/mcu-backup/`. RDP 0, TrustZone off, dual bank.
- Stock layout: Arduino Zephyr loader at 0x08000000 (no MCUboot), sketch LLEXT at 0x080F0000, boot animation at 0x080D0000.
- Restore: `openocd -s /opt/openocd -f openocd_gpiod.cfg -c "init; reset halt; flash write_image erase stock-mcu-flash-2MB.bin 0x08000000 bin; reset run; shutdown"`.

### Setup
- `arduino-router` and `arduino-app-cli` are **disabled** (`systemctl disable --now`), because they own the MCU link. Re-enable with `systemctl enable --now arduino-router arduino-app-cli`.
- Zephyr workspace in the VM: `~/zp`, a west workspace on `arduino/zephyr` at `zephyr-arduino-v4.4.1+`, trimmed to cmsis, cmsis_6 and hal_stm32. Uses Zephyr SDK 1.0.1 (arm only) and board `arduino_uno_q`.
- `tools/mcu-build-flash.sh firmware/<app>` builds in the VM, pushes over adb, then flashes and verifies with the on-board OpenOCD. The "reset-init failed / Translation from khz" messages from OpenOCD are harmless (linuxgpiod has no speed setting).
- MCU console: with `zephyr,console = &lpuart1` the output appears on Linux at **`/dev/ttyHS1`** (115200). Very handy.

### Test: `firmware/spi-echo` + `tools/spitest`
64-byte full-duplex frames. The MCU echoes the previous frame. The Linux side is SCHED_FIFO 80 on CPU3 with mlockall and cpu_dma_latency=0, one transfer per 1 ms.

| Step | Result |
|---|---|
| First run, 1 MHz | 98% echo OK, but **7.6 ms per transfer** on CPU3 (2.8 ms on CPU1) |
| Hold `/dev/cpu_dma_latency`=0 | CPU3 drops to 2.8 ms. Deep idle exit was part of it |
| Sweep 250 kHz / 1 MHz / 8 MHz | 4.4 / 2.8 / 2.4 ms: **a fixed ~2.3 ms overhead plus wire time** |
| Root cause | spi-geni-qcom sets the clock to `spi->max_speed_hz` (50 MHz, spidev never changes it) in prepare_message, then to `xfer->speed_hz` per transfer. **Every change re-votes interconnect bandwidth through the RPM (~ms).** At exactly 50 MHz there's no change: **165 µs**. |
| MCU `printk` moved to a low-priority thread (polled UART blocked ~5 ms per line) | **2999/2999 frames OK** at 1 MHz and at 50 MHz |
| 60 000 frames, 50 MHz, hackbench + dd load | 100% OK. avg 230 µs, p99 502 µs, **max 2.5 ms** |
| + SPI IRQ (115) and its thread moved to CPU3 at FIFO 85 | 100% OK. avg 234 µs, p99.9 390 µs, **max 430 µs** |

- The SPI controller runs in **GENI FIFO mode** (5 IRQs per 64-byte transfer, no GPI DMA).
- The STM32U585 SPI3 slave with GPDMA (req 10/11) handled a 50 MHz SCLK with zero errors in more than 120k frames.
- Made persistent: `board/arducnc-rt-tune` plus `.service` (SPI IRQ to CPU3, FIFO 85, runtime PM on).
- Kernel fix: `kernel/patches/0001-spi-geni-qcom-don-t-reprogram-clock-in-prepare_message.patch` drops the redundant per-message clock set, so any SPI speed avoids the RPM vote. It's built into `6.16.0-rt-arducnc2`, which also adds ftrace/osnoise/timerlat. Installed with 2 tries; needs a power cycle.

### LED matrix
- Our firmware drives the 13x8 matrix itself: "Linux" scrolls on the top 4 rows and "CNC" stays fixed on the bottom 3. The display is charlieplexed on PF0-PF10 and scanned by TIM17 at IRQ priority 7 (lowest), 100 µs per LED.

### Patched kernel `6.16.0-rt-arducnc2` (booted, blessed)
SPI speed sweep with 3000 frames each, idle, CPU3. All runs had 100% echo OK.

| SCLK | avg | p99 | p99.9 | max |
|---|---|---|---|---|
| 1 MHz | 663 µs (512 µs of it on the wire) | 666 | 695 | 1707 |
| 10 MHz | 203 µs | 248 | 286 | 1095 |
| 20 MHz | 185 µs | 200 | 261 | 1247 |
| 50 MHz | 187 µs | 231 | 264 | 1317 |

The clock-switch penalty is gone at every speed. What's left is about 150–180 µs of fixed overhead per transfer (FIFO mode, 5 IRQs). There's a single ~1.1–1.3 ms outlier per run, probably the first transfer; to check with timerlat. **20 MHz is the chosen operating point**: same latency as 50 MHz with more signal margin.

## 2026-09-24: Step generation, first motion

### Pieces
- `firmware/common/arducnc_proto.h`: the 64-byte cmd/status frames with CRC-16, shared by the firmware and the HAL driver.
- `firmware/arducnc-fw`: SPI3 DMA slave loop, DDS stepgen in a **direct TIM6 ISR at 100 kHz** (priority 0, no kernel calls), 10 µs step pulses, 20 µs direction setup, a 20 ms link watchdog (it latches until the host sends enable=0), the CNC Shield v3 pin map in `src/pins.h`, and the LED matrix.
- `hal/unoq_spi.c` (+ `hal/Makefile`, builds against `linuxcnc-uspace-dev` 2.9.4 in the VM). It does one transfer per servo period. The position loop compensates for the 2-command-old feedback with the velocities it actually commanded.
- `configs/unoq-shield`: a trivkins XYZ LinuxCNC config (headless display for now; units are raw steps, SCALE=1). `mdi-test.py` drives it through the Python API.
- Board: `board/60-arducnc.rules` (spidev and cpu_dma_latency to group gpiod), `board/lcnc-ctl` (start/stop/run as the arduino user via `setpriv`).

### Gotchas
6. **The STM32 BOOT0 pin is driven from Linux** (gpiochip1 line 37). `arduino-router.service` sets it low in `ExecStartPre`. With the router disabled, the next MCU reset boots the **ST ROM bootloader** (PC = 0x0BF9xxxx). FLASH_OPTR = 0x1FEFF8AA (nSWBOOT0=1, so the pin decides). `arducnc-rt-tune` now drives 37=0 at boot. Don't read it with `gpioget`: that turns it into an input.
7. `RTAPI_UID` from a root shell keeps root's group list, so the RT process can't open gpiod-group devices. Launch as arduino with `setpriv --reuid=arduino --regid=arduino --init-groups` (no PAM, so the pending password change doesn't block it).
8. `adb push` over an existing file drops its exec bit. Re-run `chmod`.
9. LinuxCNC 2.9 task hung on the first command (exec_state 7, waiting for motion and IO) until the INI had `[EMCIO] EMCIO = io` / `CYCLE_TIME` and `[TASK] CYCLE_TIME = 0.001`. No error was printed.

### Results
- **HAL test** (`hal/y-move-test.hal`, limit3 → unoq.1): 0 → 400 → 0 steps landed on exactly 400 and 0. 3820 frames, 0 bad.
- **Full LinuxCNC** (`mdi-test.py`: estop reset, machine on, home, MDI):

| Move | End position (steps) | max following error | time |
|---|---|---|---|
| G0 Y400 | 400.001 | 2.21 | 0.89 s |
| G0 Y0 | -0.002 | 1.09 | 0.88 s |
| G1 Y-400 F30000 | -400.002 | 1.47 | 1.03 s |
| G0 Y0 | 0.000 | 1.01 | 0.88 s |

- 127k frames, 0 bad in either direction, and the watchdog never tripped.
- Servo thread (motion + unoq.update): **288 µs avg, 394 µs max** of 1000 µs.
- MCU stepgen ISR: 42 cycles idle and ~208 cycles (1.3 µs) while stepping, worst 330 cycles (2.1 µs) of each 10 µs tick, so about 13% CPU at 100 kHz. The LED matrix scan runs alongside at the lowest priority.

## 2026-09-24: Units, AXIS over VNC, async SPI

- **Units:** DRV8825 at 1/32 microstepping and 200-step motors give 6400 steps/rev. The config now works in **motor revolutions** (SCALE 6400, 5 rev/s, 20 rev/s²) until the mechanics are known. One commanded revolution was verified.
- **Display:** `board/arducnc-xvfb.service` runs Xvfb :1 at 1280x800, and `board/arducnc-vnc.service` runs x11vnc on 127.0.0.1:5900 (localhost only). **macOS Screen Sharing won't connect to a no-auth VNC server**, so the service uses `-rfbauth /etc/arducnc/vnc.pass` when that file exists. The owner set the password with `x11vnc -storepasswd`, and it isn't stored in the repo. Connect with `open vnc://127.0.0.1:5900` (IPv4: the adb forward doesn't listen on ::1). Both are on CPUs 0-2. From the Mac: `adb forward tcp:5900 tcp:5900`, then `open vnc://localhost:5900`. `~/.axisrc` fills the screen because there's no window manager. AXIS renders its GL preview in software (llvmpipe).
- `lcnc-ctl stop` uses `axis-remote --quit`, because AXIS ignores SIGTERM.
- **Following-error trip under AXIS** (`G1 Y2 F120`). The HAL maxaccel was set equal to the joint MAX_ACCELERATION, so the driver could never brake as hard as the planner asked. It overshot on deceleration and tripped MIN_FERROR (seen with a `sampler` capture). Fixed with `STEPGEN_MAXVEL/MAXACCEL` = 1.25x, the StepConf convention.
- **"Unexpected realtime delay" under AXIS.** Worst cases were unoq.update 696 µs (the SPI ioctl slows down under GUI memory contention), motion-controller 259 µs, and servo-thread 760 µs. **Fix: the SPI transfer now runs in a SCHED_FIFO 97 worker thread.** The servo thread posts the frame and reads the previous reply. The estimator now uses the MCU's `seq_echo` plus a per-seq velocity/duration ring, so a late transfer is accounted for exactly (`unoq.link-late` counts them).

| Under AXIS, 3 program runs | before | after |
|---|---|---|
| servo-thread tmax | 637–760 µs | **234 µs** |
| unoq.update tmax | 511–696 µs | 116 µs |
| motion-controller tmax | 195–259 µs | 183 µs |
| realtime-delay warning | yes | no |

## 2026-09-24: Auto-restart service, jog-hold fix

- **`board/arducnc-linuxcnc.service`** runs LinuxCNC + AXIS on :1 as the arduino user. It's enabled at boot, with `Restart=always` (3 s), so closing AXIS brings it back. `lcnc-ctl start|stop|restart` wrap the service. `lcnc-ctl start-headless` stops it and runs `unoq-shield-headless.ini` for scripted tests. AXIS opens `nc_files/y-demo.ngc` (INI `OPEN_FILE`).
- **Motor hunting after a jog.** A jog usually stops between two steps. The P-loop chased the fraction, so the motor dithered ±1 step forever. Fixed with a new param `unoq.N.deadband` (default 0.5 steps): when the command isn't moving and the error is under half a step, the driver holds. Verified with `hold-test.py` (tiny G91 moves only): 0 count changes in 2 s at every sub-step target.
- **Lesson:** my first hold test used absolute targets and ran while the owner was jogging in AXIS. Y travelled about 20 rev, and the two sessions fought over machine on/off. Command and feedback always agreed, so it wasn't a fault. **Scripted motion tests now need a check-in first, and relative moves only.**

### Bench validation (`soak-test.py`, motor free on the bench, AXIS running)
- **Jog, then stop** at 0.37, 1.13, 2.71 and 0.05 rev/s: **0 count changes** in the 2 s after each stop, so there's no dithering.
- **60 s soak**: 45 random G0/G1 moves (F15–300) within ±2 rev. Max following error was **0.00112 rev (7.2 steps)**, and it returned to the start position with a **0-step difference**.
- Link: 0 late, 0 errors, 0 MCU bad frames, no watchdog trips. MCU ISR max 2.24 µs.
- tmax: servo-thread **259 µs**, motion-controller 208 µs, unoq.update 106 µs (period 1000 µs).

## 2026-09-24: Overnight soak

- `configs/unoq-shield/soak-overnight.py`, launched by `board/soak-start [hours] [block-min]` as the transient unit `arducnc-soak` (it survives adb disconnects). `arducnc-soak-mcu` captures the MCU console and is `BindsTo=` the soak. Output goes to `~arduino/arducnc-config/soak/` (a CSV row and a log line per block).
- Each 10-min block: random G0/G1 (F6–300 rev/min)/jog moves within ±2 rev, 60 s of hackbench on CPUs 0-2, then a return to start with an **exact step-count check**. It logs per-block tmax (servo, motion, unoq), link late/errors, MCU bad frames, watchdog, ISR max, temperature, and RSS of rtapi_app/axis/milltask. On any failure it aborts and turns the machine off. SIGTERM does the same.
- The dry run caught three script bugs: a fixed 30 s move timeout (an F6 move over 4 rev takes about 40 s), the MCU-logger stop hook running as the unprivileged user, and stopping mid-move leaving the machine running. All fixed and re-verified.
- **Started 2026-09-24 06:17 UTC, 10 h.** Dry-run blocks: count error 0, late 0, link errors 0, servo tmax 236–454 µs (the higher values under hackbench), 53–54 °C.

### Overnight soak result (2026-09-24 06:17–16:17 UTC, 60 × 10-min blocks), PASS
Logs are in `docs/soak/2026-09-24-overnight/` (per-block CSV and log, plus the MCU console, gzipped).

| Metric (per block) | min | median | max |
|---|---|---|---|
| following error (steps) | 8.8 | 11.8 | **34.5** (0.0054 rev; limit ≥ 64) |
| servo-thread tmax (µs, period 1000) | 267 | 360 | **483** |
| motion-controller tmax (µs) | 202 | 266 | 380 |
| unoq.update tmax (µs) | 82 | 128 | 177 |
| MCU stepgen ISR max (µs) | 2.24 | 2.24 | 2.24 |
| temperature (°C) | 49.2 | 53.5 | 54.8 |

- **15,754 moves** (G0, G1 at F6–300, jogs) with 60 s of hackbench per block. **Returned to the start step exactly in all 60 blocks** (0 count error).
- **Link: 0 late, 0 errors, 0 MCU bad frames** over about 36 M frames. **0 watchdog trips.** No MCU resets (no boot banner in the console log).
- Memory flat: rtapi_app 59.2 MB, milltask 18.7 MB, AXIS 199→201 MB (settles after the first blocks). No leak.

## 2026-09-24: Real two-step install test (fresh board), PASS

1. **Step 1:** `arduino-flasher-cli` 0.5.4 flashed Arduino's official image 20250807-136 (35 partitions). The first attempt failed partway with `qdl: bulk write failed` after the jumper had been placed on the wrong pins (my description was wrong; `dist/README.md` now has a diagram that follows Arduino's photo). The retry, from a locally cached copy of the image, succeeded. The board then booted a clean stock system: kernel 6.16.0-geffa, no ArduCNC, arduino-router active, 3.2 GB free after the flasher grew the rootfs.
2. **Step 2:** `sh arducnc-bootstrap.sh --bundle arducnc-0.1.0-rc1.tar.gz` over `adb shell`, with the Mac proxy standing in for Wi-Fi. It passed the base check, verified the bundle, and installed the packages, the RT kernel, the system services, the HAL driver and the config. It backed up the STM32 and flashed the firmware (`Verified OK`). **Bug found:** adb exports `TMPDIR=/data/local/tmp`, which doesn't exist here, so apt-listchanges errored (non-fatal). The bootstrap now falls back to /tmp (`ecbf667`).
3. **After a power cycle:** it booted `6.16.0-rt-arducnc2 #1 PREEMPT_RT` (the neutral build) and blessed the entry. isolcpus=3. All ArduCNC services active, arduino-router inactive, no failed units. rtapi RT thread at FIFO 98 on CPU3, SPI worker at FIFO 97. AXIS came up by itself. Link connected with 0 errors.
4. **Motion:** `hold-test.py` (0 count changes at sub-step targets) and `soak-test.py`: 45 moves, max following error 7.3 steps, back at start with 0 steps difference, link 0 late and 0 errors, servo tmax 349 µs.

Note: the STM32 isn't touched by the Linux flasher, so on this board the "stock" backup the installer took is a copy of the earlier ArduCNC firmware. The real stock image is in `../arducnc-private`.

### Next
- Get the per-transfer overhead down further: 5 IRQs per frame in FIFO mode. Options are GPI DMA mode, or a single transfer with CS handled in hardware.
- Set real SCALE (steps/mm) and limits once the microstepping jumpers and mechanics are known.
- Attach a display and switch the config to AXIS. Measure GUI load against servo-thread jitter.
- Hardware e-stop input, and limit/home switches through `unoq.input.*`. Spindle PWM.
- Scope the STEP/DIR pins to verify pulse width, dir setup and jitter.
- Run a LinuxCNC sim config with AXIS on the HDMI display to check GUI load.
