/*
 * ArduCNC SPI link test firmware (STM32U585 side).
 *
 * Linux (spidev0.0, master) clocks fixed-size frames into SPI3 (slave, DMA).
 * Each reply carries the previous request back so the master can check every
 * byte of the round trip:
 *
 *   request  : [0]=0xA5 [1]=seq [2..3]=unused [4..N-1]=payload
 *   reply    : [0]=0x5A [1]=seq of previous request [2..3]=frames rx (LE16)
 *              [4..N-1]=payload of previous request
 *
 * RDY (PG13) is high while the slave is armed and waiting for a frame.
 * Stats are printed once per second on LPUART1 (/dev/ttyHS1 on Linux).
 * The LED matrix shows "Linux" scrolling over "CNC" while this runs.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include "matrix.h"

#define FRAME_LEN 64

static const struct device *const spi = DEVICE_DT_GET(DT_NODELABEL(spi3));
static const struct gpio_dt_spec rdy = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), rdy_gpios);

static const struct spi_config spi_cfg = {
	.operation = SPI_OP_MODE_SLAVE | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	/* The master drives SCLK, but the driver still validates this value
	 * against the prescaler table, so it must not be 0. */
	.frequency = 80000000,
	.slave = 0,
};

static uint8_t rx_buf[FRAME_LEN] __aligned(32);
static uint8_t tx_buf[FRAME_LEN] __aligned(32);

static volatile uint32_t frames, bad_len, bad_hdr, errors;
static volatile int last_err;

/* printk on the polled UART busy-waits ~5 ms per line, so report from the
 * lowest-priority thread; the SPI loop in main() preempts it. */
static void report_thread(void *a, void *b, void *c)
{
	for (;;) {
		k_msleep(1000);
		printk("frames=%u bad_len=%u bad_hdr=%u err=%u (last %d)\n", frames, bad_len,
		       bad_hdr, errors, last_err);
	}
}

K_THREAD_DEFINE(report, 1024, report_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

int main(void)
{
	printk("\narducnc spi-echo: frame %d bytes\n", FRAME_LEN);

	if (matrix_start() != 0) {
		printk("matrix: failed to start\n");
	}
	if (!device_is_ready(spi) || !gpio_is_ready_dt(&rdy)) {
		printk("spi3/rdy not ready\n");
		return 0;
	}
	gpio_pin_configure_dt(&rdy, GPIO_OUTPUT_INACTIVE);

	memset(tx_buf, 0, sizeof(tx_buf));
	tx_buf[0] = 0x5A;

	struct spi_buf txb = {.buf = tx_buf, .len = FRAME_LEN};
	struct spi_buf rxb = {.buf = rx_buf, .len = FRAME_LEN};
	struct spi_buf_set txs = {.buffers = &txb, .count = 1};
	struct spi_buf_set rxs = {.buffers = &rxb, .count = 1};

	for (;;) {
		gpio_pin_set_dt(&rdy, 1);
		int ret = spi_transceive(spi, &spi_cfg, &txs, &rxs);

		gpio_pin_set_dt(&rdy, 0);

		if (ret < 0) {
			errors++;
			last_err = ret;
		} else if (ret != FRAME_LEN) {
			bad_len++;
		} else if (rx_buf[0] != 0xA5) {
			bad_hdr++;
		} else {
			frames++;
			/* Echo this request back in the next transfer. */
			tx_buf[0] = 0x5A;
			tx_buf[1] = rx_buf[1];
			tx_buf[2] = frames & 0xFF;
			tx_buf[3] = (frames >> 8) & 0xFF;
			memcpy(&tx_buf[4], &rx_buf[4], FRAME_LEN - 4);
		}
	}
	return 0;
}
