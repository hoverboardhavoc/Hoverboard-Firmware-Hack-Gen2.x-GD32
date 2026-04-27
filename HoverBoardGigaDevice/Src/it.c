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
#include "../Inc/bldc.h"
#include "../Inc/led.h"
#include "../Inc/commsMasterSlave.h"

/* Phase 2 finalization: ISR rename + HAL inline. The vector table
 * libopencm3 provides expects lowercase `*_isr` symbols (per the brief's
 * Phase 2 ISR rename table); a CMSIS-style `*_IRQHandler` left here
 * would link silently against libopencm3's weak default handler and the
 * IRQ would do nothing at runtime — the linker won't warn. Names
 * verified against ~/dev/c/libopencm3/lib/gd32/f1x0/vector_nvic.c. */
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/gd32/f1x0/timer.h>
#include <libopencm3/gd32/f1x0/dma.h>
#include <libopencm3/gd32/f1x0/adc.h>

//#include "../Inc/commsSteering.h"

//#include "../Inc/commsBluetooth.h"

uint32_t msTicks;
uint32_t timeoutCounter_ms = 0;
FlagStatus timedOut = RESET;

#ifdef SLAVE
uint32_t hornCounter_ms = 0;
#endif

extern int32_t steer;
extern int32_t speed;
extern FlagStatus activateWeakening;
extern FlagStatus beepsBackwards;

//----------------------------------------------------------------------------
// sys_tick_handler — fires every 1 ms (configured by main via SysTick_Config)
//----------------------------------------------------------------------------
void sys_tick_handler(void)
{
	msTicks++;
}

//----------------------------------------------------------------------------
// Resets the timeout to zero
//----------------------------------------------------------------------------
void ResetTimeout(void)
{
  timeoutCounter_ms = 0;
}

//----------------------------------------------------------------------------
// tim14_isr — TIMER13 (= TIM14 lp) update IRQ, fires at 1 kHz (every 1 ms).
// Counts the timeout-since-last-steering-command for emergency-off and the
// horn-2s timeout for the slave LED program.
//----------------------------------------------------------------------------
void tim14_isr(void)
{
	if (timeoutCounter_ms > TIMEOUT_MS)
	{
		// First timeout reset all process values
		if (timedOut == RESET)	// robo: had been RESET = bug ?
		{
#ifdef MASTER
			steer = 0;
			speed = 0;
			beepsBackwards = RESET;
#else
			speed = 0;
#endif
		}
		timedOut = SET;
	}
	else
	{
		timedOut = RESET;
		timeoutCounter_ms++;
	}

#ifdef SLAVE
	if (hornCounter_ms >= 2000)
	{
		// Avoid horn to be activated longer than 2 seconds
		SetUpperLEDMaster(RESET);
	}
	else if (hornCounter_ms < 2000)
	{
		hornCounter_ms++;
	}

	// Update LED program
	CalculateLEDProgram();
#endif

	// Clear update interrupt flag (UIF bit in TIMx_SR).
	timer_clear_flag(TIM14, TIM_SR_UIF);
}

//----------------------------------------------------------------------------
// tim1_brk_up_trg_com_isr — was Timer0_Update_Handler in pre-port code.
// Fires when upcouting of TIM1 (= TIMER0) is finished and the UPDATE-flag
// is set, AND when downcouting is finished and the UPDATE-flag is set
// → PWM of TIM1 running with 16 kHz, with rep counter 1 → interrupt every
// 62.5 µs (once per full PWM period, not every half-period).
//----------------------------------------------------------------------------
extern uint32_t steerCounter;								// Steer counter for setting update rate
uint32_t iPwmTicks = 0, iPwmTicks0 = 0, iPwmCounter = 0, iPwmTime=0, iPwmRate=0;
uint32_t iAdcTicks = 0, iAdcTicks0 = 0, iAdcCounter = 0, iAdcTime=0, iAdcRate=0;
#define COUNT_Irqs 1000

//----------------------------------------------------------------------------
// tim1_brk_up_trg_com_isr — fires on each TIMER0 (= TIM1) update event
// (= every 62.5 µs at 16 kHz with rep counter 1; once per full PWM period).
// Also wired to BRK / TRG / COM events on the same vector slot — those
// don't fire in this configuration.
//----------------------------------------------------------------------------
void tim1_brk_up_trg_com_isr(void)
{
	if (timer_get_flag(TIM1, TIM_SR_UIF))
	{
		// ADC trigger is now hardware-driven via TIM3 TRGO →
		// ADC_CR2_EXTSEL_TIM3_TRGO on the regular group (see
		// adc_trigger_timer_init + adc_init). When phase-current sensing
		// isn't compiled in, the regular group falls back to SWSTART and
		// we fire it here.
		#if !(defined(PHASE_CURRENT_A) && defined(PHASE_CURRENT_B))
			adc_start_conversion_regular(ADC1);
		#endif

		if (msTicks > iPwmTime)
		{
			iPwmTime = msTicks + 1000;
			iPwmRate = iPwmCounter;
			iPwmCounter = 0;
		}
		else iPwmCounter++;

		timer_clear_flag(TIM1, TIM_SR_UIF);
	}
}

