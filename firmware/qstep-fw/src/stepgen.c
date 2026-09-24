/*
 * DDS step generation for up to QSTEP_JOINTS joints.
 *
 * TIM6 interrupts at QSTEP_BASE_FREQ_HZ (100 kHz). Each tick:
 *   1. lower the STEP pins raised on the previous tick (10 us pulses),
 *   2. per joint, add |incr| to a 32-bit accumulator; an overflow is one step.
 * |incr| is clamped to 2^31, so a joint steps at most every other tick
 * (50 kHz max, 10 us high / >= 10 us low, well inside the DRV8825 limits).
 * A direction change holds stepping for DIR_HOLD_TICKS (20 us setup time;
 * the DRV8825 needs 650 ns).
 *
 * The ISR is a Zephyr "direct" ISR at the highest priority. It never calls
 * the kernel and touches the GPIO registers directly.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq.h>
#include <stm32u5xx.h>
#include <stm32u5xx_ll_bus.h>

#include "pins.h"
#include "stepgen.h"

#define DIR_HOLD_TICKS 2
#define INCR_MAX       0x80000000u

static const struct pin step_pins[QSTEP_JOINTS] = STEP_PINS;
static const struct pin dir_pins[QSTEP_JOINTS] = DIR_PINS;
static const struct pin enable_pin = ENABLE_PIN;
static const struct pin input_pins[] = INPUT_PINS;
static const struct pin output_pins[] = OUTPUT_PINS;

static volatile int32_t incr[QSTEP_JOINTS];
static volatile int32_t pos[QSTEP_JOINTS];
static volatile bool enabled;

static uint32_t acc[QSTEP_JOINTS];
static int8_t cur_dir[QSTEP_JOINTS] = {1, 1, 1, 1};
static uint8_t hold[QSTEP_JOINTS];
static bool pending[QSTEP_JOINTS];

/* STEP pins raised on the last tick, per port, so the next tick can lower them. */
static uint32_t raised_a, raised_b, raised_c;

static volatile uint32_t isr_max, isr_sum, isr_count;
static volatile uint16_t isr_avg;

static inline void pin_write(const struct pin *p, bool high)
{
	if (p->port) {
		p->port->BSRR = high ? (1U << p->bit) : (1U << (p->bit + 16));
	}
}

static inline uint32_t *raised_for(GPIO_TypeDef *port)
{
	return port == GPIOA ? &raised_a : port == GPIOB ? &raised_b : &raised_c;
}

ISR_DIRECT_DECLARE(stepgen_isr)
{
	uint32_t t0 = DWT->CYCCNT;

	TIM6->SR = 0;

	/* End the pulses started on the previous tick. */
	GPIOA->BSRR = raised_a << 16;
	GPIOB->BSRR = raised_b << 16;
	GPIOC->BSRR = raised_c << 16;
	raised_a = raised_b = raised_c = 0;

	if (enabled) {
		for (int j = 0; j < QSTEP_JOINTS; j++) {
			if (!step_pins[j].port) {
				continue;
			}
			int32_t inc = incr[j];
			uint32_t mag = inc < 0 ? (uint32_t)(-(int64_t)inc) : (uint32_t)inc;
			int8_t dir = inc < 0 ? -1 : (inc > 0 ? 1 : cur_dir[j]);

			if (mag > INCR_MAX) {
				mag = INCR_MAX;
			}
			if (dir != cur_dir[j]) {
				cur_dir[j] = dir;
				pin_write(&dir_pins[j], dir > 0);
				hold[j] = DIR_HOLD_TICKS;
			}

			uint32_t old = acc[j];

			acc[j] = old + mag;
			if (acc[j] < old) {
				pending[j] = true;
			}
			if (hold[j]) {
				hold[j]--;
			} else if (pending[j]) {
				pending[j] = false;
				*raised_for(step_pins[j].port) |= 1U << step_pins[j].bit;
				pos[j] += cur_dir[j];
			}
		}
		GPIOA->BSRR = raised_a;
		GPIOB->BSRR = raised_b;
		GPIOC->BSRR = raised_c;
	}

	uint32_t dt = DWT->CYCCNT - t0;

	if (dt > isr_max) {
		isr_max = dt;
	}
	isr_sum += dt;
	if (++isr_count == 65536) {
		isr_avg = isr_sum >> 16;
		isr_sum = 0;
		isr_count = 0;
	}
	return 0; /* no reschedule needed */
}

