/*
* This file is part of the hoverboard-firmware-hack-V2 project. The 
* firmware is used to hack the generation 2 board of the hoverboard.
* These new hoverboards have no mainboard anymore. They consist of 
* two Sensorboards which have their own BLDC-Bridge per Motor and an
* ARM Cortex-M3 processor GD32F130C8.
*
* Copyright (C) 2018 Florian Staeblein
* Copyright (C) 2018 Jakob Broemauer
* Copyright (C) 2018 Kai Liebich
* Copyright (C) 2018 Christoph Lehnert
*
* The program is based on the hoverboard project by Niklas Fauth. The 
* structure was tried to be as similar as possible, so that everyone 
* could find a better way through the code.
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "../Inc/defines.h"
#include "../Inc/it.h"

/* Phase 2 stage 1 (clock_init) ports against libopencm3. RCC + SCB are the
 * only libopencm3 surfaces touched at this stage; their function-name
 * namespaces (rcc_*, scb_*) don't collide with SPL (rcu_*) so this can
 * coexist with the still-SPL bodies of the other init functions during
 * the staged port. As subsequent peripherals port, more libopencm3
 * headers are added and the corresponding SPL #include chain is
 * removed file-wide. See decisions.md for the staging rationale. */
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/gd32/f1x0/rcc.h>
#include <libopencm3/gd32/f1x0/gpio.h>
#include <libopencm3/gd32/f1x0/iwdg.h>
#include <libopencm3/gd32/f1x0/usart.h>
#include <libopencm3/gd32/f1x0/dma.h>
#include <libopencm3/gd32/f1x0/nvic.h>
#include <libopencm3/gd32/f1x0/timer.h>
#include <libopencm3/gd32/f1x0/adc.h>

#ifndef pinMode
void pinMode(uint32_t pin, uint32_t mode)
{
	gpio_mode_set(pin&0xffffff00U, mode, GPIO_PUPD_NONE,BIT(pin&0xfU) );
	gpio_output_options_set(pin&0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_10MHZ, BIT(pin&0xfU));
}

void pinModePull(uint32_t pin, uint32_t mode, uint32_t pull)
{
	gpio_mode_set(pin&0xffffff00U, mode, pull,BIT(pin&0xfU) );
	gpio_output_options_set(pin&0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_10MHZ, BIT(pin&0xfU));
}
#endif


#define TIMEOUT_FREQ  1000

// timeout timer parameter structs
timer_parameter_struct timeoutTimer_paramter_struct;

// PWM timer Parameter structs
timer_parameter_struct timerBldc_paramter_struct;	
timer_break_parameter_struct timerBldc_break_parameter_struct;
timer_oc_parameter_struct timerBldc_oc_parameter_struct;

// DMA (USART) structs
dma_parameter_struct dma_init_struct_usart;

//uint8_t usartMasterSlave_rx_buf[USART_MASTERSLAVE_RX_BUFFERSIZE];
//uint8_t usartSteer_COM_rx_buf[USART_STEER_COM_RX_BUFFERSIZE];

uint8_t usart0_rx_buf[1];
uint8_t usart1_rx_buf[1];
uint8_t usart2_rx_buf[1];


// DMA (ADC) structs
dma_parameter_struct dma_init_struct_adc;
extern adc_buf_t adc_buffer;

/* Interrupt_init removed: the only thing it did — set NVIC priority
 * grouping to PRE4_SUB0 — folded into clock_init() per brief Phase 2.
 * Per-peripheral NVIC enables stay where they are (in each peripheral's
 * init function). */

//----------------------------------------------------------------------------
// Initializes the watchdog
//
// Phase 2 stage 3 of the libopencm3 port. iwdg_set_period_ms(2048) lands on
// IWDG_PR=2 (=/16) and IWDG_RLR=0x0FFF — final-state-identical to the SPL
// fwdgt_config(0x0FFF, FWDGT_PSC_DIV16) path. Verified by regtrace vector
// iwdg/config_2sec_period.yaml in final_state mode (gd-spl/gd32f1x0 ↔
// libopencm3/gd32f1x0 → match).
//
// Actual hardware timeout is LSI-frequency dependent: GD32 LSI nominal
// 40 kHz → ~1638 ms; STM32-style nominal 32 kHz → ~2048 ms. The bit-pattern
// in IWDG_PR/IWDG_RLR is identical either way. The window-mode write that
// the SPL Watchdog_init issued (TARGET_fwdgt_window_value_config(0x0FFF))
// programmed IWDG_WINR to its post-reset default of 0x0FFF — equivalent to
// "no window," and libopencm3's iwdg_set_period_ms doesn't write WINR at
// all, leaving the same final state.
//----------------------------------------------------------------------------
ErrStatus watchdog_init(void)
{
	// If the previous reset was caused by the IWDG firing, clear the
	// reset-cause flags. Diagnostic only — the firmware doesn't take a
	// different code path based on reset cause; the original Watchdog_init
	// did this so behavior parity preserved.
	if (RCC_CSR & RCC_CSR_IWDGRSTF) {
		RCC_CSR |= RCC_CSR_RMVF;
	}

	iwdg_set_period_ms(2048);
	iwdg_start();

	return SUCCESS;
}

//----------------------------------------------------------------------------
// timeout_timer_init — TIMER13 (= TIM14 lp) at 1 kHz for the steering /
// command timeout watchdog. Fires every 1 ms via tim14_isr in it.c.
//
// Not in the brief's Phase 2 list but called from main.c → needs to be
// ported for the firmware to function.
//
// TIM14 is a 16-bit general-purpose timer with no center-aligned mode
// (CR1.CMS is reserved on this peripheral per the GD32F1x0 RM). The SPL
// path used CENTER_DOWN with period=SystemCoreClock/2/TIMEOUT_FREQ=36000
// at PSC=0 — center alignment counted up + down, total cycles = 72000,
// half-period = 1 ms, UPIF fires once per period.
//
// Without center mode we use EDGE up-counting with PSC=1 (divide by 2)
// → timer clock = 36 MHz, ARR = 36000-1, UPIF every 36000 cycles =
// every 1 ms. Same effective tick rate, different register encoding.
//----------------------------------------------------------------------------
void timeout_timer_init(void)
{
	rcc_periph_clock_enable(RCC_TIM14);
	rcc_periph_reset_pulse(RST_TIM14);

	timer_set_mode(TIM14, TIM_CR1_CKD_CK_INT, TIM_CR1_CMS_EDGE, TIM_CR1_DIR_UP);
	timer_set_prescaler(TIM14, 1);
	timer_set_period(TIM14, (SystemCoreClock / 2 / TIMEOUT_FREQ) - 1);
	timer_set_repetition_counter(TIM14, 0);
	timer_disable_preload(TIM14);

	// NVIC priority 3: cannot interrupt 0 (BLDC/hall), 1 (ADC), or 2
	// (USART). << 4 because Cortex-M3 implements only the upper 4 bits.
	nvic_set_priority(NVIC_TIM14_IRQ, 3 << 4);
	nvic_enable_irq(NVIC_TIM14_IRQ);
	timer_enable_irq(TIM14, TIM_DIER_UIE);

	timer_enable_counter(TIM14);
}

