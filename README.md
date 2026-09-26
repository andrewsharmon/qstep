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

> **Tested scope:** QStep has only been tested with **one axis and one
> stepper motor** (on the bench), a generic Arduino **"CNC SHIELD" Ver 3.00**, and
> a **DRV8825** driver module. Other axes, drivers, inputs and outputs are wired
> up but untested. See [TODO.md](TODO.md).

> [!WARNING]
> **Safety:** QStep drives stepper motors, and it has **no hardware e-stop and no limit
> or home switches yet**. The only e-stop is in software: LinuxCNC's own, and
> the MCU turning the drivers off when the SPI link from Linux stops. Only run it
> on the bench or on a machine that can't hurt anyone, and keep a way to cut the
> motor power within reach. QStep comes with no warranty (see the GPL).

- **Status and measurements:** [docs/BRINGUP_LOG.md](docs/BRINGUP_LOG.md)
- **Original feasibility study:** [docs/PORTING_ASSESSMENT.md](docs/PORTING_ASSESSMENT.md)
- **Install (no build tools needed):** flash Arduino's official image 20250807-136 with
  `arduino-flasher-cli`, then run
  [`qstep-bootstrap.sh`](https://github.com/andrewsharmon/qstep/releases/latest/download/qstep-bootstrap.sh)
  from the latest release on the board. See [dist/README.md](dist/README.md).

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
| `tools/` | `make-release.sh`, `deploy.sh`, `mcu-build-flash.sh`, `restore-stock.sh`, `spitest` |
| `image/` | Single flashable image builder (local or private use only; not published) |

Building from source needs the Lima VM (Debian 13 arm64), the Zephyr SDK and the
`vendor/` clones, which aren't committed. See the bring-up log for the exact steps.

The prebuilt files in `dist/` are stored with Git LFS (about 40 MB, mostly the
kernel). To clone only the source, skip them:

```bash
GIT_LFS_SKIP_SMUDGE=1 git clone https://github.com/andrewsharmon/qstep.git
```

## Standing on the shoulders of giants

QStep is mostly glue. Nearly all the hard work was done by other people and
projects, and QStep only connects their work on one small board:

- **[LinuxCNC](https://linuxcnc.org)** and its community: the motion controller,
  trajectory planner, HAL, AXIS GUI and decades of CNC know-how that QStep simply
  runs. The Debian `linuxcnc-uspace` packages are used unmodified.
- **[Arduino](https://www.arduino.cc)**: the UNO Q hardware, its Debian image,
  the [UNO Q Linux kernel](https://github.com/arduino/linux-qcom), the
  [Zephyr fork and board support](https://github.com/arduino/zephyr),
  [ArduinoCore-zephyr](https://github.com/arduino/ArduinoCore-zephyr) (its LED
  matrix pin map is reused here), the on-board OpenOCD SWD setup and the
  [Flasher CLI](https://github.com/arduino/arduino-flasher-cli).
- **The Linux kernel and PREEMPT_RT developers**, and the Qualcomm/Linaro mainline
  work on the QRB2210 (`linux-msm`), including the GENI SPI driver that QStep
  patches in one line.
- **[Zephyr RTOS](https://zephyrproject.org)**, **STMicroelectronics** (STM32Cube
  HAL/LL) and **Arm CMSIS**, which make up the STM32 firmware's foundation.
- **[Remora](https://github.com/scottalford75/Remora)** by Scott Alford, and the
  Mesa/LinuxCNC stepgen design: the model for SPI-linked step generation with a
  host-side position loop. No Remora code is used; the ideas are.
- **[Debian](https://www.debian.org)**, **[OpenOCD](https://openocd.org)**,
  **[Lima](https://lima-vm.io)** and the many other free software tools used
  to build and test QStep.
- **AI assistance:** much of the code, testing and documentation was developed
  with **Claude (Anthropic)** as a coding assistant, working under the author's
  direction; commits carry a `Co-Authored-By: Claude` trailer.

All trademarks belong to their owners, and QStep isn't affiliated with or
endorsed by any of these projects or companies. See [NOTICE](NOTICE).

## License

Copyright (C) 2026 andrewsharmon.

QStep is free software, licensed under the **GNU General Public License,
version 2 or later** ([COPYING](COPYING)). The kernel patches are GPL-2.0-only,
and one Arduino-derived firmware file is Apache-2.0. The prebuilt firmware image
is distributed under GPL-3.0-or-later, because it links with Apache-2.0 Zephyr
code. Every source file carries an SPDX identifier. [NOTICE](NOTICE) lists third-party
components and the source for every binary in `dist/`. License texts are in
[LICENSES/](LICENSES/).
