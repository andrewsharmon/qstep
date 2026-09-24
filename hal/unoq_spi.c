/*
 * unoq_spi: LinuxCNC HAL driver for the QStep firmware on the Arduino UNO Q.
 *
 * One full-duplex spidev transfer per servo period to the on-board STM32U585
 * (see firmware/common/qstep_proto.h). The MCU runs velocity-mode DDS step
 * generation; this driver closes the position loop.
 *
 * The transfer itself runs in a private SCHED_FIFO worker thread on the same
 * CPU as the servo thread, so the servo thread never blocks on SPI (under GUI
 * load a transfer can take ~700 us). unoq.update hands the worker the frame for
 * this period and picks up the reply to the previous one.
 *
 * Position estimate: every status frame carries the step counts sampled right
 * after the MCU applied command `seq_echo`. The position when the command sent
 * now takes effect is therefore
 *
 *       counts + dt * sum(v[i] * periods[i]) for i = seq_echo .. last sent
 *
 * using the velocities we actually commanded (after DDS quantisation), kept in
 * a ring indexed by sequence number. Then
 *
 *       v = ff + pgain * (position_cmd - estimate) / dt,   ff = d(position_cmd)/dt
 *
 * clamped by maxvel and maxaccel (give these ~25% headroom over the joint's
 * trajectory limits, as StepConf does, or decelerations will overshoot).
 *
 * Pins (N = 0..3 = X, Y, Z, A):
 *   unoq.N.position-cmd   float in    commanded position (machine units)
 *   unoq.N.position-fb    float out   estimated actual position
 *   unoq.N.enable         bit   in    joint may move
 *   unoq.N.counts         s32   out   raw step count (relative to load time)
 *   unoq.N.velocity-cmd   float out   commanded velocity (units/s)
 *   unoq.N.freq           float out   commanded step rate (steps/s)
 *   unoq.enable           bit   in    enable the stepper drivers (EN pin)
 *   unoq.connected        bit   out   valid status frames are arriving
 *   unoq.watchdog         bit   out   MCU link watchdog tripped (toggle enable to clear)
 *   unoq.enabled          bit   out   MCU reports drivers enabled
 *   unoq.link-errors      u32   out   failed transfers / bad status frames
 *   unoq.link-late        u32   out   periods where the previous transfer had not finished
 *   unoq.mcu-bad-frames   u32   out   bad frames seen by the MCU
 *   unoq.isr-max-us       float out   worst MCU stepgen ISR time
 *   unoq.input.NAME       bit   out   limit-x limit-y limit-z abort hold resume probe
 *   unoq.output.NAME      bit   in    spindle-enable spindle-dir coolant
 * Parameters:
 *   unoq.N.position-scale float rw    steps per machine unit (default 1)
 *   unoq.N.maxvel         float rw    units/s, 0 = only the hardware limit
 *   unoq.N.maxaccel       float rw    units/s^2, 0 = unlimited
 *   unoq.N.pgain          float rw    fraction of position error corrected per period (default 0.5)
 *   unoq.N.deadband       float rw    steps; when the command is not moving and the error is
 *                                     below this, stop instead of chasing a sub-step
 *                                     target (default 0.5, i.e. hold the nearest step)
 * Module parameters:
 *   spi_dev="/dev/spidev0.0" spi_speed=20000000 worker_prio=97
 */

#include "rtapi.h"
#include "rtapi_app.h"
#include "rtapi_math.h"
#include "rtapi_string.h"
#include "hal.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "qstep_proto.h"

MODULE_AUTHOR("QStep");
MODULE_DESCRIPTION("QStep UNO Q STM32 stepgen/IO over SPI");
MODULE_LICENSE("GPL");

static char *spi_dev = "/dev/spidev0.0";
RTAPI_MP_STRING(spi_dev, "spidev device node");
static int spi_speed = 20000000;
RTAPI_MP_INT(spi_speed, "SPI clock in Hz");
static int worker_prio = 97;
RTAPI_MP_INT(worker_prio, "SCHED_FIFO priority of the SPI worker thread");

