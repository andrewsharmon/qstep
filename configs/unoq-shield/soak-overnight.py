#!/usr/bin/env python3
"""Long unattended soak of the ArduCNC Y joint (motor free on the bench).

Runs in blocks (default 10 min). Each block:
  * random RELATIVE-range moves (G0, G1 at mixed feeds, short jogs) within
    +/-RANGE rev of the start position, with AXIS running;
  * STRESS_S seconds of hackbench on the non-RT CPUs partway through;
  * returns to the start and checks the step count is exactly what it was.
One CSV row per block with timing maxima (reset each block), link counters,
following error, temperature and process memory. Stops motion and exits on
the first real failure (machine dropped out, watchdog, lost link).

usage: soak-overnight.py [--hours 10] [--block-min 10] [--out DIR]
"""
import argparse
import csv
import datetime
import glob
import os
import random
import signal
import subprocess
import sys
import time

import linuxcnc

RANGE = 2.0
STEPS_PER_REV = 6400
STRESS_S = 60

ap = argparse.ArgumentParser()
ap.add_argument("--hours", type=float, default=10.0)
ap.add_argument("--block-min", type=float, default=10.0)
ap.add_argument("--out", default="/home/arduino/arducnc-config/soak")
args = ap.parse_args()

s, c = linuxcnc.stat(), linuxcnc.command()
err_ch = linuxcnc.error_channel()
os.makedirs(args.out, exist_ok=True)
stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M")
csv_path = os.path.join(args.out, f"soak-{stamp}.csv")
log_path = os.path.join(args.out, f"soak-{stamp}.log")
logf = open(log_path, "a", buffering=1)


def log(msg):
    line = f"{datetime.datetime.now().isoformat(timespec='seconds')} {msg}"
    print(line, flush=True)
    logf.write(line + "\n")


def hal(name):
    return subprocess.run(["halcmd", "getp", name], capture_output=True, text=True).stdout.strip()


def hal_int(name):
    return int(hal(name), 0)


def settled():
    s.poll()
    return s.interp_state == linuxcnc.INTERP_IDLE and s.inpos and abs(s.current_vel) < 1e-9


def drain_errors():
    msgs = []
    while True:
        e = err_ch.poll()
        if not e:
            return msgs
        msgs.append(e[1].strip())


def fail(why):
    log(f"FAIL: {why}")
    for m in drain_errors():
        log(f"  linuxcnc error: {m}")
    try:
        c.abort()
        c.state(linuxcnc.STATE_OFF)
    except Exception:
        pass
    sys.exit(1)


def wait_for(pred, timeout, what):
    t0 = time.time()
    while time.time() - t0 < timeout:
        s.poll()
        if pred():
            return
        if s.task_state != linuxcnc.STATE_ON:
            fail(f"machine dropped out while waiting for {what}")
        time.sleep(0.01)
    fail(f"timeout waiting for {what}")


def wait_move(track, timeout=30.0):
    """Wait for the current move to finish, tracking the worst following error."""
    time.sleep(0.02)
    t0 = time.time()
    while True:
        s.poll()
        track[0] = max(track[0], abs(s.joint[1]["ferror_current"]))
        if s.task_state != linuxcnc.STATE_ON:
            fail("machine dropped out during a move")
        if s.interp_state == linuxcnc.INTERP_IDLE and s.inpos and abs(s.current_vel) < 1e-9:
            return
        if time.time() - t0 > timeout:
            fail(f"move did not finish in {timeout:.0f} s")
        time.sleep(0.005)


def temps():
    vals = []
    for z in glob.glob("/sys/class/thermal/thermal_zone*/temp"):
        try:
            vals.append(int(open(z).read()) / 1000.0)
        except (OSError, ValueError):
            pass
    return max(vals) if vals else float("nan")


def rss_mb(pattern):
    out = subprocess.run(["pgrep", "-f", pattern], capture_output=True, text=True).stdout.split()
    total = 0
    for pid in out:
        try:
            for line in open(f"/proc/{pid}/status"):
                if line.startswith("VmRSS:"):
                    total += int(line.split()[1])
        except OSError:
            pass
    return round(total / 1024.0, 1)


def on_sigterm(signum, frame):
    fail("stopped by SIGTERM")


def move_time(dist, rev_per_s):
    """Generous upper bound for a move: distance at speed, plus accel/settle margin."""
    return abs(dist) / rev_per_s + 10.0


signal.signal(signal.SIGTERM, on_sigterm)

# ---- setup -------------------------------------------------------------------
drain_errors()
c.state(linuxcnc.STATE_ESTOP_RESET)
c.state(linuxcnc.STATE_ON)
wait_for(lambda: s.task_state == linuxcnc.STATE_ON, 5, "machine on")
if not all(s.homed[j] for j in range(3)):
    c.mode(linuxcnc.MODE_MANUAL)
    c.home(-1)
    wait_for(lambda: all(s.homed[j] for j in range(3)), 10, "homing")
c.mode(linuxcnc.MODE_MANUAL)
c.teleop_enable(1)
wait_for(settled, 5, "teleop")
c.mode(linuxcnc.MODE_MDI)
wait_for(lambda: s.task_mode == linuxcnc.MODE_MDI, 5, "MDI")
c.mdi("G90")
wait_for(settled, 5, "G90")
s.poll()
y0 = round(s.actual_position[1], 5)
counts0 = hal_int("unoq.1.counts")
log(f"start: {args.hours} h in {args.block_min} min blocks, Y0={y0:+.5f} rev, counts0={counts0}, csv={csv_path}")

