/*
 * Hoverboard libopencm3 port — setup.c
 *
 * This is a clean libopencm3 rewrite of HoverBoardGigaDevice/Src/setup.c.
 * Function signatures match the original so dependent files (main.c, it.c,
 * bldc.c, comms.c, ...) can still link against this once they're ported
 * themselves. Internally every initialisation routine is rewritten using
 * libopencm3 APIs.
 *
 * Status: WORK IN PROGRESS. Functions marked PORTED have been rewritten
 * and exercise libopencm3 APIs validated by regtrace. Functions marked
 * STUB have placeholder bodies; they need to be ported before the
 * full firmware will run on real silicon.
 */

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/iwdg.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/dma.h>
#include <libopencm3/stm32/adc.h>
#include <libopencm3/stm32/usart.h>

/* Project-wide constants (originally from defines.h). Pulled in piecemeal
 * to avoid the gd-spl-tied include chain. */
#define PWM_FREQ            16000           /* PWM frequency in Hz */
#define DEAD_TIME           60              /* PWM deadtime, measured by oscilloscope */
#define TIMEOUT_MS          2000            /* No-steering-command timeout */
#define BLDC_TIMER_PERIOD   2250            /* SystemCoreClock/2/PWM_FREQ at 72 MHz / 16 kHz */

/* The NVIC API in libopencm3 doesn't take a sub-priority arg the way SPL
 * does. We collapse to a single 4-bit priority. Original SPL code passed
 * (irqn, preempt_priority, sub_priority) and used PRIGROUP_PRE4_SUB0 → all
 * four priority bits go to the preempt half, sub-priority is unused.
 * That semantics is preserved here. */
static inline void nvic_setup_irq(uint8_t irqn, uint8_t priority)
{
    nvic_enable_irq(irqn);
    nvic_set_priority(irqn, priority << 4);
}

/* ============================================================
 * PORTED — Interrupt_init
 * Original: sets NVIC priority grouping to PRE4_SUB0.
 * libopencm3 default after reset is already PRE4_SUB0-equivalent
 * (priority grouping is a Cortex-M SCB register; libopencm3's
 * scb_set_priority_grouping wraps it).
 * ============================================================ */
void Interrupt_init(void)
{
    /* PRE4_SUB0 in gd-spl vocabulary = 4 bits of pre-emption (16 levels),
     * 0 bits of sub-priority. libopencm3 calls this GROUP16_NOSUB. */
    scb_set_priority_grouping(SCB_AIRCR_PRIGROUP_GROUP16_NOSUB);
}

/* ============================================================
 * PORTED — Watchdog_init
 * Original: rcu_flag_get + fwdgt_config(0x0FFF, /16) + window mode
 * + fwdgt_enable. ~1638 ms timeout at 40 kHz LSI / divider 16.
 *
 * libopencm3: iwdg_set_period_ms(2048) computes the same prescaler
 * and reload values (validated by regtrace v0.2 IWDG decision).
 * Window mode is not exposed by libopencm3's iwdg_common_v2/all
 * APIs — left out. The hoverboard's window-mode usage was redundant
 * (the application reloads the watchdog inside the main loop, well
 * inside any conceivable window).
 * ============================================================ */
typedef enum { ERROR = 0, SUCCESS = !ERROR } ErrStatus;

ErrStatus Watchdog_init(void)
{
    /* libopencm3 doesn't surface FWDGTRST flag check + clear directly,
     * but the bit lives in RCC_CSR. Skipping the diagnostic check; the
     * original used it only for logging. */

    /* Set ~2 second timeout. iwdg_set_period_ms internally picks
     * prescale=2 (=/16) and reload=0x0FFF for period=2048ms — same
     * register-level state as the gd-spl fwdgt_config(0x0FFF, /16). */
    iwdg_set_period_ms(2048);

    /* Enable. */
    iwdg_start();

    return SUCCESS;
}

/* ============================================================
 * STUB — TimeoutTimer_init
 * TODO: port. Original uses TIM13/TIM14 timeout timer to detect
 * stale steering commands. Needs timer_set_mode/period + NVIC.
 * ============================================================ */
void TimeoutTimer_init(void)
{
    /* TODO: port */
}

/* ============================================================
 * PORTED (partial) — GPIO_init
 * Original: enables GPIOA/B/C/F clocks, then configures ~30 pins for
 * the LED/buzzer/hall/MOSFET roles. Here we only port the clock
 * enables + a couple of representative pins; full pin config has
 * to follow once the rest of the firmware is also ported.
 * ============================================================ */
#define DEBUG_LED_PORT  GPIOA
#define DEBUG_LED_PIN   GPIO5

void GPIO_init(void)
{
    /* Enable GPIO port clocks via libopencm3. The RCC enum names follow
     * the STM32 vocabulary (RCC_GPIOA, ...) — already validated to
     * match the gd-spl rcu_periph_clock_enable(RCU_GPIOA) at the
     * register level by regtrace. */
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_GPIOC);
#ifdef GD32F1X0
    rcc_periph_clock_enable(RCC_GPIOF);
#endif

    /* DEBUG LED on PA5. */
#if defined(GD32F1X0)
    /* F130: v2 GPIO. */
    gpio_mode_setup(DEBUG_LED_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
                    DEBUG_LED_PIN);
    gpio_set_output_options(DEBUG_LED_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_HIGH,
                            DEBUG_LED_PIN);
#elif defined(GD32F10X)
    /* F103: v1 GPIO. Combined CRL/CRH MODE+CNF. */
    gpio_set_mode(DEBUG_LED_PORT, GPIO_MODE_OUTPUT_50_MHZ,
                  GPIO_CNF_OUTPUT_PUSHPULL, DEBUG_LED_PIN);
#endif

    /* TODO: port the remaining ~28 pin configurations. */
}

/* ============================================================
 * STUB — PWM_init (TIM1 advanced timer for FOC)
 * TODO: port. Center-aligned, 16 kHz PWM with complementary outputs +
 * dead-time + break input. The most timing-critical block in setup.c.
 * Per regtrace v0.1 TIMER decision the libopencm3 timer_set_mode +
 * timer_set_period + timer_set_repetition_counter sequence produces
 * register-equivalent state.
 * ============================================================ */
void PWM_init(void)
{
    /* TODO: port */
}

/* ============================================================
 * STUB — ADC_init (regular conversion, channel sequencing, DMA)
 * TODO: port. This is the largest single block in setup.c (~80 lines).
 * Per regtrace v0.2 ADC decision GD32F1x0 ADC is v1-style — share with
 * stm32/common/adc_common_v1 (which the libopencm3 fork provides).
 * ============================================================ */
void ADC_init(void)
{
    /* TODO: port */
}

/* ============================================================
 * STUB — USART_MasterSlave_init / USART_Steering_init
 * TODO: port. Standard USART config (115200 baud, 8N1) + DMA hookup.
 * ============================================================ */
void USART_MasterSlave_init(void)
{
    /* TODO: port */
}

void USART_Steering_init(void)
{
    /* TODO: port */
}

/* ============================================================
 * Stubs for the helpers setup.c originally provided
 * ============================================================ */
void pinMode(uint32_t pin, uint32_t mode)
{
    (void)pin;
    (void)mode;
    /* TODO: port the pinMode helper. Original packs port+pin into a
     * uint32_t; libopencm3 takes (port, mode, pull, pin) directly so
     * this helper is no longer load-bearing. */
}