#define NUM_INPUTS  7
#define NUM_OUTPUTS 3
static const char *input_names[NUM_INPUTS] = {"limit-x", "limit-y", "limit-z", "abort",
					       "hold", "resume", "probe"};
static const char *output_names[NUM_OUTPUTS] = {"spindle-enable", "spindle-dir", "coolant"};

#define STEP_RATE_MAX   (QSTEP_BASE_FREQ_HZ / 2.0)
#define DDS_PER_HZ      (4294967296.0 / QSTEP_BASE_FREQ_HZ)
#define MAX_FB_AGE      16 /* status older than this many commands = not connected */

typedef struct {
	hal_float_t *position_cmd;
	hal_float_t *position_fb;
	hal_bit_t *enable;
	hal_s32_t *counts;
	hal_float_t *velocity_cmd;
	hal_float_t *freq;
	hal_float_t scale;
	hal_float_t maxvel;
	hal_float_t maxaccel;
	hal_float_t pgain;
	hal_float_t deadband;
} joint_t;

typedef struct {
	joint_t joint[QSTEP_JOINTS];
	hal_bit_t *enable;
	hal_bit_t *connected;
	hal_bit_t *watchdog;
	hal_bit_t *enabled;
	hal_u32_t *link_errors;
	hal_u32_t *link_late;
	hal_u32_t *mcu_bad_frames;
	hal_float_t *isr_max_us;
	hal_bit_t *input[NUM_INPUTS];
	hal_bit_t *output[NUM_OUTPUTS];
} unoq_hal_t;

typedef union {
	struct qstep_cmd cmd;
	uint8_t raw[QSTEP_FRAME_LEN];
} tx_frame_t;

typedef union {
	struct qstep_stat stat;
	uint8_t raw[QSTEP_FRAME_LEN];
} rx_frame_t;

/* SPI worker: the servo thread fills `tx`, bumps `posted` and posts `go`; the
 * worker transfers and sets `done` = posted with the reply in `rx`. */
static struct {
	pthread_t thread;
	int started;
	sem_t go;
	volatile int stop;
	tx_frame_t tx;
	rx_frame_t rx;
	int xfer_ok;
	uint32_t posted;
	uint32_t done;
} w;

/* Servo-thread state. */
static struct {
	int fd;
	int lat_fd;
	uint8_t seq;
	int have_fb;
	int32_t count_offset[QSTEP_JOINTS];
	int32_t counts[QSTEP_JOINTS];      /* latest reported, relative to count_offset */
	uint8_t counts_seq;               /* command the counts were sampled after */
	double v_ring[QSTEP_JOINTS][256];  /* steps/s commanded with each seq */
	uint8_t dur_ring[256];            /* periods each seq stayed in effect */
	double old_cmd[QSTEP_JOINTS];
	int was_enabled[QSTEP_JOINTS];
} st;

static int comp_id;
static unoq_hal_t *h;

static void *spi_worker(void *arg)
{
	while (1) {
		sem_wait(&w.go);
		if (w.stop) {
			break;
		}
		uint32_t n = __atomic_load_n(&w.posted, __ATOMIC_ACQUIRE);
		struct spi_ioc_transfer xfer = {
			.tx_buf = (uintptr_t)w.tx.raw,
			.rx_buf = (uintptr_t)w.rx.raw,
			.len = QSTEP_FRAME_LEN,
			.speed_hz = spi_speed,
			.bits_per_word = 8,
		};

		w.xfer_ok = ioctl(st.fd, SPI_IOC_MESSAGE(1), &xfer) >= 0;
		__atomic_store_n(&w.done, n, __ATOMIC_RELEASE);
	}
	return NULL;
}

