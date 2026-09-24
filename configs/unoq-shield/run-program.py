#!/usr/bin/env python3
"""Machine on, home, and run a G-code file in AUTO mode (while AXIS shows it)."""
import sys
import time

import linuxcnc

path = sys.argv[1]
s, c = linuxcnc.stat(), linuxcnc.command()


def wait_for(pred, timeout, what):
    t0 = time.time()
    while time.time() - t0 < timeout:
        s.poll()
        if pred():
            return
        time.sleep(0.05)
    sys.exit(f"timeout waiting for {what}")


c.state(linuxcnc.STATE_ESTOP_RESET)
c.state(linuxcnc.STATE_ON)
wait_for(lambda: s.task_state == linuxcnc.STATE_ON, 5, "machine on")
c.mode(linuxcnc.MODE_MANUAL)
c.home(-1)
wait_for(lambda: all(s.homed[j] for j in range(3)), 10, "homing")
c.mode(linuxcnc.MODE_AUTO)
c.program_open(path)
t0 = time.time()
c.auto(linuxcnc.AUTO_RUN, 0)
wait_for(lambda: s.interp_state != linuxcnc.INTERP_IDLE, 5, "program start")
wait_for(lambda: s.interp_state == linuxcnc.INTERP_IDLE and s.inpos, 120, "program end")
print(f"ran {path} in {time.time() - t0:.1f} s, Y ends at {s.actual_position[1]:.4f}")
