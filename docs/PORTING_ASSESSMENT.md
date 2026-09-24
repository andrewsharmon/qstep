# QStep: LinuxCNC on the Arduino UNO Q, porting assessment

Written 2026-09-23, from source only. No hardware has been tested yet. Items marked
**MEASURE** need a real board to confirm.

## 1. The two targets

| | LinuxCNC (`vendor/linuxcnc`, master `61e56d7`, 2.10.0~pre1) | Arduino UNO Q |
|---|---|---|
| CPU | Needs a Linux host with a hard‑RT servo thread (1 kHz) and, for software step generation, a base thread (20–50 kHz) | **MPU:** Qualcomm QRB2210, 4× Cortex‑A53 @ 2.0 GHz, Adreno 702, 2 GB or 4 GB RAM, 16 GB or 32 GB eMMC. **MCU:** STM32U585, Cortex‑M33 @ 160 MHz, 2 MB flash, 786 KB SRAM |
| OS | Debian, `uspace` build with PREEMPT_RT (RTAI/Xenomai also supported) | Debian 13 "trixie" arm64, kernel `arduino/linux-qcom` branch `qcom-v7.0.0-unoq` (7.0.0). The MCU runs Zephyr 4.4 with the Arduino core, and sketches load as LLEXT modules |
| I/O | Parallel port, Mesa FPGA cards (PCI/Ethernet/SPI), GPIO (`hal_gpio` via libgpiod, `hal_pi_gpio`), Remora/SPI co‑processors | Every UNO header pin (D0–D13, A0–A5, D20/21) belongs to the **STM32**, not to Linux |
| MPU↔MCU link | — | (a) LPUART1 ↔ Linux UART at 115200 baud, used by `arduino-router` (Go, msgpack‑RPC). (b) **SPI**: Linux `spi5` → `/dev/spidev0.0` (`compatible = "arduino,unoq-mcu"`) ↔ MCU `SPI3` in slave mode (SCK PG9, MISO PG10, MOSI PB5, NSS PG12) with a "SPI RDY" line on PG13 |

## 2. Recommended architecture

This follows the Raspberry Pi + Remora / Mesa‑SPI pattern that LinuxCNC already supports:

```
 QRB2210 (Debian, PREEMPT_RT)                          STM32U585 (Zephyr, custom firmware)
 ┌───────────────────────────────┐   SPI (spidev,     ┌──────────────────────────────────┐
 │ LinuxCNC uspace               │   full duplex,     │ SPI3 slave + DMA, fixed frame     │
 │  motion / trajectory planner  │   once per servo   │ stepgen: timer ISR 50–100 kHz     │
 │  servo thread 1 kHz ──────────┼──── period ───────▶│ digital I/O, PWM spindle, ADC     │
 │  HAL comp "unoq-spi" (new)    │◀───────────────────┤ feedback: pos counts, inputs     │
 │  GUI: AXIS/Gmoccapy (OpenGL)  │                    │ watchdog: stop pulses if SPI dies │
 └───────────────────────────────┘                    └──────────────────────────────────┘
```

LinuxCNC runs **unmodified** on the MPU, apart from one new HAL driver. There is **no base
thread on Linux**: step pulses are generated on the MCU.

## 3. Blockers and risks (most severe first)

