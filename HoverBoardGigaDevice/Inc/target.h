/* target.h — GD32F130 (libopencm3) target definitions.
 *
 * Post-port: this file used to carry per-family branches (GD32F103,
 * GD32E230) and a long table of TARGET_* shims that re-spelled GD vendor
 * macro names as STM32-family equivalents. Per the brief's F130-only
 * directive and guardrail #5 (no SPL-shaped shims), all of that is gone.
 *
 * What stays:
 *  - PA0..PF7 packed pin codes ((port_base | pin_index) in one uint32_t)
 *  - PIN_TO_CHANNEL — pin code to ADC channel index
 *  - pinMode / pinModePull / pinModeSpeed / pinModeAF — firmware-shape
 *    helpers that decode the packed pin code and emit libopencm3 calls.
 *    They're firmware-internal abstractions, not SPL HAL recreations.
 *  - digitalWrite / digitalRead — same.
 */

#ifndef TARGET_H
#define TARGET_H

/* Use the gd32/<peripheral>.h dispatchers rather than the f1x0/-specific
 * headers directly where available: the dispatchers pull in memorymap.h
 * before the peripheral header, which is required because gpio_common_f24.h's
 * GPIOA = GPIO_PORT_A_BASE expansion needs GPIO_PORT_A_BASE defined. */
#include <stdint.h>
#include <stdbool.h>
#include <libopencm3/cm3/common.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/gd32/rcc.h>
#include <libopencm3/gd32/gpio.h>
#include <libopencm3/gd32/f1x0/timer.h>
#include <libopencm3/gd32/f1x0/dma.h>
#include <libopencm3/gd32/f1x0/usart.h>
#include <libopencm3/gd32/f1x0/iwdg.h>
#include <libopencm3/gd32/f1x0/adc.h>
#include <libopencm3/gd32/f1x0/i2c.h>
#include <libopencm3/gd32/f1x0/flash.h>
#include <libopencm3/gd32/f1x0/nvic.h>

/* SPL-vocabulary state-flag types — preserved post-port so the firmware's
 * existing `... == RESET` / `return SUCCESS;` idioms continue to work
 * without per-callsite edits. Standalone enum aliases; not a HAL shim. */
typedef enum { RESET = 0, SET = 1 } FlagStatus;
typedef enum { ERROR = 0, SUCCESS = 1 } ErrStatus;

/* SystemCoreClock — was an SPL/CMSIS global, set by SystemInit at boot.
 * libopencm3's equivalent (set by rcc_clock_setup_pll inside clock_init)
 * is `rcc_ahb_frequency`. After clock_init, both read 72_000_000. */
#define SystemCoreClock  rcc_ahb_frequency


/* GD32F130 has 10 ADC channels: PA0..PA7 = 0..7, PB0..PB1 = 8..9. The
 * 64-pin package adds further channels on GPIOC; the 48-pin package
 * (this firmware's target) does not. */
#define PIN_TO_CHANNEL(pin) ((pin & 0xffffff00U) == GPIOA \
				? (pin & 0xfU) \
				: ((pin & 0xfU) + 8))


/* Decode a packed pin code (uint32_t = port_base | pin_index) into the
 * port and bit-mask arguments libopencm3 expects. The do/while wrap lets
 * these expand as statements anywhere a function call would be valid. */

#define pinMode(pin, mode) \
	do { \
		gpio_mode_setup((pin) & 0xffffff00U, (mode), GPIO_PUPD_NONE, \
				1U << ((pin) & 0xfU)); \
		gpio_set_output_options((pin) & 0xffffff00U, GPIO_OTYPE_PP, \
				GPIO_OSPEED_MED, 1U << ((pin) & 0xfU)); \
	} while (0)

#define pinModePull(pin, mode, pull) \
	do { \
		gpio_mode_setup((pin) & 0xffffff00U, (mode), (pull), \
				1U << ((pin) & 0xfU)); \
		gpio_set_output_options((pin) & 0xffffff00U, GPIO_OTYPE_PP, \
				GPIO_OSPEED_MED, 1U << ((pin) & 0xfU)); \
	} while (0)

#define pinModeSpeed(pin, mode, speed) \
	do { \
		gpio_mode_setup((pin) & 0xffffff00U, (mode), GPIO_PUPD_NONE, \
				1U << ((pin) & 0xfU)); \
		gpio_set_output_options((pin) & 0xffffff00U, GPIO_OTYPE_PP, \
				(speed), 1U << ((pin) & 0xfU)); \
	} while (0)

/* AF callback macros from the original firmware encoded the per-pin
 * alternate-function lookup as pin-conditional ternaries (e.g.
 * AF_USART0_TX(pin) = (pin==PB6 ? GPIO_AF_0 : GPIO_AF_1)). Those names
 * were SPL-style (GPIO_AF_n with underscore); libopencm3 uses GPIO_AFn.
 * Per the GD32F130 datasheet 2.6.7 alt-function table:
 *   TIMER0 (= TIM1) channels      → AF2
 *   TIMER0 BRKIN on PA6 / PB12    → AF2
 *   USART0 on PB6 / PB7           → AF0
 *   USART0 on PA2 / PA3 / PA9..15 → AF1
 *   USART1 on PA8 / PB0           → AF4
 *   USART1 on PA2 / PA3 / PA14..15 → AF1
 */
#define AF_TIMER0_BLDC(pin)   GPIO_AF2
#define AF_TIMER0_BRKIN(pin)  GPIO_AF2
#define AF_USART0_TX(pin)     ((pin) == PB6 ? GPIO_AF0 : GPIO_AF1)
#define AF_USART0_RX(pin)     ((pin) == PB7 ? GPIO_AF0 : GPIO_AF1)
#define AF_USART1_TX(pin)     ((pin) == PA8 ? GPIO_AF4 : GPIO_AF1)
#define AF_USART1_RX(pin)     ((pin) == PB0 ? GPIO_AF4 : GPIO_AF1)