//----------------------------------------------------------------------------
// Initializes the GPIOs
//
// Phase 2 stage 2 of the libopencm3 port. Every pin config is inlined as
// direct gpio_mode_setup + gpio_set_output_options (+ gpio_set_af) calls
// against libopencm3 — no pinMode/pinModeAF/AF_TIMER0_BLDC helper macros
// (those are SPL-shape shims per brief guardrail #5). The firmware's
// packed pin-code convention `(GPIOx | n)` survives: extract the port via
// `& 0xffffff00U`, extract the bit mask via `1U << (& 0xfU)`. Speed
// mapping: SPL GPIO_OSPEED_2MHZ→LOW, _10MHZ→MED, _50MHZ→HIGH (same
// numeric values, libopencm3 names).
//
// AF map for GD32F130 (per datasheet 2.6.7): TIMER0 channels are all
// GPIO_AF2; TIMER0 BRKIN on PA6 or PB12 is GPIO_AF2 too. USART0 on PB6/PB7
// is GPIO_AF0; on PA2/PA3/PA9/PA10/PA14/PA15 is GPIO_AF1. USART1 on
// PA8/PB0 is GPIO_AF4; on PA2/PA3/PA14/PA15 is GPIO_AF1.
//----------------------------------------------------------------------------
void gpio_init(void)
{
	// Enable all GPIO clocks (libopencm3 RCC_GPIOx ↔ SPL RCU_GPIOx).
	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_GPIOB);
	rcc_periph_clock_enable(RCC_GPIOC);
	rcc_periph_clock_enable(RCC_GPIOF);

	#ifdef TIMER_BLDC_EMERGENCY_SHUTDOWN
		// Emergency shutdown pin → TIMER0 BRKIN, AF2.
		gpio_mode_setup(TIMER_BLDC_EMERGENCY_SHUTDOWN & 0xffffff00U,
				GPIO_MODE_AF, GPIO_PUPD_NONE,
				1U << (TIMER_BLDC_EMERGENCY_SHUTDOWN & 0xfU));
		gpio_set_output_options(TIMER_BLDC_EMERGENCY_SHUTDOWN & 0xffffff00U,
				GPIO_OTYPE_PP, GPIO_OSPEED_HIGH,
				1U << (TIMER_BLDC_EMERGENCY_SHUTDOWN & 0xfU));
		gpio_set_af(TIMER_BLDC_EMERGENCY_SHUTDOWN & 0xffffff00U,
				GPIO_AF2,
				1U << (TIMER_BLDC_EMERGENCY_SHUTDOWN & 0xfU));
	#endif

	// PWM output pins — TIMER0 channels CH0/CH0N/CH1/CH1N/CH2/CH2N (all AF2),
	// 2 MHz output speed (suppresses ringing into the gate driver),
	// pull configured per board (TIMER_BLDC_PULLUP from active layout).
	gpio_mode_setup(BLDC_GH & 0xffffff00U, GPIO_MODE_AF, TIMER_BLDC_PULLUP, 1U << (BLDC_GH & 0xfU));
	gpio_set_output_options(BLDC_GH & 0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_LOW, 1U << (BLDC_GH & 0xfU));
	gpio_set_af(BLDC_GH & 0xffffff00U, GPIO_AF2, 1U << (BLDC_GH & 0xfU));

	gpio_mode_setup(BLDC_GL & 0xffffff00U, GPIO_MODE_AF, TIMER_BLDC_PULLUP, 1U << (BLDC_GL & 0xfU));
	gpio_set_output_options(BLDC_GL & 0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_LOW, 1U << (BLDC_GL & 0xfU));
	gpio_set_af(BLDC_GL & 0xffffff00U, GPIO_AF2, 1U << (BLDC_GL & 0xfU));

	gpio_mode_setup(BLDC_BH & 0xffffff00U, GPIO_MODE_AF, TIMER_BLDC_PULLUP, 1U << (BLDC_BH & 0xfU));
	gpio_set_output_options(BLDC_BH & 0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_LOW, 1U << (BLDC_BH & 0xfU));
	gpio_set_af(BLDC_BH & 0xffffff00U, GPIO_AF2, 1U << (BLDC_BH & 0xfU));

	gpio_mode_setup(BLDC_BL & 0xffffff00U, GPIO_MODE_AF, TIMER_BLDC_PULLUP, 1U << (BLDC_BL & 0xfU));
	gpio_set_output_options(BLDC_BL & 0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_LOW, 1U << (BLDC_BL & 0xfU));
	gpio_set_af(BLDC_BL & 0xffffff00U, GPIO_AF2, 1U << (BLDC_BL & 0xfU));

	gpio_mode_setup(BLDC_YH & 0xffffff00U, GPIO_MODE_AF, TIMER_BLDC_PULLUP, 1U << (BLDC_YH & 0xfU));
	gpio_set_output_options(BLDC_YH & 0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_LOW, 1U << (BLDC_YH & 0xfU));
	gpio_set_af(BLDC_YH & 0xffffff00U, GPIO_AF2, 1U << (BLDC_YH & 0xfU));

	gpio_mode_setup(BLDC_YL & 0xffffff00U, GPIO_MODE_AF, TIMER_BLDC_PULLUP, 1U << (BLDC_YL & 0xfU));
	gpio_set_output_options(BLDC_YL & 0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_LOW, 1U << (BLDC_YL & 0xfU));
	gpio_set_af(BLDC_YL & 0xffffff00U, GPIO_AF2, 1U << (BLDC_YL & 0xfU));

	#ifndef REMOTE_AUTODETECT

		#ifdef DEBUG_LED_PIN
			gpio_mode_setup(DEBUG_LED_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, DEBUG_LED_PIN);
			gpio_set_output_options(DEBUG_LED_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_MED, DEBUG_LED_PIN);
		#endif

		#ifdef LED_GREEN
			gpio_mode_setup(LED_GREEN & 0xffffff00U, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, 1U << (LED_GREEN & 0xfU));
			gpio_set_output_options(LED_GREEN & 0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_MED, 1U << (LED_GREEN & 0xfU));
		#endif
		#ifdef LED_RED
			gpio_mode_setup(LED_RED & 0xffffff00U, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, 1U << (LED_RED & 0xfU));
			gpio_set_output_options(LED_RED & 0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_MED, 1U << (LED_RED & 0xfU));
		#endif
		#ifdef LED_ORANGE
			gpio_mode_setup(LED_ORANGE & 0xffffff00U, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, 1U << (LED_ORANGE & 0xfU));
			gpio_set_output_options(LED_ORANGE & 0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_MED, 1U << (LED_ORANGE & 0xfU));
		#endif
		#ifdef UPPER_LED
			gpio_mode_setup(UPPER_LED & 0xffffff00U, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, 1U << (UPPER_LED & 0xfU));
			gpio_set_output_options(UPPER_LED & 0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_MED, 1U << (UPPER_LED & 0xfU));
		#endif
		#ifdef LOWER_LED
			gpio_mode_setup(LOWER_LED & 0xffffff00U, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, 1U << (LOWER_LED & 0xfU));
			gpio_set_output_options(LOWER_LED & 0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_MED, 1U << (LOWER_LED & 0xfU));
		#endif
		#ifdef MOSFET_OUT
			gpio_mode_setup(MOSFET_OUT & 0xffffff00U, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, 1U << (MOSFET_OUT & 0xfU));
			gpio_set_output_options(MOSFET_OUT & 0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_MED, 1U << (MOSFET_OUT & 0xfU));
		#endif

		// Hall sensor inputs — floating, debounced via the hall-width learner
		// in bldcFOC. No pull required because the hoverboard hall PCB has
		// open-drain comparators with on-board pull-ups already.
		gpio_mode_setup(HALL_A & 0xffffff00U, GPIO_MODE_INPUT, GPIO_PUPD_NONE, 1U << (HALL_A & 0xfU));
		gpio_mode_setup(HALL_B & 0xffffff00U, GPIO_MODE_INPUT, GPIO_PUPD_NONE, 1U << (HALL_B & 0xfU));
		gpio_mode_setup(HALL_C & 0xffffff00U, GPIO_MODE_INPUT, GPIO_PUPD_NONE, 1U << (HALL_C & 0xfU));

		// ADC analog inputs — analog mode disconnects the digital input
		// (Schmitt trigger off, no glitch energy on the supply).
		#ifdef VBATT
			gpio_mode_setup(VBATT & 0xffffff00U, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, 1U << (VBATT & 0xfU));
		#endif
		#ifdef CURRENT_DC
			gpio_mode_setup(CURRENT_DC & 0xffffff00U, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, 1U << (CURRENT_DC & 0xfU));
		#endif
		#if defined(PHASE_CURRENT_A) && defined(PHASE_CURRENT_B)
			gpio_mode_setup(PHASE_CURRENT_A & 0xffffff00U, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, 1U << (PHASE_CURRENT_A & 0xfU));
			gpio_mode_setup(PHASE_CURRENT_B & 0xffffff00U, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, 1U << (PHASE_CURRENT_B & 0xfU));
		#endif
		#ifdef REMOTE_ADC
			gpio_mode_setup(PA2 & 0xffffff00U, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, 1U << (PA2 & 0xfU));
			gpio_mode_setup(PA3 & 0xffffff00U, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, 1U << (PA3 & 0xfU));
		#endif

		#ifdef SELF_HOLD
			gpio_mode_setup(SELF_HOLD & 0xffffff00U, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, 1U << (SELF_HOLD & 0xfU));
			gpio_set_output_options(SELF_HOLD & 0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_MED, 1U << (SELF_HOLD & 0xfU));
		#endif

		#ifdef BUZZER
			// 50 MHz output speed for the buzzer — the carrier needs sharp
			// edges so the audible note isn't muddied by output-stage rolloff.
			gpio_mode_setup(BUZZER & 0xffffff00U, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, 1U << (BUZZER & 0xfU));
			gpio_set_output_options(BUZZER & 0xffffff00U, GPIO_OTYPE_PP, GPIO_OSPEED_HIGH, 1U << (BUZZER & 0xfU));
		#endif

		#ifdef MASTER_OR_SINGLE
			#ifdef BUTTON_PU
				// Button with internal pull-up. Reads HIGH when not pressed,
				// LOW when pressed (open switch to GND).
				gpio_mode_setup(BUTTON_PU & 0xffffff00U, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, 1U << (BUTTON_PU & 0xfU));
			#elif defined(BUTTON)
				gpio_mode_setup(BUTTON & 0xffffff00U, GPIO_MODE_INPUT, GPIO_PUPD_NONE, 1U << (BUTTON & 0xfU));
			#endif

			#if defined(CHARGE_STATE) && defined(MASTER_OR_SINGLE)
				gpio_mode_setup(CHARGE_STATE & 0xffffff00U, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, 1U << (CHARGE_STATE & 0xfU));
			#endif
		#endif

		#ifdef PHOTO_L
			gpio_mode_setup(PHOTO_L & 0xffffff00U, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, 1U << (PHOTO_L & 0xfU));
		#endif
		#ifdef PHOTO_R
			gpio_mode_setup(PHOTO_R & 0xffffff00U, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, 1U << (PHOTO_R & 0xfU));
		#endif

	#endif // #ifndef REMOTE_AUTODETECT
}


//volatile uint8_t hall = 0;        // Global hall state
//volatile uint32_t last_edge = 0;  // Timestamp of last edge (e.g., SysTick count)





