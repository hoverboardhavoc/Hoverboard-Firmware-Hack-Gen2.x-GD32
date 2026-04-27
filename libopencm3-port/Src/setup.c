/*
 * Hoverboard libopencm3 port — setup.c
 *
 * Clean libopencm3 rewrite of HoverBoardGigaDevice/Src/setup.c. Function
 * signatures match the original so other ported files can link against
 * this. Internally every initialisation routine is rewritten using
 * libopencm3 APIs.
 *
 * Status: see PORT.md. Most functions PORTED with the canonical happy
 * path (no conditional REMOTE_x / HAS_USARTx / board-variant compiles).
 * Per-board pin remapping (TARGET == 2 for F103) needs a follow-up
 * pass before a real board can be flashed.
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

/* Project-wide constants. Only the ones setup.c actually uses, kept
 * inline here to avoid pulling in defines.h (which transitively pulls
 * in gd-spl headers). */
#define PWM_FREQ            16000U          /* 16 kHz center-aligned PWM */
#define DEAD_TIME           60              /* PWM deadtime in TIM-clock ticks (~1us @ 72MHz) */
#define BLDC_TIMER_PERIOD   2250            /* SystemCoreClock/2/PWM_FREQ at 72 MHz / 16 kHz */
#define ADC_BUFFER_LEN      4               /* 4× uint16_t channels: VBATT, CURRENT_DC, PA2, PA3 */
#define USART0_BAUD         115200
#define DEBUG_LED_PORT      GPIOA
#define DEBUG_LED_PIN       GPIO5

/* Standard ADC streaming buffer. Scoped here for the port; the original
 * declared it in main.c as an `adc_buf_t`. */
volatile uint16_t adc_buffer[ADC_BUFFER_LEN];

/* USART RX buffer for the master/slave protocol. */
volatile uint8_t usart0_rx_buf[1];

typedef enum { ERROR = 0, SUCCESS = !ERROR } ErrStatus;

/* The NVIC API in libopencm3 doesn't take a sub-priority arg the way SPL
 * does. The hoverboard always uses PRE4_SUB0 (no sub-priority), so we
 * collapse to a single priority value. */
static inline void nvic_setup_irq(uint8_t irqn, uint8_t priority)
{
    nvic_enable_irq(irqn);
    nvic_set_priority(irqn, (uint8_t)(priority << 4));
}

/* Per-family aliases. F1x0 and F10x have different libopencm3 RCC enum
 * names + NVIC IRQ names (F1x0 uses RCC_ADC/RCC_DMA singletons + combined
 * IRQs; F10x mirrors STM32F1 with RCC_ADC1/RCC_DMA1 + split IRQs). */
#if defined(GD32F1X0)
#  define HOVER_RCC_ADC                 RCC_ADC
#  define HOVER_RCC_DMA                 RCC_DMA
#  define NVIC_HOVER_TIM1_UP_IRQ        NVIC_TIM1_BRK_UP_TRG_COM_IRQ
#  define NVIC_HOVER_DMA1_CH1_IRQ       NVIC_DMA_CHANNEL1_IRQ
#  define NVIC_HOVER_DMA1_CH4_5_IRQ     NVIC_DMA_CHANNEL4_5_IRQ
#  define NVIC_HOVER_TIM_TIMEOUT_IRQ    NVIC_TIM14_IRQ
#  define HOVER_TIM_TIMEOUT             TIM14
#  define HOVER_RCC_TIM_TIMEOUT         RCC_TIM14
#elif defined(GD32F10X)
#  define HOVER_RCC_ADC                 RCC_ADC1
#  define HOVER_RCC_DMA                 RCC_DMA1
#  define NVIC_HOVER_TIM1_UP_IRQ        NVIC_TIM1_UP_IRQ
#  define NVIC_HOVER_DMA1_CH1_IRQ       NVIC_DMA1_CHANNEL1_IRQ
#  define NVIC_HOVER_DMA1_CH4_5_IRQ     NVIC_DMA1_CHANNEL5_IRQ
   /* F10x has no TIM14 — use TIM3 for the timeout timer (also APB1). */