#define pinModeAF(pin, AF, pullUpDown, speed) \
	do { \
		gpio_mode_setup((pin) & 0xffffff00U, GPIO_MODE_AF, (pullUpDown), \
				1U << ((pin) & 0xfU)); \
		gpio_set_output_options((pin) & 0xffffff00U, GPIO_OTYPE_PP, \
				(speed), 1U << ((pin) & 0xfU)); \
		gpio_set_af((pin) & 0xffffff00U, AF(pin), \
				1U << ((pin) & 0xfU)); \
	} while (0)


/* digitalWrite/digitalRead — libopencm3's gpio_set / gpio_clear take
 * separate flow but the SPL-style "set this pin to value" pattern is
 * common firmware vocabulary; preserved as ternary-driven select. */
#define digitalWrite(pin, val) \
	do { \
		if (val) { \
			gpio_set((pin) & 0xffffff00U, 1U << ((pin) & 0xfU)); \
		} else { \
			gpio_clear((pin) & 0xffffff00U, 1U << ((pin) & 0xfU)); \
		} \
	} while (0)

#define digitalRead(pin) \
	(gpio_get((pin) & 0xffffff00U, 1U << ((pin) & 0xfU)) ? 1 : 0)


/* Packed pin-code constants. Port base in the high 24 bits, pin index
 * in the low 4 bits. libopencm3's GPIOA/GPIOB/GPIOC/GPIOF resolve to
 * the same physical addresses as the SPL macros of the same name —
 * verified by regtrace v0.2 and the gpio/output_pa5_pp_50mhz vector. */

#define PA0  ((uint32_t)GPIOA | 0)
#define PA1  ((uint32_t)GPIOA | 1)
#define PA2  ((uint32_t)GPIOA | 2)
#define PA3  ((uint32_t)GPIOA | 3)
#define PA4  ((uint32_t)GPIOA | 4)
#define PA5  ((uint32_t)GPIOA | 5)
#define PA6  ((uint32_t)GPIOA | 6)
#define PA7  ((uint32_t)GPIOA | 7)
#define PA8  ((uint32_t)GPIOA | 8)
#define PA9  ((uint32_t)GPIOA | 9)
#define PA10 ((uint32_t)GPIOA | 10)
#define PA11 ((uint32_t)GPIOA | 11)
#define PA12 ((uint32_t)GPIOA | 12)
#define PA13 ((uint32_t)GPIOA | 13)
#define PA14 ((uint32_t)GPIOA | 14)
#define PA15 ((uint32_t)GPIOA | 15)

#define PB0  ((uint32_t)GPIOB | 0)
#define PB1  ((uint32_t)GPIOB | 1)
#define PB2  ((uint32_t)GPIOB | 2)
#define PB3  ((uint32_t)GPIOB | 3)
#define PB4  ((uint32_t)GPIOB | 4)
#define PB5  ((uint32_t)GPIOB | 5)
#define PB6  ((uint32_t)GPIOB | 6)
#define PB7  ((uint32_t)GPIOB | 7)
#define PB8  ((uint32_t)GPIOB | 8)
#define PB9  ((uint32_t)GPIOB | 9)
#define PB10 ((uint32_t)GPIOB | 10)
#define PB11 ((uint32_t)GPIOB | 11)
#define PB12 ((uint32_t)GPIOB | 12)
#define PB13 ((uint32_t)GPIOB | 13)
#define PB14 ((uint32_t)GPIOB | 14)
#define PB15 ((uint32_t)GPIOB | 15)

#define PC0  ((uint32_t)GPIOC | 0)
#define PC1  ((uint32_t)GPIOC | 1)
#define PC2  ((uint32_t)GPIOC | 2)
#define PC3  ((uint32_t)GPIOC | 3)
#define PC4  ((uint32_t)GPIOC | 4)
#define PC5  ((uint32_t)GPIOC | 5)
#define PC6  ((uint32_t)GPIOC | 6)
#define PC7  ((uint32_t)GPIOC | 7)
#define PC8  ((uint32_t)GPIOC | 8)
#define PC9  ((uint32_t)GPIOC | 9)
#define PC10 ((uint32_t)GPIOC | 10)
#define PC11 ((uint32_t)GPIOC | 11)
#define PC12 ((uint32_t)GPIOC | 12)
#define PC13 ((uint32_t)GPIOC | 13)
#define PC14 ((uint32_t)GPIOC | 14)
#define PC15 ((uint32_t)GPIOC | 15)

#define PF0 ((uint32_t)GPIOF | 0)
#define PF1 ((uint32_t)GPIOF | 1)
#define PF4 ((uint32_t)GPIOF | 4)
#define PF6 ((uint32_t)GPIOF | 6)
#define PF7 ((uint32_t)GPIOF | 7)

/* Legacy pin-index aliases used by a few callers. */
#define PB1i  17
#define PB2i  18
#define PB11i 27
#define PC14i 62
#define PF1i  81

/* GD32F130 48-pin package available IO pins:
 *   C13 C14 C15 F0 F1
 *   A0 A1 A2 A3 A4 A5 A6 A7
 *   B0 B1 B2 B10 B11 B12 B13 B14 B15
 *   A8 A9 A10 A11 A12 A13 F6 F7
 *   A14 A15 B3 B4 B5 B6 B7 B8 B9
 */

#endif /* TARGET_H */