/*
//----------------------------------------------------------------------------
// Initializes the PWM
//----------------------------------------------------------------------------
void PWM_initOld(void)
{
	// Enable timer clock
	rcu_periph_clock_enable(RCU_TIMER_BLDC);
	
	// Initial deinitialize of the timer
	timer_deinit(TIMER_BLDC);
	
	// Set up the basic parameter struct for the timer
	timerBldc_paramter_struct.counterdirection = TIMER_COUNTER_UP;
	timerBldc_paramter_struct.prescaler = 0;
	timerBldc_paramter_struct.alignedmode = TIMER_COUNTER_CENTER_BOTH;	//changed from TIMER_COUNTER_CENTER_DOWN by deepseek for SVM;
	timerBldc_paramter_struct.period = BLDC_TIMER_PERIOD;
	timerBldc_paramter_struct.clockdivision = TIMER_CKDIV_DIV1;

	
	timerBldc_paramter_struct.repetitioncounter = 0;
	timer_auto_reload_shadow_disable(TIMER_BLDC);
	
	// Initialize timer with basic parameter struct
	timer_init(TIMER_BLDC, &timerBldc_paramter_struct);

	// Deactivate output channel fastmode
	timer_channel_output_fast_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_G, TIMER_OC_FAST_DISABLE);
	timer_channel_output_fast_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_B, TIMER_OC_FAST_DISABLE);
	timer_channel_output_fast_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_Y, TIMER_OC_FAST_DISABLE);
	
	// Deactivate output channel shadow function
	timer_channel_output_shadow_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_G, TIMER_OC_SHADOW_DISABLE);
	timer_channel_output_shadow_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_B, TIMER_OC_SHADOW_DISABLE);
	timer_channel_output_shadow_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_Y, TIMER_OC_SHADOW_DISABLE);
	
	// Set output channel PWM type to PWM1

	// CH0COMCTL[2:0]
	// 110: PWM mode0.
	// When counting up, OxCPRE is high when the counter is smaller than TIMER0_CHxCV, and low otherwise.
	// When counting down, OxCPRE is low when the counter is larger than TIMER0_CHxCV, and high otherwise.
	// 111: PWM mode1.
	// When counting up, OxCPRE is low when the counter is smaller than TIMER0_CHxCV, and high otherwise.
	// When counting down, OxCPRE is high when the counter is larger than TIMER0_CHxCV, and low otherwise.
	timer_channel_output_mode_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_G, TIMER_OC_MODE_PWM1);
	timer_channel_output_mode_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_B, TIMER_OC_MODE_PWM1);
	timer_channel_output_mode_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_Y, TIMER_OC_MODE_PWM1);

	// Initialize pulse length with value 0 (pulse duty factor = zero)
	timer_channel_output_pulse_value_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_G, 0);
	timer_channel_output_pulse_value_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_B, 0);
	timer_channel_output_pulse_value_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_Y, 0);
	
	// Set up the output channel parameter struct
	timerBldc_oc_parameter_struct.ocpolarity 		= TIMER_OC_POLARITY_HIGH; //HIGH: CHx_O is the same as OxCPRE , LOW: CHx_O is contrary to OxCPRE
	timerBldc_oc_parameter_struct.ocnpolarity 	= TIMER_OCN_POLARITY_LOW; //HIGH: CHx_ON is contrary to OxCPRE, LOW: CHx_O is the same as OxCPRE
	timerBldc_oc_parameter_struct.ocidlestate 	= TIMER_OC_IDLE_STATE_LOW;
	timerBldc_oc_parameter_struct.ocnidlestate 	= TIMER_OCN_IDLE_STATE_HIGH;
	
	// Configure all three output channels with the output channel parameter struct
	timer_channel_output_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_G, &timerBldc_oc_parameter_struct);
  timer_channel_output_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_B, &timerBldc_oc_parameter_struct);
	timer_channel_output_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_Y, &timerBldc_oc_parameter_struct);

	// Set up the break parameter struct
	timerBldc_break_parameter_struct.runoffstate			= TIMER_ROS_STATE_ENABLE;
	timerBldc_break_parameter_struct.ideloffstate 		= TIMER_IOS_STATE_DISABLE;
	timerBldc_break_parameter_struct.protectmode			= TIMER_CCHP_PROT_OFF;
	timerBldc_break_parameter_struct.outputautostate 	= TIMER_OUTAUTO_ENABLE;
	timerBldc_break_parameter_struct.breakpolarity		= TIMER_BREAK_POLARITY_LOW;

	//timerBldc_break_parameter_struct.deadtime 				= DEAD_TIME;
	//timerBldc_break_parameter_struct.breakstate				= TIMER_BREAK_DISABLE;		// Gen2.2 HarleyBob used TIMER_BREAK_DISABLE instead of TIMER_BREAK_ENABLE
	//deepseek: Add dead time configuration (critical for SVM):
	#ifdef BLDC_SINEx
		timerBldc_break_parameter_struct.deadtime = 0;  // No dead time needed for SVM   ; robo: really ?? deadtime is to prevent short cut through highside mosfet and lowside mosfet being on at the same time
	#else
		timerBldc_break_parameter_struct.deadtime 				= DEAD_TIME;
	#endif
	timerBldc_break_parameter_struct.breakstate = TIMER_BREAK_DISABLE;

	
	
	// Configure the timer with the break parameter struct
	timer_break_config(TIMER_BLDC, &timerBldc_break_parameter_struct);

	// Disable until all channels are set for PWM output
	timer_disable(TIMER_BLDC);

	// Enable all three channels for PWM output
	timer_channel_output_state_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_G, TIMER_CCX_ENABLE);
	timer_channel_output_state_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_B, TIMER_CCX_ENABLE);
	timer_channel_output_state_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_Y, TIMER_CCX_ENABLE);

	// Enable all three complemenary channels for PWM output
	timer_channel_complementary_output_state_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_G, TIMER_CCXN_ENABLE);
	timer_channel_complementary_output_state_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_B, TIMER_CCXN_ENABLE);
	timer_channel_complementary_output_state_config(TIMER_BLDC, TIMER_BLDC_CHANNEL_Y, TIMER_CCXN_ENABLE);
	
	// Enable TIMER_INT_UP interrupt and set priority
	TARGET_nvic_irq_enable(TIMER0_BRK_UP_TRG_COM_IRQn, 0, 0);		// can interrupt everything, but wait for hall-irq to finish
	timer_interrupt_enable(TIMER_BLDC, TIMER_INT_UP);
	
	// Enable the timer and start PWM
	timer_enable(TIMER_BLDC);
}
*/

