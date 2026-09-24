/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 andrewsharmon
 */
/*
 * Pin map: Arduino CNC Shield v3 (GRBL 1.1 layout) on the UNO Q header.
 * Everything that needs to change to move signals (e.g. to JMISC) is here.
 */
#pragma once

#include <stm32u5xx.h>

struct pin {
	GPIO_TypeDef *port;
	uint8_t bit;
};

#define PIN_NONE {NULL, 0}

/* Joints 0..3 = X, Y, Z, A.  A has no dedicated pins on the shield. */
#define STEP_PINS {{GPIOB, 3} /* D2 */, {GPIOB, 0} /* D3 */, {GPIOA, 12} /* D4 */, PIN_NONE}
#define DIR_PINS  {{GPIOA, 11} /* D5 */, {GPIOB, 1} /* D6 */, {GPIOB, 2} /* D7 */, PIN_NONE}

/* Shared driver enable, active low (DRV8825 /ENABLE). */
#define ENABLE_PIN {GPIOB, 4} /* D8 */

/* Inputs, active low with internal pull-up; order matches QSTEP_IN_* bits. */
#define INPUT_PINS {                                                                   \
	{GPIOB, 8},  /* D9  X limit */                                                  \
	{GPIOB, 9},  /* D10 Y limit */                                                  \
	{GPIOB, 15}, /* D11 Z limit */                                                  \
	{GPIOA, 4},  /* A0  abort */                                                    \
	{GPIOA, 5},  /* A1  feed hold */                                                \
	{GPIOA, 6},  /* A2  cycle start/resume */                                       \
	{GPIOC, 0},  /* A5  probe */                                                    \
}

/* Outputs; order matches QSTEP_OUT_* bits. */
#define OUTPUT_PINS {                                                                  \
	{GPIOB, 14}, /* D12 spindle enable */                                           \
	{GPIOB, 13}, /* D13 spindle direction */                                        \
	{GPIOA, 7},  /* A3  coolant */                                                  \
}
