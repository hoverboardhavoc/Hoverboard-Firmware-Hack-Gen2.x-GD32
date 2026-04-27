/*
 * Hoverboard libopencm3 port — main.c (skeleton)
 *
 * The original main.c is 569 lines of business logic + state-machine
 * processing; most of it isn't libopencm3-related (steering arithmetic,
 * battery LED display, master/slave protocol, etc). This skeleton
 * brings up the peripherals via the ported setup.c functions, sets up
 * SysTick at 1 ms, then enters a minimal loop that just blinks the
 * debug LED + reloads the watchdog.
 *
 * To extend: copy logic from HoverBoardGigaDevice/Src/main.c into the
 * main loop block. Most of it ("if (timedOut) ...", master-slave
 * exchange, PWM duty calculation) is libopencm3-agnostic.
 */

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/iwdg.h>

#include <stdint.h>

/* Defined in setup.c. */
extern void Interrupt_init(void);
extern int  Watchdog_init(void);
extern void TimeoutTimer_init(void);
extern void GPIO_init(void);
extern void PWM_init(void);
extern void ADC_init(void);
extern void USART0_Init(uint32_t iBaud);
extern void Clock_init(void);

/* Defined in it.c. */
extern uint32_t msTicks;

/* Globals the it.c handlers reference. The real definitions belong in
 * the not-yet-ported main.c business logic. */
int32_t           steer = 0;
int32_t           speed = 0;
volatile int      activateWeakening = 0;
volatile int      beepsBackwards = 1;

#define DEBUG_LED_PORT GPIOA
#define DEBUG_LED_PIN  GPIO5

static void delay_ms(uint32_t ms)
{
    uint32_t target = msTicks + ms;
    while (msTicks < target) {
        __asm__ volatile("wfi");
    }
}

int main(void)
{
    /* TODO: Clock_init() should run first to set 72 MHz from HXTAL.
     * For now we run on the default HSI 8 MHz clock — the setup
     * functions still work, just slower. */
    Clock_init();

    /* Configure SysTick at the HSI frequency. */
    systick_set_clocksource(STK_CSR_CLKSOURCE_AHB_DIV8);
    systick_set_reload(8000 - 1);   /* 8 MHz / 8 / 1000 Hz - 1 */
    systick_clear();
    systick_counter_enable();
    systick_interrupt_enable();

    Interrupt_init();
    GPIO_init();
    Watchdog_init();
    TimeoutTimer_init();
    PWM_init();
    ADC_init();
    USART0_Init(115200);

    /* Minimal post-init loop: blink debug LED at 2 Hz, reload watchdog.
     * The real hoverboard main loop runs the state machine, processes
     * remote commands, drives the LED battery indicator, etc. */
    while (1) {
        gpio_toggle(DEBUG_LED_PORT, DEBUG_LED_PIN);
        delay_ms(250);
        iwdg_reset();
    }
}
