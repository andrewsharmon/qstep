#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "arducnc_proto.h"

/* Configure step/dir/enable/IO pins and start the TIM6 stepgen tick. */
int stepgen_init(void);

/* Set the per-tick DDS increments (signed, sign = direction). */
void stepgen_set_incr(const int32_t incr[ACNC_JOINTS]);

/* Enable or disable the drivers. Disabling also zeroes all increments. */
void stepgen_enable(bool on);
bool stepgen_enabled(void);

/* Absolute step counts per joint. */
void stepgen_get_pos(int32_t pos[ACNC_JOINTS]);

/* Stepgen ISR cost in CPU cycles (worst since boot, running average). */
void stepgen_isr_stats(uint16_t *max_cycles, uint16_t *avg_cycles);

void io_set_outputs(uint32_t outputs);
uint32_t io_get_inputs(void);