/* Take the reply to the last posted frame, if the worker has finished it. */
static int collect_reply(void)
{
	rx_frame_t rx;

	if (__atomic_load_n(&w.done, __ATOMIC_ACQUIRE) != w.posted) {
		(*h->link_late)++;
		return -EBUSY;
	}
	if (w.posted == 0) {
		return -EAGAIN; /* nothing sent yet */
	}
	memcpy(&rx, &w.rx, sizeof(rx));
	if (!w.xfer_ok || rx.stat.magic != QSTEP_STAT_MAGIC ||
	    rx.stat.crc != qstep_crc16(rx.raw, QSTEP_FRAME_LEN - 2)) {
		(*h->link_errors)++;
		return -EIO;
	}

	int32_t steps[QSTEP_JOINTS];

	memcpy(steps, rx.stat.steps, sizeof(steps));
	if (!st.have_fb) {
		/* Positions start at 0 whatever the MCU counted before we loaded. */
		memcpy(st.count_offset, steps, sizeof(steps));
		st.have_fb = 1;
	}
	for (int j = 0; j < QSTEP_JOINTS; j++) {
		st.counts[j] = steps[j] - st.count_offset[j];
		*h->joint[j].counts = st.counts[j];
	}
	st.counts_seq = rx.stat.seq_echo;
	*h->enabled = !!(rx.stat.status & QSTEP_ST_ENABLED);
	*h->watchdog = !!(rx.stat.status & QSTEP_ST_WATCHDOG);
	*h->mcu_bad_frames = rx.stat.frames_bad;
	*h->isr_max_us = rx.stat.isr_max_cycles / 160.0; /* 160 MHz core */
	for (int i = 0; i < NUM_INPUTS; i++) {
		*h->input[i] = !!(rx.stat.inputs & (1u << i));
	}
	return 0;
}

static void update(void *arg, long period)
{
	const double dt = period * 1e-9;
	int got = collect_reply();

	if (got == -EBUSY) {
		/* Previous transfer still running: the MCU keeps executing the last
		 * command for another period. Account for that and skip this period. */
		if (st.dur_ring[st.seq] < 255) {
			st.dur_ring[st.seq]++;
		}
		return;
	}

	/* How many commands have been applied since the counts were sampled. */
	uint8_t age = (uint8_t)(st.seq - st.counts_seq);
	int fresh = st.have_fb && age < MAX_FB_AGE;

	*h->connected = fresh && got == 0;
	int drivers_on = *h->enable && fresh;

	tx_frame_t tx;

	memset(&tx, 0, sizeof(tx));
	tx.cmd.magic = QSTEP_CMD_MAGIC;
	tx.cmd.flags = drivers_on ? QSTEP_CMD_ENABLE : 0;
	uint8_t new_seq = st.seq + 1;

	for (int j = 0; j < QSTEP_JOINTS; j++) {
		joint_t *jt = &h->joint[j];
		double scale = jt->scale != 0.0 ? jt->scale : 1.0;
		double cmd = *jt->position_cmd;
		double est_steps = st.counts[j];
		double v = 0.0; /* units/s */

		/* Commands counts_seq .. seq (last sent) have run since the sample. */
		for (uint8_t s = st.counts_seq;; s++) {
			est_steps += st.v_ring[j][s] * st.dur_ring[s] * dt;
			if (s == st.seq) {
				break;
			}
		}
		*jt->position_fb = est_steps / scale;

		if (drivers_on && *jt->enable) {
			double ff = st.was_enabled[j] ? (cmd - st.old_cmd[j]) / dt : 0.0;
			double err = cmd - *jt->position_fb;
			double v_prev = st.v_ring[j][st.seq] / scale;

			/* The motor can only sit on whole steps. With the command at rest
			 * and the error under half a step, stop there: chasing the
			 * fraction makes the motor dither between neighbouring steps. */
			if (ff == 0.0 && fabs(err * scale) <= jt->deadband) {
				err = 0.0;
			}
			v = ff + jt->pgain * err / dt;
			if (jt->maxvel > 0.0 && fabs(v) > jt->maxvel) {
				v = copysign(jt->maxvel, v);
			}
			if (jt->maxaccel > 0.0) {
				double dv = jt->maxaccel * dt;

				if (v > v_prev + dv) {
					v = v_prev + dv;
				} else if (v < v_prev - dv) {
					v = v_prev - dv;
				}
			}
			st.was_enabled[j] = 1;
		} else {
			st.was_enabled[j] = 0;
		}
		st.old_cmd[j] = cmd;

		double steps_per_s = v * scale;

		if (fabs(steps_per_s) > STEP_RATE_MAX) {
			steps_per_s = copysign(STEP_RATE_MAX, steps_per_s);
		}
		int32_t incr = (int32_t)rint(steps_per_s * DDS_PER_HZ);

		if (incr == INT32_MIN) {
			incr = INT32_MIN + 1;
		}
		tx.cmd.dds_incr[j] = incr;

		/* What the MCU will really do with this seq (quantised). */
		st.v_ring[j][new_seq] = incr / DDS_PER_HZ;
		*jt->velocity_cmd = st.v_ring[j][new_seq] / scale;
		*jt->freq = st.v_ring[j][new_seq];
	}
	st.seq = new_seq;
	st.dur_ring[new_seq] = 1;
	tx.cmd.seq = new_seq;

	uint32_t outputs = 0;

	for (int i = 0; i < NUM_OUTPUTS; i++) {
		if (*h->output[i]) {
			outputs |= 1u << i;
		}
	}
	tx.cmd.outputs = outputs;
	tx.cmd.crc = qstep_crc16(tx.raw, QSTEP_FRAME_LEN - 2);

	/* Hand the frame to the worker (it is idle: done == posted). */
	memcpy(&w.tx, &tx, sizeof(tx));
	__atomic_store_n(&w.posted, w.posted + 1, __ATOMIC_RELEASE);
	sem_post(&w.go);
}