#  define NVIC_HOVER_TIM_TIMEOUT_IRQ    NVIC_TIM3_IRQ
#  define HOVER_TIM_TIMEOUT             TIM3
#  define HOVER_RCC_TIM_TIMEOUT         RCC_TIM3
#endif

/* ============================================================
 * Interrupt_init — set NVIC priority grouping
 * ============================================================ */
void Interrupt_init(void)
{
    /* PRE4_SUB0 (gd-spl) = 4 bits pre-emption / 0 sub. libopencm3 calls
     * this GROUP16_NOSUB. */
    scb_set_priority_grouping(SCB_AIRCR_PRIGROUP_GROUP16_NOSUB);
}

/* ============================================================
 * Watchdog_init — IWDG ~1.6s timeout
 * Window-mode is dropped (libopencm3 doesn't expose it for this
 * IWDG variant; the hoverboard's reload pattern in the main loop
 * sits well inside any reasonable window).
 * ============================================================ */
ErrStatus Watchdog_init(void)
{
    iwdg_set_period_ms(2048);   /* picks prescale=2 (=/16) + reload=0xFFF */
    iwdg_start();
    return SUCCESS;
}

/* ============================================================
 * TimeoutTimer_init — TIM14 used to detect stale steering commands.
 * Generates an UPDATE interrupt every 2 ms; the ISR resets a counter
 * the comms code checks.
 * ============================================================ */
void TimeoutTimer_init(void)
{
    rcc_periph_clock_enable(HOVER_RCC_TIM_TIMEOUT);
    timer_set_mode(HOVER_TIM_TIMEOUT, TIM_CR1_CKD_CK_INT, TIM_CR1_CMS_EDGE, TIM_CR1_DIR_UP);
    timer_set_prescaler(HOVER_TIM_TIMEOUT, 7200 - 1);    /* 72 MHz / 7200 = 10 kHz tick */
    timer_set_period(HOVER_TIM_TIMEOUT, 20 - 1);         /* 10 kHz / 20 = 500 Hz = 2 ms */
    timer_enable_irq(HOVER_TIM_TIMEOUT, TIM_DIER_UIE);
    nvic_setup_irq(NVIC_HOVER_TIM_TIMEOUT_IRQ, 3);
    timer_enable_counter(HOVER_TIM_TIMEOUT);
}

/* ============================================================
 * GPIO_init — port-clock enables + canonical pin set.
 * The original had ~30 conditional pins; this port handles the
 * always-needed ones (BLDC PWM AF pins, hall inputs, debug LED,
 * VBATT/CURRENT_DC ADC pins). Per-board variants left as TODO.
 * ============================================================ */
void GPIO_init(void)
{
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_GPIOC);
#ifdef GD32F1X0
    rcc_periph_clock_enable(RCC_GPIOF);
#endif

#if defined(GD32F1X0)
    /* F130 (v2 GPIO): split MODER/OTYPER/OSPEEDR/PUPDR/AFRL/AFRH. */

    /* BLDC PWM outputs on PA8/PA9/PA10 (high-side) + PB13/PB14/PB15 (low-side).
     * Alternate function 2 for TIM1_CHx on STM32F0/GD32F1x0. */
    gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO8 | GPIO9 | GPIO10);
    gpio_set_output_options(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_LOW, GPIO8 | GPIO9 | GPIO10);
    gpio_set_af(GPIOA, GPIO_AF2, GPIO8 | GPIO9 | GPIO10);

    gpio_mode_setup(GPIOB, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO13 | GPIO14 | GPIO15);
    gpio_set_output_options(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_LOW, GPIO13 | GPIO14 | GPIO15);
    gpio_set_af(GPIOB, GPIO_AF2, GPIO13 | GPIO14 | GPIO15);

    /* Debug LED on PA5, push-pull, high speed. */
    gpio_mode_setup(DEBUG_LED_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, DEBUG_LED_PIN);
    gpio_set_output_options(DEBUG_LED_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_HIGH, DEBUG_LED_PIN);

    /* Hall inputs on PB10/PB11/PB12 — floating (the hall sensor pulls). */
    gpio_mode_setup(GPIOB, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO10 | GPIO11 | GPIO12);

    /* ADC analog pins: VBATT on PA0, CURRENT_DC on PA1. */
    gpio_mode_setup(GPIOA, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, GPIO0 | GPIO1);