fields = ["block", "time", "moves", "max_ferror_steps", "count_error_steps", "link_late", "link_errors",
          "mcu_bad_frames", "watchdog", "servo_tmax_us", "motion_tmax_us", "unoq_tmax_us",
          "mcu_isr_max_us", "temp_c", "rss_rtapi_mb", "rss_axis_mb", "rss_task_mb", "errors"]
csvf = open(csv_path, "w", newline="", buffering=1)
writer = csv.DictWriter(csvf, fieldnames=fields)
writer.writeheader()

random.seed()
t_stop = time.time() + args.hours * 3600
block = 0
late_prev, err_prev = hal_int("unoq.link-late"), hal_int("unoq.link-errors")

# ---- blocks ------------------------------------------------------------------
while time.time() < t_stop:
    block += 1
    for p in ("servo-thread.tmax", "motion-controller.tmax", "unoq.update.tmax"):
        subprocess.run(["halcmd", "setp", p, "0"])
    t_block_end = min(time.time() + args.block_min * 60, t_stop)
    t_stress = time.time() + args.block_min * 60 * 0.3
    stress = None
    moves, track = 0, [0.0]

    while time.time() < t_block_end:
        if stress is None and time.time() >= t_stress:
            # hackbench runs until killed; it is not pinned, so the scheduler keeps it off isolated CPU3
            stress = subprocess.Popen(["hackbench", "-l", "100000000", "-g", "4"],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            stress_end = time.time() + STRESS_S
        if stress is not None and stress.poll() is None and time.time() >= stress_end:
            stress.terminate()
            stress.wait()

        kind = random.random()
        target = y0 + random.uniform(-RANGE, RANGE)
        s.poll()
        dist = target - s.actual_position[1]
        if kind < 0.4:
            c.mdi(f"G0 Y{target:.5f}")
            wait_move(track, move_time(dist, 5.0))
        elif kind < 0.85:
            feed = random.choice([6, 15, 60, 150, 300])  # rev/min
            c.mdi(f"G1 Y{target:.5f} F{feed}")
            wait_move(track, move_time(dist, feed / 60.0))
        else:
            # Short jog, then back to MDI.
            c.mode(linuxcnc.MODE_MANUAL)
            wait_for(lambda: s.task_mode == linuxcnc.MODE_MANUAL, 5, "manual")
            s.poll()
            y = s.actual_position[1]
            vel = random.uniform(0.05, 3.0) * (1 if y < y0 else -1)
            c.jog(linuxcnc.JOG_CONTINUOUS, False, 1, vel)
            time.sleep(random.uniform(0.1, 0.5))
            c.jog(linuxcnc.JOG_STOP, False, 1)
            wait_move(track)
            c.mode(linuxcnc.MODE_MDI)
            wait_for(lambda: s.task_mode == linuxcnc.MODE_MDI, 5, "MDI")
        moves += 1

    if stress is not None and stress.poll() is None:
        stress.terminate()
        stress.wait()

    # Back to the start: the step count must match exactly.
    s.poll()
    c.mdi(f"G0 Y{y0:.5f}")
    wait_move(track, move_time(y0 - s.actual_position[1], 5.0))
    time.sleep(0.3)
    count_err = hal_int("unoq.1.counts") - counts0
    late, errs = hal_int("unoq.link-late"), hal_int("unoq.link-errors")
    wd = hal("unoq.watchdog")
    errors = drain_errors()
    row = {
        "block": block,
        "time": datetime.datetime.now().isoformat(timespec="seconds"),
        "moves": moves,
        "max_ferror_steps": round(track[0] * STEPS_PER_REV, 1),
        "count_error_steps": count_err,
        "link_late": late - late_prev,
        "link_errors": errs - err_prev,
        "mcu_bad_frames": hal_int("unoq.mcu-bad-frames"),
        "watchdog": wd,
        "servo_tmax_us": round(hal_int("servo-thread.tmax") / 1000, 1),
        "motion_tmax_us": round(hal_int("motion-controller.tmax") / 1000, 1),
        "unoq_tmax_us": round(hal_int("unoq.update.tmax") / 1000, 1),
        "mcu_isr_max_us": round(float(hal("unoq.isr-max-us")), 2),
        "temp_c": temps(),
        "rss_rtapi_mb": rss_mb("^/usr/bin/rtapi_app"),
        "rss_axis_mb": rss_mb("^/usr/bin/python3 /usr/bin/axis"),
        "rss_task_mb": rss_mb("^milltask"),
        "errors": " | ".join(errors),
    }
    writer.writerow(row)
    late_prev, err_prev = late, errs
    log(f"block {block}: {moves} moves, ferror {row['max_ferror_steps']} steps, count err {count_err}, "
        f"late {row['link_late']}, link err {row['link_errors']}, servo {row['servo_tmax_us']} us, "
        f"{row['temp_c']} C")
    if wd == "TRUE":
        fail("MCU watchdog tripped")
    if count_err != 0:
        log(f"WARNING: step count differs by {count_err} at the start position")

c.state(linuxcnc.STATE_OFF)
log(f"done: {block} blocks")