static int export_pins(void)
{
	int r = 0;

	for (int j = 0; j < QSTEP_JOINTS; j++) {
		joint_t *jt = &h->joint[j];

		r |= hal_pin_float_newf(HAL_IN, &jt->position_cmd, comp_id, "unoq.%d.position-cmd", j);
		r |= hal_pin_float_newf(HAL_OUT, &jt->position_fb, comp_id, "unoq.%d.position-fb", j);
		r |= hal_pin_bit_newf(HAL_IN, &jt->enable, comp_id, "unoq.%d.enable", j);
		r |= hal_pin_s32_newf(HAL_OUT, &jt->counts, comp_id, "unoq.%d.counts", j);
		r |= hal_pin_float_newf(HAL_OUT, &jt->velocity_cmd, comp_id, "unoq.%d.velocity-cmd", j);
		r |= hal_pin_float_newf(HAL_OUT, &jt->freq, comp_id, "unoq.%d.freq", j);
		r |= hal_param_float_newf(HAL_RW, &jt->scale, comp_id, "unoq.%d.position-scale", j);
		r |= hal_param_float_newf(HAL_RW, &jt->maxvel, comp_id, "unoq.%d.maxvel", j);
		r |= hal_param_float_newf(HAL_RW, &jt->maxaccel, comp_id, "unoq.%d.maxaccel", j);
		r |= hal_param_float_newf(HAL_RW, &jt->pgain, comp_id, "unoq.%d.pgain", j);
		r |= hal_param_float_newf(HAL_RW, &jt->deadband, comp_id, "unoq.%d.deadband", j);
		jt->scale = 1.0;
		jt->pgain = 0.5;
		jt->deadband = 0.5;
	}
	r |= hal_pin_bit_newf(HAL_IN, &h->enable, comp_id, "unoq.enable");
	r |= hal_pin_bit_newf(HAL_OUT, &h->connected, comp_id, "unoq.connected");
	r |= hal_pin_bit_newf(HAL_OUT, &h->watchdog, comp_id, "unoq.watchdog");
	r |= hal_pin_bit_newf(HAL_OUT, &h->enabled, comp_id, "unoq.enabled");
	r |= hal_pin_u32_newf(HAL_OUT, &h->link_errors, comp_id, "unoq.link-errors");
	r |= hal_pin_u32_newf(HAL_OUT, &h->link_late, comp_id, "unoq.link-late");
	r |= hal_pin_u32_newf(HAL_OUT, &h->mcu_bad_frames, comp_id, "unoq.mcu-bad-frames");
	r |= hal_pin_float_newf(HAL_OUT, &h->isr_max_us, comp_id, "unoq.isr-max-us");
	for (int i = 0; i < NUM_INPUTS; i++) {
		r |= hal_pin_bit_newf(HAL_OUT, &h->input[i], comp_id, "unoq.input.%s", input_names[i]);
	}
	for (int i = 0; i < NUM_OUTPUTS; i++) {
		r |= hal_pin_bit_newf(HAL_IN, &h->output[i], comp_id, "unoq.output.%s", output_names[i]);
	}
	return r;
}