//----------------------------------------------------------------------------
// dma_channel1_isr — fires when the ADC scan sequence has DMA'd into the
// adc_buffer (transfer-complete on DMA1 channel 1, which holds the GD CH0
// peripheral request from ADC1). With the FOC trigger pipeline this runs
// at PWM_FREQ (16 kHz), giving CalculateBLDC the freshest current sample.
//----------------------------------------------------------------------------
void dma_channel1_isr(void)
{
	if (dma_get_interrupt_flag(DMA1, DMA_CHANNEL1, DMA_TCIF))
	{
		if (msTicks > iAdcTime)
		{
			iAdcTime = msTicks + 1000;
			iAdcRate = iAdcCounter;
			iAdcCounter = 0;
		}
		else iAdcCounter++;

		CalculateBLDC(); // safe: NVIC blocks re-entrancy on this priority
		dma_clear_interrupt_flags(DMA1, DMA_CHANNEL1, DMA_TCIF);
	}
}


uint32_t iCounterUsart0 = 0;
uint32_t iCounter2Usart0 = 0;

#ifdef HAS_USART0
	// dma_channel2_3_isr — fires when USART0 (= USART1 lp) RX DMA on
	// channel 3 finishes a 1-byte transfer. Circular DMA reloads the
	// count to 1 automatically; the byte sits in usart0_rx_buf[0].
	void dma_channel2_3_isr(void)
	{
		iCounterUsart0++;
		if (dma_get_interrupt_flag(DMA1, DMA_CHANNEL3, DMA_TCIF))
		{
			iCounter2Usart0++;
			#if (REMOTE_USART==0) && defined(MASTER_OR_SINGLE)
					RemoteCallback();
			#elif (MASTERSLAVE_USART==0) && defined(MASTER_OR_SLAVE)
					UpdateUSARTMasterSlaveInput();
			#endif
			dma_clear_interrupt_flags(DMA1, DMA_CHANNEL3, DMA_TCIF);
		}
	}
#endif

uint32_t iCounterUsart1 = 0;
uint32_t iCounter2Usart1 = 0;

#ifdef HAS_USART1
	// dma_channel4_5_isr — fires when USART1 (= USART2 lp) RX DMA on
	// channel 5 finishes a 1-byte transfer. Same shape as
	// dma_channel2_3_isr above, different USART/DMA channel.
	void dma_channel4_5_isr(void)
	{
		iCounterUsart1++;
		if (dma_get_interrupt_flag(DMA1, DMA_CHANNEL5, DMA_TCIF))
		{
			iCounter2Usart1++;
			#if (REMOTE_USART==1) && defined(MASTER_OR_SINGLE)
					RemoteCallback();
			#elif (MASTERSLAVE_USART==1) && defined(MASTER_OR_SLAVE)
					UpdateUSARTMasterSlaveInput();
			#endif
			dma_clear_interrupt_flags(DMA1, DMA_CHANNEL5, DMA_TCIF);
		}
	}
#endif

uint32_t iCounterUsart2 = 0;
uint32_t iCounter2Usart2 = 0;
#if TARGET==2 && defined(HAS_USART2)
	//----------------------------------------------------------------------------
	// This function handles DMA_Channel3_4_IRQHandler interrupt
	// Is asynchronously called when USART_SLAVE RX finished
	//----------------------------------------------------------------------------
	void DMA0_Channel2_IRQHandler(void)		// DMA_Channel3_4_IRQHandler
	{
		iCounterUsart2++;
		//DEBUG_LedSet(	(steerCounter%10) < 5	,0)
		// USART master slave RX
		if (TARGET_dma_interrupt_flag_get(DMA_CH2, DMA_INT_FLAG_FTF))
		{
			iCounter2Usart2++;
			#if (REMOTE_USART==2) && defined(MASTER_OR_SINGLE)
					RemoteCallback();
			#elif (MASTERSLAVE_USART==2) && defined(MASTER_OR_SLAVE)
					UpdateUSARTMasterSlaveInput();
					// Update USART bluetooth input mechanism
					//UpdateUSARTBluetoothInput();
			#endif
			
			TARGET_dma_interrupt_flag_clear(DMA_CH2, DMA_INT_FLAG_FTF);        
		}
	}
#endif


//----------------------------------------------------------------------------
// Returns number of milliseconds since system start
//----------------------------------------------------------------------------
uint32_t millis()
{
	return msTicks;
}

//----------------------------------------------------------------------------
// Delays number of tick Systicks (happens every 10 ms)
//----------------------------------------------------------------------------
void Delay (uint32_t dlyTicks)
{
  uint32_t curTicks;

  curTicks = msTicks;
  while ((msTicks - curTicks) < dlyTicks)
	{
		__NOP();
	}
}

//----------------------------------------------------------------------------
// Cortex-M3 system exception handlers — names match the libopencm3 vector
// table in lib/cm3/vector.c (see #pragma weak ... = blocking_handler chain
// at line 112+). Leaving these named CMSIS-style would link silently
// against libopencm3's weak default and the exception would default-handle
// with no diagnostic.
//----------------------------------------------------------------------------
void nmi_handler(void)
{
}

void hard_fault_handler(void)
{
  while(1) {}
}

void mem_manage_handler(void)
{
  while(1) {}
}

void bus_fault_handler(void)
{
  while(1) {}
}

void usage_fault_handler(void)
{
  while(1) {}
}

void sv_call_handler(void)
{
}

void debug_monitor_handler(void)
{
}

void pend_sv_handler(void)
{
}

void debug_timer_init() {
    DEM_CR |= 0x01000000; // Enable trace
    DWT_CYCCNT = 0;       // Reset cycle counter
    DWT_CONTROL |= 1;     // Enable cycle counter
}
