#!/usr/bin/env python3
"""Check the motor holds still at targets between motor steps (no dithering).

Uses only tiny RELATIVE moves (G91) from wherever Y is, so it is safe on a
machine that has been jogged or touched off. Reads unoq.1.counts via halcmd.
"""
import subprocess
import sys
import time

import linuxcnc

# Relative Y moves in revolutions (6400 steps/rev): +0.45, +0.19 (-> 0.64), +0.51, -1.15 steps.
STEPS = [0.00007, 0.00003, 0.00008, -0.00018]
s, c = linuxcnc.stat(), linuxcnc.command()


def wait_for(pred, timeout, what):
    t0 = time.time()
    while time.time() - t0 < timeout:
        s.poll()
        if pred():
            return
        time.sleep(0.02)
    c.state(linuxcnc.STATE_OFF)
    sys.exit(f"timeout waiting for {what}")


def counts():
    out = subprocess.run(["halcmd", "getp", "unoq.1.counts"], capture_output=True, text=True)
    return int(out.stdout.strip())


def settled():
    """Motion finished: interpreter idle, in position, no velocity, stable for 0.2 s."""
    s.poll()
    return s.interp_state == linuxcnc.INTERP_IDLE and s.inpos and abs(s.current_vel) < 1e-9


c.state(linuxcnc.STATE_ESTOP_RESET)
c.state(linuxcnc.STATE_ON)
wait_for(lambda: s.task_state == linuxcnc.STATE_ON, 5, "machine on")
if not all(s.homed[j] for j in range(3)):
    c.mode(linuxcnc.MODE_MANUAL)
    c.home(-1)
    wait_for(lambda: all(s.homed[j] for j in range(3)), 10, "homing")
c.mode(linuxcnc.MODE_MDI)
wait_for(lambda: s.task_mode == linuxcnc.MODE_MDI, 5, "MDI mode")
c.mdi("G91")
wait_for(settled, 5, "G91")

start = counts()
print(f"start: Y machine={s.actual_position[1]:.5f} rev, counts={start}")
for d in STEPS:
    c.mdi(f"G0 Y{d}")
    c.wait_complete(5)
    time.sleep(0.2)
    wait_for(settled, 5, "move")
    time.sleep(0.2)
    samples = []
    for _ in range(40):
        samples.append(counts())
        time.sleep(0.05)
    changes = sum(1 for a, b in zip(samples, samples[1:]) if a != b)
    s.poll()
    print(f"G91 G0 Y{d:+.5f}: cmd={s.joint[1]['output'] if 'output' in s.joint[1] else 0:+.5f}  "
          f"counts held at {sorted(set(samples))}  changes in 2 s: {changes}")

c.mdi("G90")
wait_for(settled, 5, "G90")
c.state(linuxcnc.STATE_OFF)
print(f"end: counts={counts()} (moved {counts() - start} steps net)")
