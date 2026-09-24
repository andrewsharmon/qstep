# QStep: LinuxCNC on the Arduino UNO Q

LinuxCNC 2.9 runs on the UNO Q's Qualcomm QRB2210 (Debian 13, PREEMPT_RT kernel).
Step pulses come from the on-board STM32U585 over the internal SPI link.

```
QRB2210 (Linux, PREEMPT_RT)                       STM32U585 (Zephyr)
LinuxCNC + AXIS (VNC :1)                          SPI3 DMA slave, 64-byte frames + CRC
  servo thread 1 kHz, CPU3 (isolated)   spidev    DDS stepgen, TIM6 direct ISR @100 kHz
  unoq_spi HAL driver  ---- 20 MHz ---->          CNC Shield v3 pins, 20 ms link watchdog
    (SPI in its own RT worker thread)             LED matrix: "Linux" over "CNC"
```

- **Status and measurements:** [docs/BRINGUP_LOG.md](docs/BRINGUP_LOG.md)
- **Original feasibility study:** [docs/PORTING_ASSESSMENT.md](docs/PORTING_ASSESSMENT.md)
- **Install (no build tools needed):** flash Arduino's official image 20250807-136 with
  `arduino-flasher-cli`, then run `qstep-bootstrap.sh` on the board. See [dist/README.md](dist/README.md).

| Path | Contents |
|---|---|
| `dist/` | Prebuilt kernel .deb, HAL module, STM32 firmware, checksums |
| `firmware/common/qstep_proto.h` | Link protocol shared by the firmware and the HAL driver |
| `firmware/qstep-fw/` | STM32 firmware (stepgen, SPI, IO, watchdog, LED matrix) |
| `firmware/spi-echo/` | SPI link test firmware (pairs with `tools/spitest/`) |
| `hal/` | `unoq_spi` LinuxCNC HAL driver (+ Makefile against linuxcnc-uspace-dev) |
| `configs/unoq-shield/` | LinuxCNC machine config (AXIS + headless) and test scripts |
| `board/` | Board-side installer, systemd units, udev rules, helper scripts |
| `kernel/` | RT kernel config fragment, GENI SPI patch, build script, stock config |
| `tools/` | `make-release.sh`, `deploy.sh`, `mcu-build-flash.sh`, `spitest` |
| `image/` | Single flashable image builder (local or private use only; not published) |

Building from source needs the Lima VM (Debian 13 arm64), the Zephyr SDK and the
`vendor/` clones, which aren't committed. See the bring-up log for the exact steps.