//----------------------------------------------------------------------------
// 3-phase PWM init (TIMER0 = libopencm3 TIM1, advanced timer)
//
// Phase 2 stage 5a of the libopencm3 port. All SPL timer_*_config /
// timer_break_config / TARGET_nvic_irq_enable shims inlined as direct
// libopencm3 calls.
//
// TIMER0 channels — vendor naming → board naming → libopencm3:
//   CH0 / CH0N → BLDC_BLUE  → TIM_OC1 / TIM_OC1N
//   CH1 / CH1N → BLDC_BLUE? → TIM_OC2 / TIM_OC2N  (board uses CH_1 = blue, CH_2 = green; see TIMER_BLDC_CHANNEL_*)
//   CH2 / CH2N → BLDC_GREEN → TIM_OC3 / TIM_OC3N
// Active layout (defines_2-1-20.h via defines.h:98-100):
//   TIMER_BLDC_CHANNEL_G = TIMER_CH_2 (= TIM_OC3 / TIM_OC3N)
//   TIMER_BLDC_CHANNEL_B = TIMER_CH_1 (= TIM_OC2 / TIM_OC2N)
//   TIMER_BLDC_CHANNEL_Y = TIMER_CH_0 (= TIM_OC1 / TIM_OC1N)
//
// Center-aligned mode CMS=11 ("center-aligned mode 3" — UPIF on both
// overflow and underflow); rep counter = 1 makes UPIF fire once per full
// period (every other half-period). Per GD32F1x0 User Manual Rev3.6
// §15.1.4. The post-init UPG software event re-locks update polarity.
//
// Master mode = UPDATE → TRGO fires on each UEV; consumed by TIMER2
// (= TIM3) in adc_trigger_timer_init via the ITI0 input trigger path.
//
// Vector coverage: regtrace `vectors/timer/pwm_init_center_aligned_16khz.yaml`
// covers the basic timer-init shape (mode + period + prescaler + repetition
// + UPG). The output-channel + break-config + per-channel state writes
// in this function are not regtrace-covered yet — final state matches the
// SPL pattern register-by-register and is well-documented in the GD32
// SPL/libopencm3 mapping (see decisions.md for the per-call mapping
// table).
//----------------------------------------------------------------------------
void pwm_init(void)
{
	// TIMER0 (= TIM1) clock and full reset.
	rcc_periph_clock_enable(RCC_TIM1);
	rcc_periph_reset_pulse(RST_TIM1);

	// Center-aligned PWM at PWM_FREQ. CMS=CENTER_3 fires UPIF at both
	// overflow AND underflow; rep_counter=1 then halves UPIF rate so it
	// lands once per full period (replaces the SPL software-toggle hack
	// the firmware used to need in it.c). CKD=DIV1 (no clock division).
	timer_set_mode(TIM1, TIM_CR1_CKD_CK_INT, TIM_CR1_CMS_CENTER_3, TIM_CR1_DIR_UP);
	timer_set_period(TIM1, BLDC_TIMER_PERIOD);
	timer_set_prescaler(TIM1, 0);
	timer_set_repetition_counter(TIM1, 1);
	timer_disable_preload(TIM1);  // ARPE = 0 — auto-reload shadow off.

	// Force a software UPG after writing odd CREP so update events are
	// guaranteed to land on underflow (GD32F1x0 TRM §15.1.4: "If an update
	// event is generated by software after writing an odd number to CREP,
	// the update events will be generated on the underflow.").
	timer_generate_event(TIM1, TIM_EGR_UG);

	// TRGO = update event. Drives TIMER2 in adc_trigger_timer_init via
	// ITI0; the ADC regular group's external trigger ETSRC selects
	// T2_TRGO. F130 can't route TIM1's TRGO directly to ADC ETSRC
	// (which only offers T0_CH0/CH1/CH2 = moving PWM outputs), so
	// TIMER2 acts as a fixed-offset bridge.
	timer_set_master_mode(TIM1, TIM_CR2_MMS_UPDATE);

	// Per-channel config for OC1/OC2/OC3 (the vendor's TIMER_CH_0/1/2).
	// PWM mode 1: counting up → OxCPRE LOW when CNT<CCR, HIGH otherwise;
	// counting down → OxCPRE HIGH when CNT>CCR, LOW otherwise.
	// Polarity HIGH on CHx (= OxCPRE not inverted), LOW on CHxN (= OxCPRE
	// inverted on the complementary pin). Idle state: CHx = LOW, CHxN =
	// HIGH (matches the gate-driver convention with high-side conducting
	// when the controller asserts HIGH and low-side conducting when the
	// controller asserts LOW).
	enum tim_oc_id ocs[3]   = { TIM_OC1, TIM_OC2, TIM_OC3 };
	enum tim_oc_id ocns[3]  = { TIM_OC1N, TIM_OC2N, TIM_OC3N };
	for (int i = 0; i < 3; i++) {
		timer_set_oc_slow_mode(TIM1, ocs[i]);          // OCxFE = 0 (fast off)
		timer_disable_oc_preload(TIM1, ocs[i]);        // OCxPE = 0 (shadow off)
		timer_set_oc_mode(TIM1, ocs[i], TIM_OCM_PWM1);
		timer_set_oc_value(TIM1, ocs[i], 0);           // start at duty=0
		timer_set_oc_polarity_high(TIM1, ocs[i]);
		timer_set_oc_polarity_low(TIM1, ocns[i]);
		timer_set_oc_idle_state_unset(TIM1, ocs[i]);   // OISx = 0 → idle LOW
		timer_set_oc_idle_state_set(TIM1, ocns[i]);    // OISxN = 1 → idle HIGH
	}

	// Break / dead-time config (BDTR):
	//   OSSR = 1 (run-mode-off-state ENABLE — drives outputs to OISx/OISxN
	//             values when CCxE/CCxNE = 0)
	//   OSSI = 0 (idle-mode-off-state DISABLE — disconnects outputs)
	//   LOCK = 00 (no register-level write protection)
	//   AOE  = 1 (automatic-output ENABLE — MOE auto-sets on next UEV)
	//   BKP  = 0 (break input polarity LOW)
	//   BKE  = 0 (break input DISABLE — Gen2.2 HarleyBob convention; the
	//             gate-driver fault line is wired but not always present
	//             so leaving it disabled prevents spurious shutdowns)
	//   DTG  = DEAD_TIME (e.g. 32 → ~444 ns at 72 MHz; clipped at 0xFF)
	timer_set_enabled_off_state_in_run_mode(TIM1);
	timer_set_break_lock(TIM1, TIM_BDTR_LOCK_OFF);
	timer_enable_break_automatic_output(TIM1);
	timer_set_break_polarity_low(TIM1);
	timer_disable_break(TIM1);
	timer_set_deadtime(TIM1, DEAD_TIME);

	// Disable until all channels enabled, then enable all 6 outputs
	// (CH0..CH2 + CH0N..CH2N). With BDTR.AOE=1, MOE will auto-assert
	// on the next UEV after timer_enable_counter.
	timer_disable_counter(TIM1);
	for (int i = 0; i < 3; i++) {
		timer_enable_oc_output(TIM1, ocs[i]);
		timer_enable_oc_output(TIM1, ocns[i]);
	}

	// NVIC: TIM1_BRK_UP_TRG_COM is the IRQ that fires on UPIF (and also
	// on BRK / TRG / COM events, which we don't use). Pre-empt priority
	// 0 — the highest, peer with the hall IRQ; the firmware relies on
	// the hall handler completing before this one runs (same priority,
	// non-nested).
	nvic_set_priority(NVIC_TIM1_BRK_UP_TRG_COM_IRQ, 0);
	nvic_enable_irq(NVIC_TIM1_BRK_UP_TRG_COM_IRQ);
	timer_enable_irq(TIM1, TIM_DIER_UIE);

	// Start PWM. UEV sets MOE; outputs go live the same cycle.
	timer_enable_counter(TIM1);
}

#if defined(PHASE_CURRENT_A) && defined(PHASE_CURRENT_B)
//----------------------------------------------------------------------------
// ADC trigger timer (TIMER2) — hardware pipeline:
//
//     TIMER0 UPIF (valley) ──TRGO──> TIMER2 reset (slave restart mode)
//                                         │
//                                         │ counts up from 0 at TIMER_CK
//                                         │
//                                         ▼
//                              TIMER2 CH0 compare at FOC_SAMPLE_OFFSET_TICKS
//                                         │
//                                         │ fires O0CPRE → TRGO
//                                         │
//                                         ▼
//                     ADC regular group (ETSRC = T2_TRGO) starts scan
//
// Net effect: ADC conversion begins FOC_SAMPLE_OFFSET_TICKS counts after
// each PWM valley, with sub-µs jitter determined only by silicon path
// delay. No CPU in the loop. Adjust FOC_SAMPLE_OFFSET_TICKS to dodge
// dead-time edges at extreme duty cycles.
//
// TIMER2 ITI0 = TIMER0_TRGO per TRM §15 "Slave mode example table".
//----------------------------------------------------------------------------
#ifndef FOC_SAMPLE_OFFSET_TICKS
	#define FOC_SAMPLE_OFFSET_TICKS 10   // ~140 ns at 72 MHz — essentially at the valley
#endif

//----------------------------------------------------------------------------
// adc_trigger_timer_init — TIMER2 (= libopencm3 TIM3) as a fixed-offset
// slave to TIMER0 (= TIM1)'s TRGO. Phase 2 stage 5b.
//
// Hardware pipeline (no CPU in the loop after init):
//
//   TIM1 UPIF (PWM valley) ──TRGO──> TIM3 reset (slave restart mode)
//                                       │
//                                       │ counts up from 0 at 72 MHz
//                                       ▼
//                            TIM3 CH1 compare at FOC_SAMPLE_OFFSET_TICKS
//                                       │
//                                       │ rising edge of OC1REF → TRGO
//                                       ▼
//                  ADC regular group (ETSRC = T2_TRGO) starts scan
//
// libopencm3 channel naming offset by 1 from GD: TIMER_CH_0 (GD) = TIM_OC1
// (libopencm3). Same hardware, different vendor numbering.
//----------------------------------------------------------------------------
void adc_trigger_timer_init(void)
{
	rcc_periph_clock_enable(RCC_TIM3);
	rcc_periph_reset_pulse(RST_TIM3);

	// Plain up-counter at 72 MHz, period 0xFFFF (never overflows within a
	// 62.5 µs PWM half-period). CKD=DIV1, EDGE alignment, rep=0.
	timer_set_mode(TIM3, TIM_CR1_CKD_CK_INT, TIM_CR1_CMS_EDGE, TIM_CR1_DIR_UP);
	timer_set_prescaler(TIM3, 0);
	timer_set_period(TIM3, 0xFFFF);
	timer_set_repetition_counter(TIM3, 0);

	// Slave mode: reset (= restart) on every ITI0 rising edge. ITI0 on
	// TIM3 maps to TIM1 TRGO per F1x0 reference manual §15 "Slave mode
	// example table". libopencm3's TIM_SMCR_SMS_RM = "reset mode" =
	// SPL's TIMER_SLAVE_MODE_RESTART (counter clears to 0 on trigger).
	timer_slave_set_trigger(TIM3, TIM_SMCR_TS_ITR0);
	timer_slave_set_mode(TIM3, TIM_SMCR_SMS_RM);

	// CH1 (= GD CH_0) output compare in PWM mode 1, value =
	// FOC_SAMPLE_OFFSET_TICKS (10 ticks ≈ 140 ns past the valley). PWM1
	// raises OC1REF at compare match, which feeds the master-mode TRGO.
	// Output pin disabled — we only need the internal OC1REF for the
	// trigger; nothing routed to the package.
	timer_disable_oc_output(TIM3, TIM_OC1);
	timer_set_oc_mode(TIM3, TIM_OC1, TIM_OCM_PWM1);
	timer_set_oc_value(TIM3, TIM_OC1, FOC_SAMPLE_OFFSET_TICKS);

	// TRGO source = OC1REF (MMS = COMPARE_OC1REF = 0b100). Maps to SPL's
	// TIMER_TRI_OUT_SRC_O0CPRE.
	timer_set_master_mode(TIM3, TIM_CR2_MMS_COMPARE_OC1REF);

	// CEN=1: counter runs, then gets reset by TIM1 TRGO each cycle.
	// (Slave reset mode resets the counter but doesn't toggle CEN.)
	timer_enable_counter(TIM3);
}
#endif

