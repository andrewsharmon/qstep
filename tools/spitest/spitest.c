/*
 * ArduCNC SPI link test (QRB2210 / Linux side).
 *
 * Runs a SCHED_FIFO loop at a fixed rate (default 1 kHz, like a LinuxCNC servo
 * thread), does one full-duplex spidev transfer per period and checks that the
 * STM32 echoes the previous frame back byte for byte (see
 * firmware/spi-echo/src/main.c for the frame format).
 *
 * usage: spitest [-d /dev/spidev0.0] [-s hz] [-n frames] [-p period_us] [-c cpu]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define FRAME_LEN 64
#define HIST_US   2000

static int64_t ns_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static unsigned hist[HIST_US + 1];

static unsigned pct(unsigned total, double p)
{
	unsigned target = (unsigned)(total * p), acc = 0;

	for (unsigned i = 0; i <= HIST_US; i++) {
		acc += hist[i];
		if (acc >= target && acc > 0)
			return i;
	}
	return HIST_US;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/spidev0.0";
	uint32_t speed = 1000000;
	long nframes = 10000, period_us = 1000;
	int cpu = 3, opt;

	while ((opt = getopt(argc, argv, "d:s:n:p:c:")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 's': speed = strtoul(optarg, NULL, 0); break;
		case 'n': nframes = strtol(optarg, NULL, 0); break;
		case 'p': period_us = strtol(optarg, NULL, 0); break;
		case 'c': cpu = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s [-d dev] [-s hz] [-n frames] [-p period_us] [-c cpu]\n", argv[0]);
			return 2;
		}
	}

	int fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror(dev);
		return 1;
	}
	uint8_t mode = SPI_MODE_0, bits = 8;
	if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 || ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
	    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
		perror("spi setup");
		return 1;
	}

	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) < 0)
		perror("sched_setaffinity");
	struct sched_param sp = {.sched_priority = 80};
	if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
		perror("sched_setscheduler (not realtime!)");
	if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
		perror("mlockall");

	/* Keep all CPUs out of deep idle states while we run (held until exit);
	 * waking a power-collapsed Qualcomm core costs milliseconds. */
	int lat_fd = open("/dev/cpu_dma_latency", O_WRONLY);
	int32_t zero = 0;
	if (lat_fd < 0 || write(lat_fd, &zero, sizeof(zero)) != sizeof(zero))
		perror("cpu_dma_latency");

	uint8_t tx[FRAME_LEN], rx[FRAME_LEN], prev[FRAME_LEN];
	struct spi_ioc_transfer xfer = {
		.tx_buf = (uintptr_t)tx,
		.rx_buf = (uintptr_t)rx,
		.len = FRAME_LEN,
		.speed_hz = speed,
		.bits_per_word = 8,
	};

	long ok = 0, bad_hdr = 0, bad_seq = 0, bad_data = 0, ioerr = 0, late = 0;
	unsigned max_us = 0;
	long max_at = -1, over_period = 0;
	uint64_t sum_ns = 0;
	int have_prev = 0;
	uint32_t rng = 0x12345678;

	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);

	for (long n = 0; n < nframes; n++) {
		next.tv_nsec += period_us * 1000;
		while (next.tv_nsec >= 1000000000) {
			next.tv_nsec -= 1000000000;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

		tx[0] = 0xA5;
		tx[1] = (uint8_t)n;
		tx[2] = tx[3] = 0;
		for (int i = 4; i < FRAME_LEN; i++) {
			rng = rng * 1103515245u + 12345u;
			tx[i] = rng >> 24;
		}

		int64_t t0 = ns_now();
		int r = ioctl(fd, SPI_IOC_MESSAGE(1), &xfer);
		int64_t dt = ns_now() - t0;

		if (r < 0) {
			ioerr++;
			continue;
		}
		unsigned us = (unsigned)(dt / 1000);
		hist[us > HIST_US ? HIST_US : us]++;
		sum_ns += dt;
		if (us > max_us) {
			max_us = us;
			max_at = n;
		}
		if (us >= (unsigned)period_us)
			over_period++;
		if (dt > period_us * 1000LL / 2)
			late++;

		if (have_prev) {
			if (rx[0] != 0x5A)
				bad_hdr++;
			else if (rx[1] != prev[1])
				bad_seq++;
			else if (memcmp(&rx[4], &prev[4], FRAME_LEN - 4) != 0)
				bad_data++;
			else
				ok++;
		}
		memcpy(prev, tx, FRAME_LEN);
		have_prev = 1;
	}

	long checked = nframes - 1 - ioerr;
	unsigned total = 0;
	for (unsigned i = 0; i <= HIST_US; i++)
		total += hist[i];

	printf("speed=%u Hz frames=%ld period=%ld us\n", speed, nframes, period_us);
	printf("  echo ok=%ld/%ld  bad_hdr=%ld bad_seq=%ld bad_data=%ld ioerr=%ld\n", ok, checked,
	       bad_hdr, bad_seq, bad_data, ioerr);
	printf("  xfer us: avg=%.1f p50=%u p99=%u p99.9=%u max=%u  (>half period: %ld)\n",
	       total ? sum_ns / 1000.0 / total : 0.0, pct(total, 0.50), pct(total, 0.99),
	       pct(total, 0.999), max_us, late);
	printf("  slowest transfer was #%ld; %ld transfers took >= one period\n", max_at, over_period);
	return (ok == checked) ? 0 : 1;
}
