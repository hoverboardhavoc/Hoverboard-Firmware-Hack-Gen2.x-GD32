/*
 * Hoverboard libopencm3 port — minimal proof-of-concept.
 *
 * Toggles PA5 (the typical user LED on hoverboard mainboards) at ~2 Hz,
 * with the independent watchdog enabled (must be reloaded inside the
 * loop or the chip resets).
 *
 * This is NOT the hoverboard firmware. It's a single-file demonstration
 * that the libopencm3 fork at hoverboardhavoc/libopencm3 actually links
 * for both GD32F103 and GD32F130. The real port (setup.c + main.c +
 * BLDC + comms + ...) is a multi-day effort tracked in PORT.md.
 */

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/iwdg.h>

static void delay_ms_busy(uint32_t ms)
{
    /* Crude software delay; assumes ~8 MHz HSI default. The real port
     * will use SysTick + a tick-counter ISR. */
    for (uint32_t i = 0; i < ms * 1000; i++) {
        __asm__ volatile("nop");
    }
}

int main(void)
{
#if defined(GD32F1X0)
    /* GD32F130: modern v2 GPIO. Configure PA5 as push-pull output, high speed. */
    gpio_mode_setup(GPIOA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO5);
    gpio_set_output_options(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_HIGH, GPIO5);
#else
    /* GD32F103: legacy v1 GPIO. Combined CRL/CRH MODE+CNF. */
    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, GPIO5);
#endif

    /* Watchdog: ~2 second timeout. */
    iwdg_set_period_ms(2000);
    iwdg_start();

    while (1) {
        gpio_toggle(GPIOA, GPIO5);
        delay_ms_busy(250);
        iwdg_reset();
    }
}

/* Minimal Cortex-M reset vector + initial stack pointer. The real port
 * will use libopencm3's nvic.h vector tables once we have a proper
 * setup.c equivalent. */

extern unsigned long _estack;
void _start(void);

__attribute__((section(".vectors"), used))
void *const vector_table[] = {
    &_estack,
    (void *)main,           /* Reset_Handler — directly enter main(). */
};