/*
//----------------------------------------------------------------------------
// Initializes the ADC
//----------------------------------------------------------------------------
void ADC_initOld(void)
{
	// Enable ADC and DMA clock
	rcu_periph_clock_enable(RCU_ADC);
	rcu_periph_clock_enable(RCU_DMA);
	
  // Configure ADC clock (APB2 clock is DIV1 -> 72MHz, ADC clock is DIV6 -> 12MHz)
	rcu_adc_clock_config(RCU_ADCCK_APB2_DIV6);
	
	// Interrupt channel 0 enable
	TARGET_nvic_irq_enable(DMA_Channel0_IRQn, 1, 0);
	
	// Initialize DMA channel 0 for ADC
	TARGET_dma_deinit(DMA_CH0);
	
	uint16_t iCountAdc = sizeof(adc_buffer)/2;	// array of uint16_t
	//iCountAdc = 1;
	
	dma_init_struct_adc.direction = DMA_PERIPHERAL_TO_MEMORY;
	dma_init_struct_adc.memory_addr = (uint32_t)&adc_buffer;
	dma_init_struct_adc.memory_inc = DMA_MEMORY_INCREASE_ENABLE;
	dma_init_struct_adc.memory_width = DMA_MEMORY_WIDTH_16BIT;
	dma_init_struct_adc.number = iCountAdc;
	
	dma_init_struct_adc.periph_addr = (uint32_t)&TARGET_ADC_RDATA;
	dma_init_struct_adc.periph_inc = DMA_PERIPH_INCREASE_DISABLE;
	dma_init_struct_adc.periph_width = DMA_PERIPHERAL_WIDTH_16BIT;
	dma_init_struct_adc.priority = DMA_PRIORITY_ULTRA_HIGH;
	TARGET_dma_init(DMA_CH0, &dma_init_struct_adc);
	
	// Configure DMA mode
	TARGET_dma_circulation_enable(DMA_CH0);
	TARGET_dma_memory_to_memory_disable(DMA_CH0);
	
	// Enable DMA transfer complete interrupt
	TARGET_dma_interrupt_enable(DMA_CH0, DMA_CHXCTL_FTFIE);
	
	// At least clear number of remaining data to be transferred by the DMA 
	TARGET_dma_transfer_number_config(DMA_CH0, iCountAdc);		// 2
	
	// Enable DMA channel 0
	TARGET_dma_channel_enable(DMA_CH0);
	
	
	#ifdef REMOTE_AUTODETECT
		adc_channel_length_config(ADC_REGULAR_CHANNEL, 1);
		adc_regular_channel_config(0, PIN_TO_CHANNEL(TODO_PIN), ADC_SAMPLETIME_13POINT5);
			// for some reason, the adc channel 1 used for VBat (3.3V) has to be set to TODO_PIN = PF4
	#else
		TARGET_adc_channel_length_config(ADC_REGULAR_CHANNEL, iCountAdc);	// 2
		#ifdef VBATT
			TARGET_adc_regular_channel_config(0, PIN_TO_CHANNEL(VBATT), ADC_SAMPLETIME_13POINT5);
		#endif
		#ifdef CURRENT_DC
			TARGET_adc_regular_channel_config(1, PIN_TO_CHANNEL(CURRENT_DC), ADC_SAMPLETIME_13POINT5);
		#endif
		#ifdef REMOTE_ADC
			adc_regular_channel_config(2, PIN_TO_CHANNEL(PA2), ADC_SAMPLETIME_13POINT5);
			adc_regular_channel_config(3, PIN_TO_CHANNEL(PA3), ADC_SAMPLETIME_13POINT5);
		#endif
	#endif
	
	TARGET_adc_data_alignment_config(ADC_DATAALIGN_RIGHT);
	
	// Set trigger of ADC
	TARGET_adc_external_trigger_config(ADC_REGULAR_CHANNEL, ENABLE);
	TARGET_adc_external_trigger_source_config(ADC_REGULAR_CHANNEL, ADC_EXTTRIG_REGULAR_NONE);

	// Disable the temperature sensor, Vrefint and vbat channel
	adc_tempsensor_vrefint_disable();
	#ifndef REMOTE_AUTODETECT
		TARGET_adc_vbat_disable();
	#endif
	
	// ADC analog watchdog disable
	TARGET_adc_watchdog_disable();
	
	// Enable ADC (must be before calibration)
	TARGET_adc_enable();
	
	// Calibrate ADC values
	TARGET_adc_calibration_enable();
	
	// Enable DMA request
	TARGET_adc_dma_mode_enable();
    
	// Set ADC to scan mode
	TARGET_adc_special_function_config(ADC_SCAN_MODE, ENABLE);
}
*/

//----------------------------------------------------------------------------
// adc_init — ADC0 (= libopencm3 ADC1) regular group with DMA + external
// trigger from TIM3 TRGO. Phase 2 stage 5c.
//
// GD32F1x0 ADC is the STM32F1-style v1 layout (RSQ1/2/3 sequence, SAMPT0/1
// per-channel sample times, two-step RSTCLB+CLB calibration). The fork's
// gd32/f1x0/adc.h forwards directly to stm32/f1/adc.h. Per regtrace
// decisions/v0.2/ADC.md the layouts are bit-compatible for the operations
// this firmware uses.
//
// Pin → channel decoding inherits from PIN_TO_CHANNEL() in target.h:
//   GPIOA pin n → channel n        (PA0..PA7 = ch 0..7)
//   GPIOB pin n → channel n + 8    (PB0..PB1 = ch 8..9)
// (GD32F130 only has 10 ADC channels on the 48-pin package.)
//
// Trigger sequence is two-step on purpose: ETSRC=SWSTART during init →
// calibrate → ETSRC=TIM3_TRGO afterwards. Some F130 silicon hangs the
// RSTCLB/CLB sequence if ETERC=1 + a non-SW ETSRC is set before
// calibration completes; this preserves the SPL workaround.
//
// EXTSEL bit values: STM32F1 ADC1 (bit positions per stm32/f1/adc.h):
//   ADC_CR2_EXTSEL_TIM3_TRGO = 0x4 << 17 — same value as GD's
//   ADC_EXTTRIG_REGULAR_T2_TRGO (T2 in GD = TIM3 in STM32 numbering).
//   ADC_CR2_EXTSEL_SWSTART   = 0x7 << 17 — software trigger; conversion
//   started by writing SWSTART bit.
//----------------------------------------------------------------------------
void adc_init(void)
{
	rcc_periph_clock_enable(RCC_ADC1);
	rcc_periph_clock_enable(RCC_DMA);

	// ADC clock = APB2 / 6 = 72 MHz / 6 = 12 MHz. Above 14 MHz is out of
	// spec on this part; /6 leaves some headroom.
	rcc_set_adcpre(RCC_CFGR_ADCPRE_DIV6);

	// NVIC: DMA1 channel 1 (= GD DMA_CH0) for the ADC scan-complete IRQ.
	// Pre-empt priority 1 — can interrupt priorities 2+ (timeout, USART)
	// but not 0 (BLDC/hall) per the firmware's pre-empt hierarchy.
	nvic_set_priority(NVIC_DMA_CHANNEL1_IRQ, 1 << 4);
	nvic_enable_irq(NVIC_DMA_CHANNEL1_IRQ);

	uint16_t adc_count = sizeof(adc_buffer) / 2;  // adc_buffer is uint16_t[]

	// DMA channel 1 (= GD CH0): peripheral-to-memory, 16-bit transfers,
	// circular, transfer-complete IRQ. Reads from ADC_DR (data register
	// alias for ADC1's converted-data register).
	dma_channel_reset(DMA1, DMA_CHANNEL1);
	dma_set_peripheral_address(DMA1, DMA_CHANNEL1, (uint32_t)&ADC_DR(ADC1));
	dma_set_memory_address(DMA1, DMA_CHANNEL1, (uint32_t)&adc_buffer);
	dma_set_number_of_data(DMA1, DMA_CHANNEL1, adc_count);
	dma_set_read_from_peripheral(DMA1, DMA_CHANNEL1);
	dma_disable_peripheral_increment_mode(DMA1, DMA_CHANNEL1);
	dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL1);
	dma_set_peripheral_size(DMA1, DMA_CHANNEL1, DMA_CCR_PSIZE_16BIT);
	dma_set_memory_size(DMA1, DMA_CHANNEL1, DMA_CCR_MSIZE_16BIT);
	dma_set_priority(DMA1, DMA_CHANNEL1, DMA_CCR_PL_VERY_HIGH);
	dma_enable_circular_mode(DMA1, DMA_CHANNEL1);
	dma_enable_transfer_complete_interrupt(DMA1, DMA_CHANNEL1);
	dma_enable_channel(DMA1, DMA_CHANNEL1);

	// Build the regular channel sequence in DMA-fill order so the
	// adc_buf_t field layout matches. Phase currents first (sampled at
	// the PWM valley), then slower channels.
	uint8_t channels[8];
	uint8_t length = 0;

	#ifdef REMOTE_AUTODETECT
		channels[length++] = PIN_TO_CHANNEL(TODO_PIN);
	#else
		#if defined(PHASE_CURRENT_A) && defined(PHASE_CURRENT_B)
			channels[length++] = PIN_TO_CHANNEL(PHASE_CURRENT_A);
			channels[length++] = PIN_TO_CHANNEL(PHASE_CURRENT_B);
		#endif
		#ifdef VBATT
			channels[length++] = PIN_TO_CHANNEL(VBATT);
		#endif
		#ifdef CURRENT_DC
			channels[length++] = PIN_TO_CHANNEL(CURRENT_DC);
		#endif
		#ifdef REMOTE_ADC
			channels[length++] = PIN_TO_CHANNEL(PA2);
			channels[length++] = PIN_TO_CHANNEL(PA3);
		#endif
	#endif

	adc_set_regular_sequence(ADC1, length, channels);
	adc_set_sample_time_on_all_channels(ADC1, ADC_SMPR_SMP_13DOT5CYC);

	adc_set_right_aligned(ADC1);

	// Step 1 of the trigger sequence: SWSTART. ETERC must be 1 for any
	// external trigger (incl. SW) to work. Real source set after calib.
	adc_enable_external_trigger_regular(ADC1, ADC_CR2_EXTSEL_SWSTART);

	adc_disable_temperature_sensor();
	#ifndef REMOTE_AUTODETECT
		adc_disable_temperature_sensor();  // SPL "vbat_disable" is an alias —
		                                   // disabling Vrefint/temp sensor
		                                   // also disables Vbat divider.
	#endif
	adc_disable_analog_watchdog_regular(ADC1);

	// Power on, then calibrate. adc_calibrate() handles the RSTCLB+CLB
	// two-step + waits for completion.
	adc_power_on(ADC1);
	adc_calibrate(ADC1);

	// Step 2 of the trigger sequence: post-calibration, switch ETSRC to
	// TIM3_TRGO so the ADC fires hardware-driven from the PWM-valley
	// pipeline set up in pwm_init + adc_trigger_timer_init.
	#if defined(PHASE_CURRENT_A) && defined(PHASE_CURRENT_B)
		adc_enable_external_trigger_regular(ADC1, ADC_CR2_EXTSEL_TIM3_TRGO);
	#endif

	adc_enable_dma(ADC1);
	adc_enable_scan_mode(ADC1);
}