### B1. Arduino's Bridge/RPC can't carry servo‑thread traffic. Severity: HIGH (design blocker)
- `arduino-router` defaults to `--serial-baudrate 115200` (`vendor/arduino/arduino-router/main.go:79`). That is about 11 bytes/ms. Every message also passes through a Go daemon, a Unix socket (`/var/run/arduino-router.sock`), msgpack encoding and the MCU's RPC dispatcher, so latency is unbounded.
- **Fix:** don't use the Bridge. Talk to the MCU directly over `/dev/spidev0.0` from the RT thread, and stop `arduino-router` while CNC is running.
- **Risk:** the only public SPI3 example ([mjs513/Arduino-Uno-Q-SPI3-Bridge-App](https://github.com/mjs513/Arduino-Uno-Q-SPI3-Bridge-App)) reports ~1 MHz as the practical maximum. At 1 MHz a 64‑byte full‑duplex frame takes about 512 µs, which is half the servo period. We need ≥ 8 MHz, with DMA on the STM32 side. **MEASURE:** maximum reliable SCLK on this board's traces, and `ioctl(SPI_IOC_MESSAGE)` round‑trip jitter on the Qualcomm GENI SPI driver (GPI DMA, threaded IRQs under RT).
- Fallback: LPUART1 has RTS/CTS wired (PG5/PG6), so a raw multi‑Mbaud UART is a second option.

### B2. The stock kernel isn't real‑time. Severity: HIGH (must build our own kernel)
- The kernel that ships with the image (`precompiled/linux-image-7.0.0-g122c2c22d838`) has `CONFIG_PREEMPT=y`, `CONFIG_HZ=250` and the schedutil governor. It is **not** PREEMPT_RT. LinuxCNC's `detect_preempt_rt()` (`src/rtapi/uspace_rtapi_main.cc:67`) looks for "PREEMPT_RT" in `uname`.
- Good news: kernel 7.0 has PREEMPT_RT in mainline, and arm64 `select ARCH_SUPPORTS_RT`. Arduino's `build-linux-deb.py` merges config fragments. `kernel/rt.config` in this repo is the fragment to add.
- **Risk:** the Qualcomm vendor drivers (GENI SPI/UART, GPI DMA, RPM interconnect, msm DRM/Adreno, WCN3988 Wi‑Fi/BT, remoteprocs) have not been tested under RT. Firmware‑assisted power management (RPM/SMD) can cause latency spikes that RT can't remove. **MEASURE:** `cyclictest`/`latency-histogram` under load (GPU+network). The target is < 100 µs worst case for a 1 kHz servo thread. Because stepgen runs on the MCU, we never need the 20–50 µs a Linux base thread would require.
- The kernel has to be built on Linux, either on the board (4×A53, 2 GB RAM, slow) or on an arm64 Linux host or CI runner. This Mac has no Docker or Linux cross toolchain.
- Arduino apt updates (`linux-image-*`) can replace our kernel. Pin it with `apt-mark hold`.

### B3. No LinuxCNC firmware exists for the STM32U585. Severity: HIGH (main new code)
- Remora (`vendor/reference/Remora`) is the closest prior art: STM32 step generation driven over SPI by LinuxCNC. However, it is built on **Mbed OS (end‑of‑life)**, has no STM32U5 target, and its LinuxCNC side (`Remora-spi.c`) accesses BCM2835 registers directly instead of using spidev.
- Plan: write new Zephyr firmware for board `arduino_uno_q` (`vendor/arduino/zephyr/boards/arduino/uno_q`). Reuse Remora's frame layout and ideas: DDS stepgen in a timer ISR, a watchdog that stops pulses if SPI frames stop, and PRU‑style joint/variable tables.
- Build this as a **native Zephyr app, not an Arduino sketch**. The Arduino core loads sketches as LLEXT modules on top of a loader, which adds indirection and takes the SPI3/LPUART peripherals for the Bridge.
- Flashing works from the board itself. Arduino's `remoteocd` runs OpenOCD with `openocd_gpiod.cfg` (SWD bit‑banged from Linux GPIO), so no external debug probe is required. To restore stock behaviour later, reflash Arduino's loader.

### B4. Limited I/O and 3.3 V logic. Severity: MEDIUM (hardware limit on machine size)
- About 20 usable header pins, some shared with UART/I2C/SPI. A 3‑axis router needs about 14 pins (3× step/dir, enable, 3 limits/home, probe, E‑stop, spindle PWM + dir). 4 axes fit. 5+ axes, or quadrature encoders on every axis, don't fit without expanders on I2C/SPI2 or the JMISC connector.
- Signals are 3.3 V. Many hobby stepper drivers with opto inputs expect 5 V at about 10 mA. Use level shifting or check the drivers. **MEASURE:** check against an Arduino CNC Shield v3 (GRBL pinout), which is a convenient first test fixture.

### B5. Stock Arduino services own the hardware. Severity: MEDIUM (configuration)
- `arduino-router`, App Lab and the Arduino MCU loader all expect to own LPUART1, SPI3 and the MCU flash. A CNC image must disable them: `systemctl disable arduino-router` and friends. Also add a udev rule so the LinuxCNC user can open `/dev/spidev0.0` and `/dev/gpiochip*`.

### B6. 2 GB RAM and the A53 cores. Severity: LOW–MEDIUM
- AXIS (Tk + OpenGL) should run: Freedreno supports the Adreno 702, and glmark2 scores about 290. QtDragon and other Qt/WebKit GUIs will be tight on the 2 GB model, and the 4 GB model is preferred. Building LinuxCNC from source on the board is slow. Use Debian trixie's arm64 `linuxcnc-uspace` 2.9.4 at first, or build 2.10 debs in arm64 CI.
- Set the CPU governor to performance. **MEASURE:** thermal throttling under a long job.

### Non‑blockers (checked)
- LinuxCNC already builds on arm64: Debian ships `linuxcnc-uspace` for arm64, and there is Raspberry Pi support such as `spix_rpi5`. The x86‑only code (`rtapi_io.h` inb/outb, `hal_parport`, `hm2_pci`, `hal_speaker`) is just unusable here and needs no porting.
- `hal_gpio` needs `libgpiod < 3.0`. Trixie ships 2.x, which is fine.
- No prior port exists. "1‑Q‑CNC" on GitHub is an Android GRBL HMI, not LinuxCNC on the UNO Q.

## 4. First tasks once the board arrives
1. Record the baseline: `uname -a`, `/proc/cpuinfo`, `cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_available_frequencies`, `ls /dev/spidev* /dev/ttyHS* /dev/gpiochip*`, and RAM size.
2. Run `cyclictest -m -Sp90 -i1000 -h400` on the **stock** kernel with stress, to get a baseline.
3. Build and install the PREEMPT_RT kernel (`kernel/rt.config`) and repeat step 2.
4. Test SPI throughput and latency: flash a minimal Zephyr SPI3‑slave echo firmware, then sweep SCLK from 1 to 20 MHz from an RT thread and record errors and the round‑trip time histogram.
5. Install `linuxcnc-uspace`, run `latency-test`, and run a sim config with AXIS to check GUI performance.
6. Only after steps 2–5 pass, write the `unoq-spi` HAL driver and the stepgen firmware.

## 5. Downloaded material (`vendor/`, shallow clones)
| Path | Rev | Purpose |
|---|---|---|
| `linuxcnc` | `61e56d7` master | LinuxCNC source |
| `arduino/linux-qcom` | `122c2c22d` qcom-v7.0.0-unoq | UNO Q kernel (DTS `qrb2210-arduino-imola-base.dts`) |
| `arduino/arduino-deb-images` | `e7713f1` | Image build recipes, kernel fragments, `build-linux-deb.py`, precompiled stock kernel .deb |
| `arduino/zephyr` | `17437417` zephyr-arduino-v4.4.1+ | Zephyr fork with the `uno_q` board |
| `arduino/ArduinoCore-zephyr` | `39f8354` | MCU pin map, Bridge implementation, flash scripts |
| `arduino/arduino-router`, `arduino-router-bridge-py` | `cb7b2ce`, `2219048` | Bridge daemon (to be disabled) |
| `arduino/remoteocd` | `7109bed` | OpenOCD‑from‑Linux MCU flashing |
| `arduino/arduino-flasher-cli`, `arduino-linux-config` | `72daf37`, `29d99aa` | Reflashing the board and board config |
| `reference/Remora` | `09d7e24` | Prior art for LinuxCNC↔STM32 SPI stepgen |

Not downloaded yet, because they are better fetched on the board or a Linux host: the Zephyr SDK toolchain (the Arduino image already ships `arm-zephyr-eabi` 0.16.8), the aarch64 cross GCC for the kernel, and the full UNO Q Debian image (use `arduino-flasher-cli`).