#elif defined(GD32F10X)
    /* F103 (v1 GPIO): combined CRL/CRH MODE+CNF. Same physical pin
     * assignments; different config encoding. */

    /* BLDC PWM outputs (AF push-pull, 50 MHz). On STM32F1/GD32F10x the
     * AF mux is implicit via the timer peripheral being configured —
     * just set the pin to AF push-pull. */
    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL,
                  GPIO8 | GPIO9 | GPIO10);
    gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL,
                  GPIO13 | GPIO14 | GPIO15);

    /* Debug LED on PA5. */
    gpio_set_mode(DEBUG_LED_PORT, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL,
                  DEBUG_LED_PIN);

    /* Hall inputs (floating). */
    gpio_set_mode(GPIOB, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO10 | GPIO11 | GPIO12);

    /* ADC analog pins. */
    gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_ANALOG, GPIO0 | GPIO1);
#endif
}

/* ============================================================
 * PWM_init — TIM1 advanced timer for FOC. Center-aligned, 16 kHz,
 * complementary outputs with dead-time, break input.
 * Empirically validated against gd-spl by regtrace v0.1 TIMER decision.
 * ============================================================ */
void PWM_init(void)
{
    rcc_periph_clock_enable(RCC_TIM1);
    timer_disable_counter(TIM1);

    /* Center-aligned mode 3 (counter counts up and down, OC interrupts
     * fire on both directions), CKD=1, repetition counter = 0. Same
     * register-level state as gd-spl's timer_init with
     * TIMER_COUNTER_CENTER_BOTH. */
    timer_set_mode(TIM1, TIM_CR1_CKD_CK_INT, TIM_CR1_CMS_CENTER_3, TIM_CR1_DIR_UP);
    timer_set_period(TIM1, BLDC_TIMER_PERIOD);
    timer_set_prescaler(TIM1, 0);
    timer_set_repetition_counter(TIM1, 0);
    timer_disable_preload(TIM1);

    /* Configure 3 PWM channels in PWM1 mode with dead-time. */
    for (enum tim_oc_id oc_id = TIM_OC1; oc_id <= TIM_OC3; oc_id++) {
        timer_disable_oc_output(TIM1, oc_id);
        timer_disable_oc_clear(TIM1, oc_id);
        timer_disable_oc_preload(TIM1, oc_id);
        timer_set_oc_slow_mode(TIM1, oc_id);
        timer_set_oc_mode(TIM1, oc_id, TIM_OCM_PWM1);
        timer_set_oc_value(TIM1, oc_id, 0);
        timer_set_oc_polarity_high(TIM1, oc_id);
        timer_set_oc_idle_state_unset(TIM1, oc_id);
    }

    /* Complementary outputs: enable + low polarity + high idle state. */
    timer_set_oc_polarity_low(TIM1, TIM_OC1N);
    timer_set_oc_polarity_low(TIM1, TIM_OC2N);
    timer_set_oc_polarity_low(TIM1, TIM_OC3N);
    timer_set_oc_idle_state_set(TIM1, TIM_OC1N);
    timer_set_oc_idle_state_set(TIM1, TIM_OC2N);
    timer_set_oc_idle_state_set(TIM1, TIM_OC3N);

    /* BDTR: dead-time, run-off-state on, automatic output enable. */
    timer_set_deadtime(TIM1, DEAD_TIME);
    timer_set_enabled_off_state_in_run_mode(TIM1);
    timer_disable_break(TIM1);
    timer_enable_break_automatic_output(TIM1);
    timer_set_break_polarity_low(TIM1);

    /* Enable each output channel + complementary. */
    timer_enable_oc_output(TIM1, TIM_OC1);  timer_enable_oc_output(TIM1, TIM_OC1N);
    timer_enable_oc_output(TIM1, TIM_OC2);  timer_enable_oc_output(TIM1, TIM_OC2N);
    timer_enable_oc_output(TIM1, TIM_OC3);  timer_enable_oc_output(TIM1, TIM_OC3N);

    /* Update-event interrupt for the FOC compute loop. */
    nvic_setup_irq(NVIC_HOVER_TIM1_UP_IRQ, 0);
    timer_enable_irq(TIM1, TIM_DIER_UIE);

    timer_enable_counter(TIM1);
}