//----------------------------------------------------------------------------
// USART0 (= libopencm3 USART1) init
//
// Phase 2 stage 4 of the libopencm3 port. Inlined per brief guardrail #5 —
// no AF_USART0_TX / TARGET_DMA_* / TARGET_nvic_irq_enable shims left.
//
// USART0 (GD vendor name) ↔ USART1 (libopencm3/STM32 name); same APB2[14]
// peripheral. DMA channel mapping: GD DMA_CH2 (USART0 RX) ↔ libopencm3
// DMA1_CHANNEL3 (numbering shifted by 1 — GD numbers from 0, libopencm3
// from 1). The shared DMA controller sits at DMA1_BASE with seven channels;
// channel 3 is the USART1_RX peripheral mapping per the F1x0 reference
// manual table.
//
// AF map (GD32F130 datasheet 2.6.7): USART0 on PB6/PB7 = AF0; on
// PA2/PA3/PA9/PA10/PA14/PA15 = AF1. The active layout's USART0_TX/RX
// constants pick one of those pin pairs.
//
// USART config validated by regtrace vector usart/init_115200_8n1.yaml
// (`final_state` mode, `gd-spl/gd32f1x0` ↔ `libopencm3/gd32f1x0` →
// 1 difference: an explicit CR3=0 write that gd-spl skips). Decided-
// acceptable per `~/dev/regtrace/decisions/v0.2/USART.md` — the final
// CR3 state is 0 in both, libopencm3 just writes it explicitly via
// usart_set_flow_control(NONE).
//----------------------------------------------------------------------------
void usart0_init(uint32_t iBaud)
{
#ifdef HAS_USART0

	// Pull config: open-drain on a multi-drop UART bus (REMOTE_UARTBUS)
	// because external pull-ups are provided by the bus master; pull-up
	// otherwise so the line idles HIGH between bytes.
	#if REMOTE_USART==0 && defined(REMOTE_UARTBUS)
		#define USART0_PUPD GPIO_PUPD_NONE
	#else
		#define USART0_PUPD GPIO_PUPD_PULLUP
	#endif

	// USART0 TX pin: AF mode, push-pull, 50 MHz, AF0 if PB6 else AF1.
	gpio_mode_setup(USART0_TX & 0xffffff00U, GPIO_MODE_AF, USART0_PUPD,
			1U << (USART0_TX & 0xfU));
	gpio_set_output_options(USART0_TX & 0xffffff00U, GPIO_OTYPE_PP,
			GPIO_OSPEED_HIGH, 1U << (USART0_TX & 0xfU));
	gpio_set_af(USART0_TX & 0xffffff00U,
			(USART0_TX == PB6) ? GPIO_AF0 : GPIO_AF1,
			1U << (USART0_TX & 0xfU));

	// USART0 RX pin: AF mode, AF0 if PB7 else AF1.
	gpio_mode_setup(USART0_RX & 0xffffff00U, GPIO_MODE_AF, USART0_PUPD,
			1U << (USART0_RX & 0xfU));
	gpio_set_output_options(USART0_RX & 0xffffff00U, GPIO_OTYPE_PP,
			GPIO_OSPEED_HIGH, 1U << (USART0_RX & 0xfU));
	gpio_set_af(USART0_RX & 0xffffff00U,
			(USART0_RX == PB7) ? GPIO_AF0 : GPIO_AF1,
			1U << (USART0_RX & 0xfU));

	// Peripheral clocks — USART0 (= USART1 lp = APB2[14]) and DMA1 (= AHB[0]).
	rcc_periph_clock_enable(RCC_USART1);
	rcc_periph_clock_enable(RCC_DMA);

	// USART config — 8N1, no flow control, TX+RX. 16x oversampling left at
	// post-reset default (CR1.OVER8 = 0). Baud divisor uses
	// rcc_apb2_frequency, which clock_init's rcc_clock_setup_pll(HSI_72MHZ)
	// already set to 72_000_000.
	usart_disable(USART1);
	usart_set_baudrate(USART1, iBaud);
	usart_set_databits(USART1, 8);
	usart_set_stopbits(USART1, USART_STOPBITS_1);
	usart_set_parity(USART1, USART_PARITY_NONE);
	usart_set_mode(USART1, USART_MODE_TX_RX);
	usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);
	usart_enable(USART1);

	// NVIC: pre-emption priority 2 (the SPL nvic_irq_enable(IRQn, 2, 0)
	// argument shape, given clock_init's PRIGROUP_NOSUB grouping uses all
	// 4 implemented bits as pre-emption). Cortex-M3 implements only the
	// upper 4 bits of the 8-bit priority byte → write priority=2<<4=0x20.
	// Logical priority 2 cannot interrupt priority 0 (BLDC/hall) or 1
	// (ADC/CalculateBldc), per the firmware's pre-empt hierarchy.
	nvic_set_priority(NVIC_DMA_CHANNEL2_3_IRQ, 2 << 4);
	nvic_enable_irq(NVIC_DMA_CHANNEL2_3_IRQ);

	// DMA channel 3 (GD CH2) for USART1 RX: peripheral-to-memory, 8-bit
	// transfers, single-byte circular. The transfer-complete interrupt
	// fires every byte → DMA_Channel1_2_IRQHandler in it.c → RemoteCallback
	// or UpdateUSARTMasterSlaveInput.
	dma_channel_reset(DMA1, DMA_CHANNEL3);
	dma_set_peripheral_address(DMA1, DMA_CHANNEL3, (uint32_t)&USART_RDR(USART1));
	dma_set_memory_address(DMA1, DMA_CHANNEL3, (uint32_t)usart0_rx_buf);
	dma_set_number_of_data(DMA1, DMA_CHANNEL3, 1);
	dma_set_read_from_peripheral(DMA1, DMA_CHANNEL3);
	dma_disable_peripheral_increment_mode(DMA1, DMA_CHANNEL3);
	dma_enable_memory_increment_mode(DMA1, DMA_CHANNEL3);
	dma_set_peripheral_size(DMA1, DMA_CHANNEL3, DMA_CCR_PSIZE_8BIT);
	dma_set_memory_size(DMA1, DMA_CHANNEL3, DMA_CCR_MSIZE_8BIT);
	dma_set_priority(DMA1, DMA_CHANNEL3, DMA_CCR_PL_VERY_HIGH);
	dma_enable_circular_mode(DMA1, DMA_CHANNEL3);

	usart_enable_rx_dma(USART1);
	dma_enable_transfer_complete_interrupt(DMA1, DMA_CHANNEL3);
	dma_enable_channel(DMA1, DMA_CHANNEL3);

#endif
}


