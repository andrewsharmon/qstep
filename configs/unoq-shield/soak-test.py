#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 andrewsharmon
"""Bench validation for joint 1 (Y), motor free on the bench.

1. Jog test: continuous jogs at several speeds, stop, check the motor then
   holds still (no dithering after a jog stops between steps).
2. Soak test: random RELATIVE moves within +/-RANGE rev of the start for
   DURATION seconds, mixing G0 and G1 at various feeds; track the worst
   following error; return to the start and check it lands on the same step.
Reads HAL values with halcmd (read-only).
"""
import random
import subprocess
import sys
import time

import linuxcnc

DURATION = 60.0
RANGE = 2.0          # rev either side of the start position
s, c = linuxcnc.stat(), linuxcnc.command()


def wait_for(pred, timeout, what):
    t0 = time.time()
    while time.time() - t0 < timeout:
        s.poll()
        if pred():
            return
        time.sleep(0.01)
    c.state(linuxcnc.STATE_OFF)
    sys.exit(f"timeout waiting for {what}")


def hal(name):
    return subprocess.run(["halcmd", "getp", name], capture_output=True, text=True).stdout.strip()


STEPS_PER_REV = float(hal("unoq.1.position-scale"))  # Y's SCALE from the INI


def settled():
    s.poll()
    return s.interp_state == linuxcnc.INTERP_IDLE and s.inpos and abs(s.current_vel) < 1e-9


def still_for(seconds):
    """Return the number of count changes seen over `seconds`."""
    samples = []
    t_end = time.time() + seconds
    while time.time() < t_end:
        samples.append(int(hal("unoq.1.counts")))
        time.sleep(0.05)
    return sum(1 for a, b in zip(samples, samples[1:]) if a != b)


c.state(linuxcnc.STATE_ESTOP_RESET)
c.state(linuxcnc.STATE_ON)
wait_for(lambda: s.task_state == linuxcnc.STATE_ON, 5, "machine on")
if not all(s.homed[j] for j in range(3)):
    c.mode(linuxcnc.MODE_MANUAL)
    c.home(-1)
    wait_for(lambda: all(s.homed[j] for j in range(3)), 10, "homing")

for p in ("servo-thread.tmax", "unoq.update.tmax", "motion-controller.tmax"):
    subprocess.run(["halcmd", "setp", p, "0"])
late0, err0 = int(hal("unoq.link-late"), 0), int(hal("unoq.link-errors"), 0)

# 1. Jog test (teleop jog on axis Y).
c.mode(linuxcnc.MODE_MANUAL)
c.teleop_enable(1)
wait_for(settled, 5, "manual")
print("jog test (jog, stop, then watch 2 s for dithering):")
for vel, dur in [(0.37, 0.6), (-1.13, 0.4), (2.71, 0.3), (-0.05, 1.0)]:
    c.jog(linuxcnc.JOG_CONTINUOUS, False, 1, vel)
    time.sleep(dur)
    c.jog(linuxcnc.JOG_STOP, False, 1)
    wait_for(settled, 5, "jog stop")
    time.sleep(0.2)
    s.poll()
    frac = (s.joint[1]["output"] * STEPS_PER_REV) % 1 if "output" in s.joint[1] else float("nan")
    print(f"  jog {vel:+.2f} rev/s for {dur}s -> stopped at Y={s.actual_position[1]:+.5f}, "
          f"count changes after stop: {still_for(2.0)}")

# 2. Soak test.
c.mode(linuxcnc.MODE_MDI)
wait_for(lambda: s.task_mode == linuxcnc.MODE_MDI, 5, "MDI")
s.poll()
y0 = s.actual_position[1]
counts0 = int(hal("unoq.1.counts"))
print(f"soak: {DURATION:.0f} s of moves within +/-{RANGE} rev of Y={y0:+.4f} (counts {counts0})")
random.seed(1)
moves, max_ferr = 0, 0.0
t_end = time.time() + DURATION
while time.time() < t_end:
    target = y0 + random.uniform(-RANGE, RANGE)
    if random.random() < 0.5:
        cmd = f"G90 G0 Y{target:.5f}"
    else:
        cmd = f"G90 G1 Y{target:.5f} F{random.choice([15, 60, 150, 300])}"
    c.mdi(cmd)
    time.sleep(0.02)
    while True:
        s.poll()
        max_ferr = max(max_ferr, abs(s.joint[1]["ferror_current"]))
        if s.interp_state == linuxcnc.INTERP_IDLE and s.inpos and abs(s.current_vel) < 1e-9:
            break
        if s.task_state != linuxcnc.STATE_ON:
            sys.exit("machine turned off during soak (following error?)")
        time.sleep(0.005)
    moves += 1

c.mdi(f"G90 G0 Y{y0:.5f}")
time.sleep(0.05)
wait_for(settled, 20, "return to start")
time.sleep(0.3)
counts1 = int(hal("unoq.1.counts"))
print(f"  {moves} moves, max following error {max_ferr:.5f} rev ({max_ferr * STEPS_PER_REV:.1f} steps)")
print(f"  back at start: counts {counts1} (start {counts0}, difference {counts1 - counts0} steps)")
print(f"  link: late {int(hal('unoq.link-late'), 0) - late0}, errors {int(hal('unoq.link-errors'), 0) - err0}, "
      f"mcu bad frames {int(hal('unoq.mcu-bad-frames'), 0)}, watchdog {hal('unoq.watchdog')}, "
      f"MCU ISR max {float(hal('unoq.isr-max-us')):.2f} us")
for p in ("servo-thread.tmax", "motion-controller.tmax", "unoq.update.tmax"):
    print(f"  {p:24s} {int(hal(p)) / 1000:7.1f} us")
c.state(linuxcnc.STATE_OFF)
