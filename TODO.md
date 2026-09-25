# QStep TODO

## Tested so far

QStep has only been tested in this configuration:

- **one axis, one stepper motor** (joint 1 / Y), running free on the bench, not on a
  machine axis;
- a generic Arduino **"CNC SHIELD" Ver 3.00** (GRBL pinout) on the UNO Q header;
- **DRV8825** driver module at 1/32 microstepping, and a **TMC2208** (standalone,
  1/16) on Y;
- Arduino UNO Q **2 GB / 16 GB**, Arduino image **20250807-136**, networking through
  a USB proxy (no Wi-Fi), AXIS viewed over VNC.

Everything else in the code (X, Z and A joints, limit/probe/abort inputs, spindle and
coolant outputs) is wired up but **untested**.

## Machine safety and I/O

- [ ] **Hardware e-stop input.** A normally-closed switch on a shield input, wired into
  LinuxCNC's e-stop chain (`iocontrol.0.emc-enable-in`), and the MCU should also
  drop the driver enable directly. Today only a lost SPI link triggers e-stop.
- [ ] **Limit and home switches.** Map `unoq.input.limit-x/y/z` to joint
  limits/home, set up homing sequences and search/latch velocities (homing is
  currently "home where you are").
- [ ] **Probe input.** Test `unoq.input.probe` with `G38.x`.
- [ ] **Spindle PWM.** The frame already carries `spindle_pwm`. It needs a timer
  output on the MCU, and the pin assignment must be checked against the shield
  (GRBL 1.1 moved the Z limit and spindle PWM between D11 and D12).
- [ ] Test the spindle enable/direction and coolant outputs.
- [ ] Test the abort / feed-hold / cycle-start inputs and wire them to `halui`.

## Wider hardware coverage

- [ ] **Multiple axes at once:** X, Y, Z moving simultaneously, and the A axis through
  the shield's clone jumpers (it has no dedicated pins in `pins.h` yet).
- [ ] **A real machine axis under load:** lead screw or belt, missed-step check,
  realistic accelerations.
- [ ] **Millimetre units:** set `SCALE = steps_per_rev x microsteps / mm_per_rev` per
  axis once the mechanics are known, plus real limits and speeds.
- [ ] **Other drivers:** TMC2208 standalone runs on Y at 1/16 (`SCALE = 3200`; the
  shipped config assumes 1/32, 6400), but stalls near 5 rev/s: tune Vref and the Y
  speed limit. A4988 and TMC2208/2209
  need a logic high of 0.7 x VDD, so feed the shield's "5V" rail from 3.3 V. Also
  test TMC2209 and external step/dir drivers.
- [ ] Scope the STEP/DIR pins to confirm pulse width, direction setup and jitter.
- [ ] UNO Q 4 GB model; HDMI through a USB-C dock (monitor, keyboard, mouse)
  instead of VNC.
- [ ] Wi-Fi installs. The two-step install has only been run with the USB proxy.
- [ ] More I/O through the JMISC connector (more axes, hardware timer step
  outputs, encoder inputs).

## Performance

- [ ] Cut the ~150–180 µs fixed SPI transfer overhead: GENI FIFO mode takes 5 IRQs
  per frame; try GPI DMA mode.
- [ ] Look at the average cyclictest latency on the isolated CPU (~23 µs under
  memory load).

## Software and release

- [ ] Follow Arduino's newer images and the 7.0 kernel (today the RT kernel
  only matches image 20250807-136).
- [ ] Offer the kernel patch upstream (spi-geni-qcom: don't reprogram the clock in
  prepare_message).
- [ ] Consider CI (arm64 runners) to build and publish releases.
- [ ] Ask for the VNC password during the bootstrap, so VNC works right after the
  power cycle (VNC never runs without a password; today that's a separate
  `qstep-vnc-passwd` step).
