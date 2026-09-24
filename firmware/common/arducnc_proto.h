/*
 * ArduCNC link protocol: LinuxCNC (QRB2210, spidev master) <-> STM32U585 (SPI3 slave).
 *
 * One fixed-size full-duplex transfer per LinuxCNC servo period. Every transfer
 * carries a command frame to the MCU and, at the same time, the status frame the
 * MCU prepared right after the previous command. All fields are little-endian.
 * The last two bytes of each frame are a CRC-16/CCITT-FALSE over the rest.
 *
 * Step generation is velocity-mode (like Remora/Mesa stepgen): the host sends a
 * signed DDS increment per joint; the MCU adds it to a 32-bit accumulator every
 * base tick and emits one step per accumulator overflow. The MCU reports the
 * absolute step count per joint back; the host closes the position loop.
 *
 * Shared by firmware/arducnc-fw and hal/unoq_spi.c — keep them in sync.
 */
#ifndef ARDUCNC_PROTO_H
#define ARDUCNC_PROTO_H

#include <stdint.h>

#define ACNC_FRAME_LEN      64
#define ACNC_JOINTS         4
#define ACNC_BASE_FREQ_HZ   100000u   /* stepgen tick rate on the MCU */
#define ACNC_PROTO_VERSION  1

#define ACNC_CMD_MAGIC      0xAC01u
#define ACNC_STAT_MAGIC     0xAC81u

/* cmd.flags */
#define ACNC_CMD_ENABLE     (1u << 0)  /* drivers enabled, stepping allowed */

/* stat.status */
#define ACNC_ST_ENABLED     (1u << 0)  /* drivers are enabled right now */
#define ACNC_ST_WATCHDOG    (1u << 1)  /* link watchdog tripped; clear with a frame without ENABLE */
#define ACNC_ST_CRC_SEEN    (1u << 2)  /* at least one bad frame since boot */

/* Digital outputs (cmd.outputs bits) */
#define ACNC_OUT_SPINDLE_EN   (1u << 0)
#define ACNC_OUT_SPINDLE_DIR  (1u << 1)
#define ACNC_OUT_COOLANT      (1u << 2)

/* Digital inputs (stat.inputs bits), 1 = active (switch closed to GND) */
#define ACNC_IN_LIMIT_X     (1u << 0)
#define ACNC_IN_LIMIT_Y     (1u << 1)
#define ACNC_IN_LIMIT_Z     (1u << 2)
#define ACNC_IN_ABORT       (1u << 3)
#define ACNC_IN_HOLD        (1u << 4)
#define ACNC_IN_RESUME      (1u << 5)
#define ACNC_IN_PROBE       (1u << 6)

struct __attribute__((packed)) acnc_cmd {
	uint16_t magic;                 /* ACNC_CMD_MAGIC */
	uint8_t  seq;                   /* incremented every frame */
	uint8_t  flags;                 /* ACNC_CMD_* */
	int32_t  dds_incr[ACNC_JOINTS]; /* signed; |incr| <= 2^31, sign = direction */
	uint32_t outputs;               /* ACNC_OUT_* */
	uint16_t spindle_pwm;           /* 0..65535 duty (reserved, not wired yet) */
	uint8_t  reserved[ACNC_FRAME_LEN - 2 - 1 - 1 - 4 * ACNC_JOINTS - 4 - 2 - 2];
	uint16_t crc;
};

struct __attribute__((packed)) acnc_stat {
	uint16_t magic;                 /* ACNC_STAT_MAGIC */
	uint8_t  seq_echo;              /* seq of the command this status follows */
	uint8_t  status;                /* ACNC_ST_* */
	int32_t  steps[ACNC_JOINTS];    /* absolute step position per joint */
	uint32_t inputs;                /* ACNC_IN_* */
	uint32_t frames_ok;
	uint32_t frames_bad;
	uint16_t isr_max_cycles;        /* worst stepgen ISR duration since boot (CPU cycles) */
	uint16_t isr_avg_cycles;
	uint8_t  proto_version;         /* ACNC_PROTO_VERSION */
	uint8_t  reserved[ACNC_FRAME_LEN - 2 - 1 - 1 - 4 * ACNC_JOINTS - 4 - 4 - 4 - 2 - 2 - 1 - 2];
	uint16_t crc;
};

_Static_assert(sizeof(struct acnc_cmd) == ACNC_FRAME_LEN, "cmd frame size");
_Static_assert(sizeof(struct acnc_stat) == ACNC_FRAME_LEN, "stat frame size");

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), bitwise; 62 bytes per frame. */
static inline uint16_t acnc_crc16(const void *data, unsigned len)
{
	const uint8_t *p = (const uint8_t *)data;
	uint16_t crc = 0xFFFF;

	while (len--) {
		crc ^= (uint16_t)(*p++) << 8;
		for (int i = 0; i < 8; i++) {
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

#endif /* ARDUCNC_PROTO_H */
