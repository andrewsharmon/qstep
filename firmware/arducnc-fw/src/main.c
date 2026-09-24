/*
 * ArduCNC firmware for the UNO Q STM32U585.
 *
 * SPI3 (DMA slave) exchanges one acnc_cmd/acnc_stat pair per LinuxCNC servo
 * period (see ../common/arducnc_proto.h). Commands feed the DDS stepgen in
 * stepgen.c; the status frame for the next transfer is prepared as soon as a
 * command has been applied.
 *
 * Safety: if no valid command arrives for WATCHDOG_MS while enabled, stepping
 * stops and the drivers are disabled. The trip latches until the host sends a
 * command without ACNC_CMD_ENABLE (i.e. LinuxCNC goes through machine-off).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include "arducnc_proto.h"
#include "matrix.h"
#include "stepgen.h"

#define WATCHDOG_MS 20

static const struct device *const spi = DEVICE_DT_GET(DT_NODELABEL(spi3));
static const struct gpio_dt_spec rdy = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), rdy_gpios);

static const struct spi_config spi_cfg = {
	.operation = SPI_OP_MODE_SLAVE | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	/* The master drives SCLK; the driver still validates this, so not 0. */
	.frequency = 80000000,
};

static union {
	struct acnc_cmd cmd;
	uint8_t raw[ACNC_FRAME_LEN];
} rx __aligned(32);

static union {
	struct acnc_stat stat;
	uint8_t raw[ACNC_FRAME_LEN];
} tx __aligned(32);

static volatile uint32_t frames_ok, frames_bad;
static volatile int64_t last_frame_ms;
static volatile bool wd_tripped, crc_seen;
static uint8_t last_seq;

static void watchdog_fn(struct k_timer *t)
{
	if (stepgen_enabled() && k_uptime_get() - last_frame_ms > WATCHDOG_MS) {
		stepgen_enable(false);
		io_set_outputs(0);
		wd_tripped = true;
	}
}

K_TIMER_DEFINE(watchdog, watchdog_fn, NULL);

static void prepare_status(void)
{
	struct acnc_stat *s = &tx.stat;

	memset(s, 0, sizeof(*s));
	s->magic = ACNC_STAT_MAGIC;
	s->seq_echo = last_seq;
	s->status = (stepgen_enabled() ? ACNC_ST_ENABLED : 0) | (wd_tripped ? ACNC_ST_WATCHDOG : 0) |
		    (crc_seen ? ACNC_ST_CRC_SEEN : 0);
	int32_t steps[ACNC_JOINTS];
	uint16_t imax, iavg;

	stepgen_get_pos(steps);
	memcpy(s->steps, steps, sizeof(steps));
	s->inputs = io_get_inputs();
	s->frames_ok = frames_ok;
	s->frames_bad = frames_bad;
	stepgen_isr_stats(&imax, &iavg);
	s->isr_max_cycles = imax;
	s->isr_avg_cycles = iavg;
	s->proto_version = ACNC_PROTO_VERSION;
	s->crc = acnc_crc16(s, ACNC_FRAME_LEN - 2);
}

static void apply_command(const struct acnc_cmd *c)
{
	bool want = c->flags & ACNC_CMD_ENABLE;

	last_seq = c->seq;
	last_frame_ms = k_uptime_get();

	if (wd_tripped) {
		if (want) {
			return; /* stay off until the host acknowledges with enable=0 */
		}
		wd_tripped = false;
	}
	if (want != stepgen_enabled()) {
		stepgen_enable(want);
	}
	int32_t incr[ACNC_JOINTS];

	memcpy(incr, c->dds_incr, sizeof(incr));
	stepgen_set_incr(incr);
	io_set_outputs(want ? c->outputs : 0);
}

/* Once a second on LPUART1 (/dev/ttyHS1); lowest priority so it never delays SPI. */
static void report_thread(void *a, void *b, void *c)
{
	for (;;) {
		k_msleep(1000);
		uint16_t imax, iavg;
		int32_t p[ACNC_JOINTS];

		stepgen_isr_stats(&imax, &iavg);
		stepgen_get_pos(p);
		printk("ok=%u bad=%u en=%d wd=%d pos=%d,%d,%d isr avg/max=%u/%u cyc in=%02x\n",
		       frames_ok, frames_bad, stepgen_enabled(), wd_tripped, p[0], p[1], p[2], iavg,
		       imax, io_get_inputs());
	}
}

K_THREAD_DEFINE(report, 1024, report_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO,
		0, 0);

int main(void)
{
	printk("\narducnc-fw proto %d, %d joints, base %u Hz\n", ACNC_PROTO_VERSION, ACNC_JOINTS,
	       ACNC_BASE_FREQ_HZ);

	if (stepgen_init() != 0) {
		printk("stepgen init failed\n");
		return 0;
	}
	if (matrix_start() != 0) {
		printk("matrix: failed to start\n");
	}
	if (!device_is_ready(spi) || !gpio_is_ready_dt(&rdy)) {
		printk("spi3/rdy not ready\n");
		return 0;
	}
	gpio_pin_configure_dt(&rdy, GPIO_OUTPUT_INACTIVE);
	k_timer_start(&watchdog, K_MSEC(5), K_MSEC(5));

	prepare_status();

	struct spi_buf txb = {.buf = tx.raw, .len = ACNC_FRAME_LEN};
	struct spi_buf rxb = {.buf = rx.raw, .len = ACNC_FRAME_LEN};
	struct spi_buf_set txs = {.buffers = &txb, .count = 1};
	struct spi_buf_set rxs = {.buffers = &rxb, .count = 1};

	for (;;) {
		gpio_pin_set_dt(&rdy, 1);
		int ret = spi_transceive(spi, &spi_cfg, &txs, &rxs);

		gpio_pin_set_dt(&rdy, 0);

		if (ret == ACNC_FRAME_LEN && rx.cmd.magic == ACNC_CMD_MAGIC &&
		    rx.cmd.crc == acnc_crc16(rx.raw, ACNC_FRAME_LEN - 2)) {
			frames_ok++;
			apply_command(&rx.cmd);
		} else {
			frames_bad++;
			crc_seen = true;
		}
		prepare_status();
	}
	return 0;
}