/* ============================================================
 * ADC_init — regular conversion of 4 channels into adc_buffer via
 * DMA channel 1. Per regtrace v0.2 ADC decision GD32F1x0 ADC is the
 * v1 layout; the libopencm3 fork's gd32/f1x0/adc.h forwards to
 * stm32/f1/adc.h which is the right driver.
 * ============================================================ */
void ADC_init(void)
{
    rcc_periph_clock_enable(HOVER_RCC_ADC);
    rcc_periph_clock_enable(HOVER_RCC_DMA);

    /* DMA channel 1: peripheral-to-memory, 16-bit, circular, very-high
     * priority. Source = ADC1 DR; dest = adc_buffer. */
    nvic_setup_irq(NVIC_HOVER_DMA1_CH1_IRQ, 1);
    dma_channel_reset(DMA1, DMA_CHANNEL1);
    dma_set_peripheral_address(DMA1, DMA_CHANNEL1, (uint32_t)&ADC_DR(ADC1));
    dma_set_memory_address(DMA1, DMA_CHANNEL1, (uint32_t)adc_buffer);
    dma_set_number_of_data(DMA1, DMA_CHANNEL1, ADC_BUFFER_LEN);
    dma_set_read_from_peripheral(DMA1, DMA_CHANNEL1);
    dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL1);
    dma_disable_peripheral_increment_mode(DMA1, DMA_CHANNEL1);
    dma_set_peripheral_size(DMA1, DMA_CHANNEL1, DMA_CCR_PSIZE_16BIT);
    dma_set_memory_size(DMA1, DMA_CHANNEL1, DMA_CCR_MSIZE_16BIT);
    dma_set_priority(DMA1, DMA_CHANNEL1, DMA_CCR_PL_VERY_HIGH);
    dma_enable_circular_mode(DMA1, DMA_CHANNEL1);
    dma_enable_transfer_complete_interrupt(DMA1, DMA_CHANNEL1);
    dma_enable_channel(DMA1, DMA_CHANNEL1);

    /* ADC config: regular sequence of 4 channels (PA0=ch0, PA1=ch1, PA2,
     * PA3), 13.5 cycle sample time, right-aligned, scan + DMA modes. */
    adc_power_off(ADC1);
    adc_disable_external_trigger_regular(ADC1);
    adc_set_right_aligned(ADC1);
    adc_set_continuous_conversion_mode(ADC1);
    {
        uint8_t channels[ADC_BUFFER_LEN] = {0, 1, 2, 3};
        adc_set_regular_sequence(ADC1, ADC_BUFFER_LEN, channels);
    }
    adc_set_sample_time_on_all_channels(ADC1, ADC_SMPR_SMP_13DOT5CYC);
    adc_disable_analog_watchdog_regular(ADC1);
    adc_enable_scan_mode(ADC1);
    adc_enable_dma(ADC1);
    adc_power_on(ADC1);

    /* Calibrate (must be after enable, per RM). */
    adc_reset_calibration(ADC1);
    adc_calibrate(ADC1);

    adc_start_conversion_regular(ADC1);
}

/* ============================================================
 * USART0_Init — 115200/8N1, TX+RX enabled, RX driven by DMA into
 * usart0_rx_buf for the master/slave comms protocol.
 * ============================================================ */