void USART1_Init(uint32_t iBaud)
{
#ifdef HAS_USART1

	#if TARGET == 2
		//rcu_periph_clock_enable(RCU_AF);        // Alternate Function clock
		//gpio_pin_remap_config(GPIO_USART0_REMAP, ENABLE); // JW: Remap USART0 to PB6 and PB7
	
		#if REMOTE_USART==1 && defined(REMOTE_UARTBUS)	// no pullup resistors with multiple boards on the UartBus - Esp32/Arduino (Serial.begin) have to setup pullups
			#define USART1_PUPD	GPIO_MODE_AF_OD
		#else
			#define USART1_PUPD	GPIO_MODE_AF_PP
		#endif
		pinModeSpeed(USART1_TX, USART1_PUPD, GPIO_OSPEED_50MHZ);	// // GD32F130: GPIO_AF_1 = USART
		pinModeSpeed(USART1_RX, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ);	
	#else
		#if REMOTE_USART==1 && defined(REMOTE_UARTBUS)	// no pullup resistors with multiple boards on the UartBus - Esp32/Arduino (Serial.begin) have to setup pullups
			#define USART1_PUPD	GPIO_PUPD_NONE
		#else
			#define USART1_PUPD	GPIO_PUPD_PULLUP
		#endif
		pinModeAF(USART1_TX, AF_USART1_TX, USART1_PUPD, GPIO_OSPEED_50MHZ);	// // GD32F130: GPIO_AF_1 = USART
		pinModeAF(USART1_RX, AF_USART1_RX, USART1_PUPD, GPIO_OSPEED_50MHZ);	
	#endif
	//gpio_mode_set(USART1_TX_PORT , GPIO_MODE_AF, GPIO_PUPD_PULLUP, USART1_TX_PIN);	
	//gpio_mode_set(USART1_RX_PORT , GPIO_MODE_AF, GPIO_PUPD_PULLUP, USART1_RX_PIN);
	//gpio_output_options_set(USART1_TX_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_50MHZ, USART1_TX_PIN);
	//gpio_output_options_set(USART1_RX_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_50MHZ, USART1_RX_PIN);	
	//gpio_af_set(USART1_TX_PORT, GPIO_AF_1, USART1_TX_PIN);	// GD32F130: GPIO_AF_1 = USART
	//gpio_af_set(USART1_RX_PORT, GPIO_AF_1, USART1_RX_PIN);
	
	
	// Enable ADC and DMA clock
	rcu_periph_clock_enable(RCU_USART1);
	rcu_periph_clock_enable(RCU_DMA);
	
	// Init USART for 115200 baud, 8N1
	usart_baudrate_set(USART1, iBaud);
	usart_parity_config(USART1, USART_PM_NONE);
	usart_word_length_set(USART1, USART_WL_8BIT);
	usart_stop_bit_set(USART1, USART_STB_1BIT);
	#if TARGET == 2	// robo: 2 NOT_NEEDED
		usart_hardware_flow_rts_config(USART1, USART_RTS_DISABLE);  // JW: Disable RTS
		usart_hardware_flow_cts_config(USART1, USART_CTS_DISABLE);  // JW: Disable CTS
	#else
		TARGET_usart_oversample_config(USART1, USART_OVSMOD_16);
	#endif
	
	// Enable both transmitter and receiver
	usart_transmit_config(USART1, USART_TRANSMIT_ENABLE);
	usart_receive_config(USART1, USART_RECEIVE_ENABLE);
	
	//syscfg_dma_remap_enable(SYSCFG_DMA_REMAP_USART0RX|SYSCFG_DMA_REMAP_USART0TX);

	// Enable USART
	usart_enable(USART1);
	
	// Interrupt channel 3/4 enable
	TARGET_nvic_irq_enable(TARGET_DMA_Channel3_4_IRQn, 2, 0);		// usart irqs can not interrupt 0=bldc/hall or 1=adc/CalculateBldc
	
	// Initialize DMA channel 4 for USART_SLAVE RX
	TARGET_dma_deinit(TARGET_DMA_CH4);
	dma_init_struct_usart.direction = DMA_PERIPHERAL_TO_MEMORY;
	dma_init_struct_usart.memory_addr = (uint32_t)usart1_rx_buf;
	dma_init_struct_usart.memory_inc = DMA_MEMORY_INCREASE_ENABLE;
	dma_init_struct_usart.memory_width = DMA_MEMORY_WIDTH_8BIT;
	dma_init_struct_usart.number = 1;
	dma_init_struct_usart.periph_addr = USART1_DATA_RX_ADDRESS;
	dma_init_struct_usart.periph_inc = DMA_PERIPH_INCREASE_DISABLE;
	dma_init_struct_usart.periph_width = DMA_PERIPHERAL_WIDTH_8BIT;
	dma_init_struct_usart.priority = DMA_PRIORITY_ULTRA_HIGH;
	TARGET_dma_init(TARGET_DMA_CH4, &dma_init_struct_usart);
	
	// Configure DMA mode
	TARGET_dma_circulation_enable(TARGET_DMA_CH4);
	TARGET_dma_memory_to_memory_disable(TARGET_DMA_CH4);

	// USART DMA enable for transmission and receive
	usart_dma_receive_config(USART1, USART_DENR_ENABLE);
	
	// Enable DMA transfer complete interrupt
	TARGET_dma_interrupt_enable(TARGET_DMA_CH4, DMA_CHXCTL_FTFIE);
	
	// At least clear number of remaining data to be transferred by the DMA 
	TARGET_dma_transfer_number_config(TARGET_DMA_CH4, 1);
	
	// Enable dma receive channel
	TARGET_dma_channel_enable(TARGET_DMA_CH4);
#endif
}

void USART2_Init(uint32_t iBaud)	// only for target==2 = gd32f103
{
#if defined(HAS_USART2) && TARGET==2

	//JMA enable RCU_AF for alternate functions
	rcu_periph_clock_enable(RCU_AF);

	#if REMOTE_USART==2 && defined(REMOTE_UARTBUS)	// no pullup resistors with multiple boards on the UartBus - Esp32/Arduino (Serial.begin) have to setup pullups
		#define USART2_PUPD	GPIO_MODE_AF_OD
	#else
		#define USART2_PUPD	GPIO_MODE_AF_PP
	#endif
	// JW: Configure USART2 TX (PB10) and RX (PB11) pins
	//gpio_init(USART_MASTERSLAVE_TX_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, USART_MASTERSLAVE_TX_PIN); // JW:
	//gpio_init(USART_MASTERSLAVE_RX_PORT, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, USART_MASTERSLAVE_RX_PIN); // JW:
	pinModeSpeed(USART2_TX, USART2_PUPD, GPIO_OSPEED_50MHZ);
	pinModeSpeed(USART2_RX, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ);	

		// Enable ADC and DMA clock
	rcu_periph_clock_enable(RCU_USART2); // JW: was RCU_USART1
	rcu_periph_clock_enable(RCU_DMA0); //JMA was RCU_DMA
	
	// Reset USART
	usart_deinit(USART2); // JW: added
	
	// Init USART for 115200 baud, 8N1
	usart_baudrate_set(USART2, iBaud);
	usart_parity_config(USART2, USART_PM_NONE);
	usart_word_length_set(USART2, USART_WL_8BIT);
	usart_stop_bit_set(USART2, USART_STB_1BIT);
	usart_hardware_flow_rts_config(USART2, USART_RTS_DISABLE);  // JW: Disable RTS
	usart_hardware_flow_cts_config(USART2, USART_CTS_DISABLE);  // JW: Disable CTS
	//JMA no oversampling in F103 usart_oversample_config(USART2, USART_OVSMOD_16);
	
	// Enable both transmitter and receiver
	usart_transmit_config(USART2, USART_TRANSMIT_ENABLE);
	usart_receive_config(USART2, USART_RECEIVE_ENABLE);
	
	// Enable USART
	usart_enable(USART2);
	
	// Interrupt channel 3/4 enable
	// usart irqs set to Pre-priority 2 can not interrupt 0=bldc/hall or 1=adc/CalculateBldc
	//nvic_irq_enable(DMA_Channel3_4_IRQn, 2, 0);
	//JMA F103 cannel 3 and 4 are separate. Only channel 4 is used so only channel 4 interrupt enabled
	nvic_irq_enable(DMA0_Channel2_IRQn, 2, 0); // JW: Changed to Channel2 (from Channel4)

// Initialize DMA channel 4 for USART_SLAVE RX
	dma_deinit(DMA0, DMA_CH2); // JW: Changed to CH2 (from CH4). JMA DMA0 added
	dma_init_struct_usart.direction = DMA_PERIPHERAL_TO_MEMORY;
	dma_init_struct_usart.memory_addr = (uint32_t)usart2_rx_buf;
	dma_init_struct_usart.memory_inc = DMA_MEMORY_INCREASE_ENABLE;
	dma_init_struct_usart.memory_width = DMA_MEMORY_WIDTH_8BIT;
	dma_init_struct_usart.number = 1;
	dma_init_struct_usart.periph_addr = (uint32_t)&USART_DATA(USART2); // JW: USART_MASTERSLAVE_DATA_RX_ADDRESS;
	dma_init_struct_usart.periph_inc = DMA_PERIPH_INCREASE_DISABLE;
	dma_init_struct_usart.periph_width = DMA_PERIPHERAL_WIDTH_8BIT;
	dma_init_struct_usart.priority = DMA_PRIORITY_ULTRA_HIGH;
	dma_init(DMA0, DMA_CH2, &dma_init_struct_usart); // JW: Changed to CH2 (from CH4). JMA DMA0 added & added before dma_init_struct_usart
	
	// Configure DMA mode
	dma_circulation_enable(DMA0, DMA_CH2); // JW: Changed to CH2 (from CH4). JMA DMA0 added
	dma_memory_to_memory_disable(DMA0, DMA_CH2); // JW: Changed to CH2 (from CH4). JMA DMA0 added

	// USART DMA enable for transmission and receive
	usart_dma_receive_config(USART2, USART_DENR_ENABLE);
	
	// Enable DMA transfer complete interrupt
	dma_interrupt_enable(DMA0, DMA_CH2, DMA_CHXCTL_FTFIE); // JW: Changed to CH2 (from CH4). JMA DMA0 added
	
	// At least clear number of remaining data to be transferred by the DMA 
	dma_transfer_number_config(DMA0, DMA_CH2, 1); // JW: Changed to CH2 (from CH4). JMA DMA0 added
	
	// Enable dma receive channel
	dma_channel_enable(DMA0, DMA_CH2); // JW: Changed to CH2 (from CH4). JMA DMA0 added
#endif
}





