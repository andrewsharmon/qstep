/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 andrewsharmon
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies (LED matrix pin map, from ArduinoCore-zephyr loader/matrix.inc)
 */
/*
 * UNO Q 13x8 LED matrix: "Linux" scrolling over a fixed "CNC".
 *
 * The 104 LEDs are charlieplexed on PF0..PF10 and only one LED is lit at a
 * time. TIM17 steps through them from a lowest-priority interrupt; the scroll
 * position is advanced by a normal Zephyr thread.
 * Pin pairs match Arduino's loader (ArduinoCore-zephyr/loader/matrix.inc).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <stm32u5xx.h>
#include <string.h>

#include "matrix.h"

#define COLS     13
#define ROWS     8
#define NUM_LEDS (COLS * ROWS)

static const uint8_t pins[NUM_LEDS][2] = {
	{0, 1}, {1, 0}, {0, 2}, {2, 0}, {1, 2}, {2, 1}, {0, 3}, {3, 0}, {1, 3}, {3, 1},
	{2, 3}, {3, 2}, {0, 4}, {4, 0}, {1, 4}, {4, 1}, {2, 4}, {4, 2}, {3, 4}, {4, 3},
	{0, 5}, {5, 0}, {1, 5}, {5, 1}, {2, 5}, {5, 2}, {3, 5}, {5, 3}, {4, 5}, {5, 4},
	{0, 6}, {6, 0}, {1, 6}, {6, 1}, {2, 6}, {6, 2}, {3, 6}, {6, 3}, {4, 6}, {6, 4},
	{5, 6}, {6, 5}, {0, 7}, {7, 0}, {1, 7}, {7, 1}, {2, 7}, {7, 2}, {3, 7}, {7, 3},
	{4, 7}, {7, 4}, {5, 7}, {7, 5}, {6, 7}, {7, 6}, {0, 8}, {8, 0}, {1, 8}, {8, 1},
	{2, 8}, {8, 2}, {3, 8}, {8, 3}, {4, 8}, {8, 4}, {5, 8}, {8, 5}, {6, 8}, {8, 6},
	{7, 8}, {8, 7}, {0, 9}, {9, 0}, {1, 9}, {9, 1}, {2, 9}, {9, 2}, {3, 9}, {9, 3},
	{4, 9}, {9, 4}, {5, 9}, {9, 5}, {6, 9}, {9, 6}, {7, 9}, {9, 7}, {8, 9}, {9, 8},
	{0, 10}, {10, 0}, {1, 10}, {10, 1}, {2, 10}, {10, 2}, {3, 10}, {10, 3}, {4, 10}, {10, 4},
	{5, 10}, {10, 5}, {6, 10}, {10, 6},
};

/* One bit per LED, index = row * COLS + col. */
static volatile uint8_t framebuffer[(NUM_LEDS + 7) / 8];

static void led_set(int idx, bool on)
{
	/* All matrix pins (PF0..PF10) to input = off; leave PF11..PF15 alone. */
	GPIOF->MODER &= 0xFFC00000U;
	if (on) {
		uint8_t hi = pins[idx][0];
		uint8_t lo = pins[idx][1];

		GPIOF->BSRR = (1U << hi) | (1U << (lo + 16));
		GPIOF->MODER |= (1U << (hi * 2)) | (1U << (lo * 2));
	}
}

static void scan_isr(const struct device *dev, void *user_data)
{
	static int i;

	led_set(i, (framebuffer[i >> 3] >> (i & 7)) & 1);
	i = (i + 1) % NUM_LEDS;
}

/*
 * Two-line display: "Linux" scrolls on the top line (rows 0-3), "CNC" stays
 * fixed on the bottom line (rows 5-7). Glyphs are column bitmaps, bit 0 = the
 * top row of their line.
 */

/* Top line, 4 rows: L and i are full height, n/u/x use rows 1-3. */
static const uint8_t linux_cols[] = {
	0x0F, 0x08,       /* L */
	0x00,
	0x0D,             /* i */
	0x00,
	0x0E, 0x02, 0x0C, /* n */
	0x00,
	0x06, 0x08, 0x0E, /* u */
	0x00,
	0x0A, 0x04, 0x0A, /* x */
	0x00, 0x00, 0x00, 0x00, /* gap before it wraps around */
};

/* Bottom line, 3 rows: C (3 wide), N (4 wide), C. */
static const uint8_t cnc_cols[] = {
	0x07, 0x05, 0x05,       /* C */
	0x00,
	0x07, 0x01, 0x02, 0x07, /* N */
	0x00,
	0x07, 0x05, 0x05,       /* C */
};

#define TOP_ROW    0
#define TOP_ROWS   4
#define BOTTOM_ROW 5
#define BOTTOM_ROWS 3

static void put_col(uint8_t *fb, int col, int row0, int nrows, uint8_t bits)
{
	for (int r = 0; r < nrows; r++) {
		if (bits & (1U << r)) {
			int idx = (row0 + r) * COLS + col;

			fb[idx >> 3] |= 1U << (idx & 7);
		}
	}
}

static void show_frame(int offset)
{
	uint8_t fb[sizeof(framebuffer)] = {0};
	const int n = ARRAY_SIZE(linux_cols);
	const int cnc_x = (COLS - (int)ARRAY_SIZE(cnc_cols)) / 2;

	for (int col = 0; col < COLS; col++) {
		put_col(fb, col, TOP_ROW, TOP_ROWS, linux_cols[(offset + col) % n]);
	}
	for (int i = 0; i < (int)ARRAY_SIZE(cnc_cols); i++) {
		put_col(fb, cnc_x + i, BOTTOM_ROW, BOTTOM_ROWS, cnc_cols[i]);
	}
	memcpy((void *)framebuffer, fb, sizeof(fb));
}

static void scroll_thread(void *a, void *b, void *c)
{
	for (int off = 0;; off = (off + 1) % ARRAY_SIZE(linux_cols)) {
		show_frame(off);
		k_msleep(120);
	}
}

K_THREAD_STACK_DEFINE(scroll_stack, 768);
static struct k_thread scroll_data;

int matrix_start(void)
{
	const struct device *tim = DEVICE_DT_GET(DT_NODELABEL(counter_matrix));
	struct counter_top_cfg top = {
		.callback = scan_isr,
		.flags = 0,
	};

	/* Getting the port device ready makes Zephyr enable the GPIOF clock. */
	if (!device_is_ready(tim) || !device_is_ready(DEVICE_DT_GET(DT_NODELABEL(gpiof)))) {
		return -ENODEV;
	}
	/* 100 us per LED -> ~96 Hz full refresh. */
	top.ticks = counter_us_to_ticks(tim, 100);
	counter_start(tim);
	int err = counter_set_top_value(tim, &top);

	if (err) {
		return err;
	}

	k_thread_create(&scroll_data, scroll_stack, K_THREAD_STACK_SIZEOF(scroll_stack),
			scroll_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0,
			K_NO_WAIT);
	k_thread_name_set(&scroll_data, "matrix");
	return 0;
}