static const struct device *port_dev(GPIO_TypeDef *port)
{
	if (port == GPIOA) {
		return DEVICE_DT_GET(DT_NODELABEL(gpioa));
	}
	if (port == GPIOB) {
		return DEVICE_DT_GET(DT_NODELABEL(gpiob));
	}
	return DEVICE_DT_GET(DT_NODELABEL(gpioc));
}

static int pin_setup(const struct pin *p, gpio_flags_t flags)
{
	if (!p->port) {
		return 0;
	}
	const struct device *dev = port_dev(p->port);

	/* Readying the port device also turns on its clock. */
	if (!device_is_ready(dev)) {
		return -ENODEV;
	}
	return gpio_pin_configure(dev, p->bit, flags);
}

int stepgen_init(void)
{
	int err = 0;

	/* Drivers disabled (EN high) before anything else. */
	err |= pin_setup(&enable_pin, GPIO_OUTPUT_HIGH);
	for (int j = 0; j < QSTEP_JOINTS; j++) {
		err |= pin_setup(&step_pins[j], GPIO_OUTPUT_LOW);
		err |= pin_setup(&dir_pins[j], GPIO_OUTPUT_HIGH);
	}
	for (size_t i = 0; i < ARRAY_SIZE(input_pins); i++) {
		err |= pin_setup(&input_pins[i], GPIO_INPUT | GPIO_PULL_UP);
	}
	for (size_t i = 0; i < ARRAY_SIZE(output_pins); i++) {
		err |= pin_setup(&output_pins[i], GPIO_OUTPUT_LOW);
	}
	if (err) {
		return -EIO;
	}

	/* Cycle counter for ISR timing. */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

	/* TIM6: basic timer on APB1 (timer clock = SystemCoreClock here). */
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
	TIM6->CR1 = 0;
	TIM6->PSC = 0;
	TIM6->ARR = (SystemCoreClock / QSTEP_BASE_FREQ_HZ) - 1;
	TIM6->EGR = TIM_EGR_UG;
	TIM6->SR = 0;
	TIM6->DIER = TIM_DIER_UIE;

	IRQ_DIRECT_CONNECT(TIM6_IRQn, 0, stepgen_isr, 0);
	irq_enable(TIM6_IRQn);
	TIM6->CR1 = TIM_CR1_CEN;
	return 0;
}

void stepgen_set_incr(const int32_t in[QSTEP_JOINTS])
{
	for (int j = 0; j < QSTEP_JOINTS; j++) {
		incr[j] = enabled ? in[j] : 0;
	}
}

void stepgen_enable(bool on)
{
	if (!on) {
		for (int j = 0; j < QSTEP_JOINTS; j++) {
			incr[j] = 0;
		}
	}
	enabled = on;
	pin_write(&enable_pin, !on); /* active low */
}

bool stepgen_enabled(void)
{
	return enabled;
}

void stepgen_get_pos(int32_t out[QSTEP_JOINTS])
{
	for (int j = 0; j < QSTEP_JOINTS; j++) {
		out[j] = pos[j];
	}
}

void stepgen_isr_stats(uint16_t *max_cycles, uint16_t *avg_cycles)
{
	*max_cycles = isr_max > 0xFFFF ? 0xFFFF : isr_max;
	*avg_cycles = isr_avg;
}

void io_set_outputs(uint32_t outputs)
{
	for (size_t i = 0; i < ARRAY_SIZE(output_pins); i++) {
		pin_write(&output_pins[i], outputs & (1U << i));
	}
}

uint32_t io_get_inputs(void)
{
	uint32_t in = 0;

	for (size_t i = 0; i < ARRAY_SIZE(input_pins); i++) {
		if (!(input_pins[i].port->IDR & (1U << input_pins[i].bit))) {
			in |= 1U << i;
		}
	}
	return in;
}