void USART0_Init(uint32_t iBaud)
{
    rcc_periph_clock_enable(RCC_USART1);
    rcc_periph_clock_enable(HOVER_RCC_DMA);

    /* TX/RX pin AF setup happens in GPIO_init() — for canonical
     * USART1 on STM32F1/GD32F10x: PA9 (TX) + PA10 (RX). On
     * GD32F1x0 (F130) USART0 is also at the USART1 register block
     * but uses PA9/PA10. Both already AF-configured in GPIO_init
     * for TIMER1; if the same pins are needed for USART, they'd
     * need to be remapped — out of scope for this initial port. */

    usart_disable(USART1);
    usart_set_baudrate(USART1, iBaud);
    usart_set_databits(USART1, 8);
    usart_set_stopbits(USART1, USART_STOPBITS_1);
    usart_set_parity(USART1, USART_PARITY_NONE);
    usart_set_mode(USART1, USART_MODE_TX_RX);
    usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);
    usart_enable(USART1);

    /* DMA for USART RX (channel 5 in libopencm3 numbering = "channel 4"
     * in gd-spl numbering — different conventions, same hardware). */
    nvic_setup_irq(NVIC_HOVER_DMA1_CH4_5_IRQ, 2);
    dma_channel_reset(DMA1, DMA_CHANNEL5);
#if defined(GD32F10X)
    /* v1 USART has a single data register USART_DR. */
    dma_set_peripheral_address(DMA1, DMA_CHANNEL5, (uint32_t)&USART_DR(USART1));
#else
    /* v2 USART splits TX/RX data registers; RX is USART_RDR. */
    dma_set_peripheral_address(DMA1, DMA_CHANNEL5, (uint32_t)&USART_RDR(USART1));
#endif
    dma_set_memory_address(DMA1, DMA_CHANNEL5, (uint32_t)usart0_rx_buf);
    dma_set_number_of_data(DMA1, DMA_CHANNEL5, 1);
    dma_set_read_from_peripheral(DMA1, DMA_CHANNEL5);
    dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL5);
    dma_disable_peripheral_increment_mode(DMA1, DMA_CHANNEL5);
    dma_set_peripheral_size(DMA1, DMA_CHANNEL5, DMA_CCR_PSIZE_8BIT);
    dma_set_memory_size(DMA1, DMA_CHANNEL5, DMA_CCR_MSIZE_8BIT);
    dma_set_priority(DMA1, DMA_CHANNEL5, DMA_CCR_PL_VERY_HIGH);
    dma_enable_circular_mode(DMA1, DMA_CHANNEL5);
    dma_enable_transfer_complete_interrupt(DMA1, DMA_CHANNEL5);
    usart_enable_rx_dma(USART1);
    dma_enable_channel(DMA1, DMA_CHANNEL5);
}

/* USART1_Init / USART2_Init are board-variant duplicates of USART0_Init
 * with different pin/channel assignments. Stubbed for now. */
void USART1_Init(uint32_t iBaud) { (void)iBaud; /* TODO: port */ }
void USART2_Init(uint32_t iBaud) { (void)iBaud; /* TODO: port */ }

/* ============================================================
 * pinMode / pinModePull — original helper macros that pack port+pin
 * into a uint32_t. Not load-bearing in the libopencm3 port (callers
 * use the libopencm3 gpio_* APIs directly).
 * ============================================================ */
void pinMode(uint32_t pin, uint32_t mode) { (void)pin; (void)mode; }
void pinModePull(uint32_t pin, uint32_t mode, uint32_t pull) {
    (void)pin; (void)mode; (void)pull;
}

/* ============================================================
 * Clock_init — original sets HXTAL/PLL for 72 MHz from 8 MHz crystal.
 * libopencm3 has rcc_clock_setup_pll for this; the F1x0 stub's
 * RCC variants need a small project-specific scale struct. Stubbed
 * for now since the board's XTAL choice + PLL multiplier is a real
 * config decision, not a mechanical port.
 * ============================================================ */
void Clock_init(void)
{
    /* TODO: port — pick rcc_clock_setup_pll with a struct configured
     * for the board's HXTAL frequency and target SystemCoreClock. */
}
