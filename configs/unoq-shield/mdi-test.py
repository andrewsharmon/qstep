#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 andrewsharmon
"""Headless end-to-end test: reset e-stop, machine on, home, MDI moves on Y.

Run while LinuxCNC is up with unoq-shield.ini. Prints the joint-1 following
error seen by LinuxCNC during the moves.
"""
import sys
import time

import linuxcnc

MOVES = sys.argv[1:] or ["G0 Y1", "G0 Y0", "G1 Y-1 F60", "G0 Y0"]

s = linuxcnc.stat()
c = linuxcnc.command()


def wait_for(pred, timeout, what):
    t0 = time.time()
    while time.time() - t0 < timeout:
        s.poll()
        if pred():
            return
        time.sleep(0.05)
    sys.exit(f"timeout waiting for {what}")


c.state(linuxcnc.STATE_ESTOP_RESET)
wait_for(lambda: s.task_state == linuxcnc.STATE_ESTOP_RESET, 5, "estop reset")
c.state(linuxcnc.STATE_ON)
wait_for(lambda: s.task_state == linuxcnc.STATE_ON, 5, "machine on")

c.mode(linuxcnc.MODE_MANUAL)
c.home(-1)
wait_for(lambda: all(s.homed[j] for j in range(3)), 10, "homing")
print("homed, machine on")

c.mode(linuxcnc.MODE_MDI)
wait_for(lambda: s.task_mode == linuxcnc.MODE_MDI, 5, "MDI mode")

for m in MOVES:
    c.mdi(m)
    max_ferror = 0.0
    t0 = time.time()
    time.sleep(0.05)
    while True:
        s.poll()
        max_ferror = max(max_ferror, abs(s.joint[1]["ferror_current"]))
        if s.interp_state == linuxcnc.INTERP_IDLE and s.inpos:
            break
        if time.time() - t0 > 20:
            sys.exit(f"'{m}' did not finish")
        time.sleep(0.002)
    print(f"{m:18s} -> Y={s.actual_position[1]:9.3f}  max ferror={max_ferror:.3f}  "
          f"({time.time() - t0:.2f} s)")

c.state(linuxcnc.STATE_OFF)
print("machine off")