static int start_worker(void)
{
	pthread_attr_t attr;
	struct sched_param sp = {.sched_priority = worker_prio};
	cpu_set_t cpus;

	if (sem_init(&w.go, 0, 0) != 0) {
		return -errno;
	}
	pthread_attr_init(&attr);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
	pthread_attr_setschedparam(&attr, &sp);

	/* Same CPU set as our own (rtapi_app pins itself to the isolated RT CPU). */
	if (sched_getaffinity(0, sizeof(cpus), &cpus) == 0) {
		pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
	}

	int err = pthread_create(&w.thread, &attr, spi_worker, NULL);

	pthread_attr_destroy(&attr);
	if (err) {
		return -err;
	}
	w.started = 1;
	return 0;
}

int rtapi_app_main(void)
{
	uint8_t mode = SPI_MODE_0, bits = 8;
	uint32_t speed = spi_speed;

	comp_id = hal_init("unoq_spi");
	if (comp_id < 0) {
		return comp_id;
	}

	st.fd = open(spi_dev, O_RDWR);
	if (st.fd < 0 || ioctl(st.fd, SPI_IOC_WR_MODE, &mode) < 0 ||
	    ioctl(st.fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
	    ioctl(st.fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
		rtapi_print_msg(RTAPI_MSG_ERR, "unoq_spi: cannot set up %s: %s\n", spi_dev,
				strerror(errno));
		hal_exit(comp_id);
		return -ENODEV;
	}

	/* Waking a power-collapsed Qualcomm core costs milliseconds; stay out of
	 * deep idle for as long as we are loaded. */
	int32_t zero = 0;

	st.lat_fd = open("/dev/cpu_dma_latency", O_WRONLY);
	if (st.lat_fd < 0 || write(st.lat_fd, &zero, sizeof(zero)) != sizeof(zero)) {
		rtapi_print_msg(RTAPI_MSG_WARN, "unoq_spi: cannot hold cpu_dma_latency\n");
	}

	h = hal_malloc(sizeof(*h));
	if (!h || export_pins() != 0 ||
	    hal_export_funct("unoq.update", update, NULL, 1, 0, comp_id) != 0) {
		rtapi_print_msg(RTAPI_MSG_ERR, "unoq_spi: HAL export failed\n");
		hal_exit(comp_id);
		return -EINVAL;
	}

	int err = start_worker();

	if (err) {
		rtapi_print_msg(RTAPI_MSG_ERR, "unoq_spi: cannot start SPI worker: %s\n",
				strerror(-err));
		hal_exit(comp_id);
		return err;
	}

	rtapi_print_msg(RTAPI_MSG_INFO, "unoq_spi: %s at %d Hz\n", spi_dev, spi_speed);
	hal_ready(comp_id);
	return 0;
}

void rtapi_app_exit(void)
{
	if (w.started) {
		w.stop = 1;
		sem_post(&w.go);
		pthread_join(w.thread, NULL);
	}
	/* Leave the drivers disabled: the MCU watchdog also does this within 20 ms. */
	if (st.fd >= 0) {
		tx_frame_t tx;
		rx_frame_t rx;
		struct spi_ioc_transfer xfer = {
			.tx_buf = (uintptr_t)tx.raw,
			.rx_buf = (uintptr_t)rx.raw,
			.len = QSTEP_FRAME_LEN,
			.speed_hz = spi_speed,
			.bits_per_word = 8,
		};

		memset(&tx, 0, sizeof(tx));
		tx.cmd.magic = QSTEP_CMD_MAGIC;
		tx.cmd.seq = ++st.seq;
		tx.cmd.crc = qstep_crc16(tx.raw, QSTEP_FRAME_LEN - 2);
		ioctl(st.fd, SPI_IOC_MESSAGE(1), &xfer);
		close(st.fd);
	}
	if (st.lat_fd >= 0) {
		close(st.lat_fd);
	}
	hal_exit(comp_id);
}