# define TRUE												0x01
# define FALSE												0x00

static uint32_t get_flash_size(void) 	// Helper functions for flash configuration by Deepseek
{
	return (*(volatile uint16_t *)(0x1FFFF7E0)) * 1024; // Flash size in bytes
}
static uint32_t get_page_size(uint32_t flash_size)
{
	return (flash_size <= 65536) ? 1024 : 2048; // JW: 1KB page for <=64KB, 2KB for >64KB
}

void flashErase(uint32_t address) // Clears a page of microprocessor memory. JW: Requires page erase before write (bits can only be changed from 1 to 0).
{
	fmc_unlock();
	fmc_flag_clear(FMC_FLAG_END | FMC_FLAG_WPERR);
	fmc_page_erase(address);
	fmc_lock();
}
uint32_t flashRead(uint32_t address) // Reads 4 bytes from microprocessor memory
{
	return *(uint32_t*)address;
}
uint8_t flashWrite(uint32_t address, uint32_t data)	// Writes 4 bytes to microprocessor memory
{
	uint8_t fflash = FALSE;
	fmc_unlock();
	fmc_flag_clear(FMC_FLAG_END | FMC_FLAG_WPERR);

	

	#if TARGET == 2
		if (fmc_halfword_program(address, (uint16_t) data)== FMC_READY) 	// JW: STM32F103 can only write 16 bits at a time.
		{ 
			if (fmc_halfword_program((address+2), (uint16_t) (data>>16))== FMC_READY) fflash = TRUE;
		}
	#else
		if (fmc_word_program(address, data) == FMC_READY) fflash = TRUE; 
	#endif
	fmc_lock();
	return fflash;
}
void flashWriteBuffer(uint32_t address, uint8_t *pbuffer, uint16_t len) 	// Write buffer (word-aligned) by Deepseek
{
	for (uint16_t i = 0; i < len; i += 4)
	{
		uint32_t val = *((uint32_t*)(pbuffer + i));
		flashWrite(address + i, val);
	}
}
void flashReadBuffer(uint32_t address, uint8_t *pbuffer, uint16_t len) 	// Read buffer (word-aligned) by Deepseek
{
	for (uint16_t i = 0; i < len; i += 4)
	{
			*((uint32_t*)(pbuffer + i)) = flashRead(address + i);
	}
}


//#define STATE_InverterOn  1
ConfigData oConfig;

void ConfigReset(void) 
{
	oConfig.iVersion = EEPROM_VERSION;
	oConfig.wState = 0;
	int8_t i=0;
	#ifdef REMOTE_AUTODETECT
		for(;i<PINS_DETECT;i++)	oConfig.aiPinScan[i] = -1;	// -1 = not set
	#else	
		oConfig.iSpeedNeutral = 2048;
		oConfig.iSteerNeutral = 2048;
		oConfig.iSpeedMax = 4096;
		oConfig.iSpeedMin = 0;
		oConfig.iSteerMax = 4096;
		oConfig.iSteerMin = 0;
	#endif
	for(i=0;i<sizeof(oConfig.padding);i++)	oConfig.padding[i]=0;	// Clear padding
}

void ConfigWrite(void) 	// made compatible for 32kB and 64kB mcu versions by Deepseek
{
	uint32_t flash_size = get_flash_size();
	uint32_t page_size = get_page_size(flash_size);
	uint32_t last_page_start = 0x08000000 + flash_size - page_size;

	flashErase(last_page_start);
	flashWriteBuffer(last_page_start, (uint8_t*)&oConfig, sizeof(oConfig));
}

void ConfigRead(void)  	// made compatible for 32kB and 64kB mcu versions by Deepseek
{
	uint32_t flash_size = get_flash_size();
	uint32_t page_size = get_page_size(flash_size);
	uint32_t last_page_start = 0x08000000 + flash_size - page_size;

	if (flashRead(last_page_start) == 0xFFFFFFFF)
	{
		ConfigReset();
		ConfigWrite();
		return;
	}

	flashReadBuffer(last_page_start, (uint8_t*)&oConfig, sizeof(oConfig));

	if (oConfig.iVersion != EEPROM_VERSION)
	{
		ConfigReset();
		ConfigWrite();
	}
}

uint32_t dev_id = 0;		// for debugging with StmStudio (or McuViewer)
uint32_t pll_mul = 0;		// for debugging with StmStudio (or McuViewer)
void clock_init(void)
{
	/* Diagnostic: identify the silicon. GD32F130C8 reads back 0x410 in the
	 * low 12 bits of DBGMCU_IDCODE (matches STM32F103 — vendor obfuscation).
	 * Surfaced for StmStudio / McuViewer; not a control flow input. */
	#define DBGMCU_IDCODE   (*(volatile uint32_t*)0xE0042000)
	#define DEV_ID_MASK     0x00000FFF
	dev_id = DBGMCU_IDCODE & DEV_ID_MASK;

	/* PLL + bus prescalers + sysclk switch in one call. Equivalent to the
	 * GD32 SPL's __SYSTEM_CLOCK_72M_PLL_IRC8M_DIV2 path that ran from
	 * SystemInit() before main on the SPL build. The HSI_72MHZ entry in
	 * rcc_hsi_configs[] uses pllmul=MUL18 (PLLMF[4]+PLLMF[0]) — the wider
	 * GD-only multiplier that STM32F1's 4-bit PLLMUL can't reach. See
	 * vector rcc/irc8m_pll_72mhz.yaml + decisions/v0.5+/RCC.md. */
	rcc_clock_setup_pll(&rcc_hsi_configs[RCC_CLOCK_HSI_72MHZ]);

	/* Folded from the deleted Interrupt_init: 4-bit pre-empt, no
	 * sub-priority. PRIGROUP_NOSUB == SCB_AIRCR_PRIGROUP_NOSUB ==
	 * gd-spl's NVIC_PRIGROUP_PRE4_SUB0. Runs before any peripheral's
	 * own NVIC enable so each subsequent nvic_enable_irq sees the right
	 * grouping. */
	scb_set_priority_grouping(SCB_AIRCR_PRIGROUP_NOSUB);

	/* Diagnostic: PLLMF[4:0]. After rcc_clock_setup_pll(...HSI_72MHZ) this
	 * should read 0x11 (PLLMF[4]=1, PLLMF[3:0]=1 → MUL18). Cross-check
	 * value in McuViewer to confirm the wide PLLMUL field landed. */
	pll_mul = ((RCC_CFGR & RCC_CFGR_PLLMUL_0_3) >> RCC_CFGR_PLLMUL_0_3_SHIFT) |
	          (((RCC_CFGR & RCC_CFGR_PLLMUL_4) >> RCC_CFGR_PLLMUL_4_SHIFT) << 4);
}

uint32_t iTestClock = 0;	// for debugging with StmStudio (or McuViewer)
void Clock_test(void)
{
	uint32_t iTimeStart = millis();
	for (volatile uint32_t i = 72000000; i > 0; i--);	// 72000000 NOP operations would take 1000 ms at 72 MHz ?
	iTestClock = millis()-iTimeStart;
}


#ifdef WINDOWS_RN
void add_cr_before_lf_inplace(char* str, uint16_t buffer_size) 
{
	if (str == NULL || buffer_size == 0)
			return;

	// 1. Calculate original length and count newlines in a single pass.
	size_t original_len = 0;
	int newline_count = 0;
	for (original_len = 0; str[original_len] != '\0'; original_len++) {
			if (str[original_len] == '\n') {
					newline_count++;
			}
	}

	size_t new_len = original_len + newline_count;
	// 2. Check if the new string (including null terminator) will fit.
	if (new_len + 1 > buffer_size) {
			//fprintf(stderr, "Error: Not enough buffer space for in-place modification.\n");
			return;
	}
	// 3. Work backwards from the end of the string to insert '\r'.
	// Use signed types for indices to safely decrement to -1.
	int16_t read_idx = original_len - 1;
	int16_t write_idx = new_len - 1;

	// Place the new null terminator first.
	str[new_len] = '\0';

	while (read_idx >= 0) 
	{
		if (str[read_idx] == '\n') 
		{
			str[write_idx--] = '\n';
			str[write_idx--] = '\r';

		}
		else 
		{
			str[write_idx--] = str[read_idx];
		}
		read_idx--;
	}
}
#endif
