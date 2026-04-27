# Hoverboard libopencm3 port — workflow + status

This is the **actual** port of the hoverboard firmware to libopencm3
(replacing the GigaDevice SPL it uses today). Targets: GD32F103 (F10x
family) and GD32F130 (F1x0 family).

For toolchain validation only (the LED-blink demo) see
`../libopencm3-toolchain-test/`.

## Status

The port is **in progress**. Track per-file:

| file | status | notes |
|---|---|---|
| `Src/setup.c` | partial | `Interrupt_init`, `Watchdog_init` ported; `GPIO_init` ported for the LED pin; `PWM_init`/`ADC_init`/`USART_*_init`/`TimeoutTimer_init` are stubs |
| `Src/main.c` | not started | depends on setup.c being done |
| `Src/it.c` | not started | interrupt handlers; `usart1_isr` etc. naming |
| `Src/bldc*.c` | not started | timing-critical, port LAST |
| `Src/comms.c` | not started | depends on USART being ported |
| `Src/imuMPU6050.c` / `bmi160.c` | not started | needs I2C |
| `Src/RemoteAdc.c` | not started | needs ADC |

`Src/setup.c` compiles cleanly for both targets:
```
make TARGET=gd32f1x0   # GD32F130: clean
make TARGET=gd32f10x   # GD32F103: clean (NVIC warning is a libopencm3-stub TODO, not a porting bug)
```

## How to extend the port

The build uses **per-file compile** (no link yet) since most files
aren't ported. Add a file to `SRCS` in the Makefile to start porting it.

For each function in setup.c marked STUB:
1. Read the original in `../HoverBoardGigaDevice/Src/setup.c`.
2. Look up each gd-spl call in the translation table below.
3. Rewrite using libopencm3 — keep the function signature.
4. `make TARGET=gd32f1x0` to build; iterate.

When all of setup.c is ported, switch to per-file compile of `it.c`,
`comms.c`, etc., adding stubs for any helper functions called in setup.c
that aren't yet themselves ported.

Once a coherent set links (probably setup.c + it.c + comms.c + main.c),
swap the Makefile's per-file compile for a real link step targeting an
ELF binary.

## gd-spl → libopencm3 translation table

| gd-spl                                        | libopencm3                                               |
|---|---|
| `nvic_priority_group_set(NVIC_PRIGROUP_PRE4_SUB0)` | `scb_set_priority_grouping(SCB_AIRCR_PRIGROUP_GROUP16_NOSUB)` |
| `nvic_irq_enable(IRQn, prio, sub)` (SPL macro) | `nvic_enable_irq(IRQn) + nvic_set_priority(IRQn, prio<<4)` |
| `rcu_periph_clock_enable(RCU_GPIOA)`           | `rcc_periph_clock_enable(RCC_GPIOA)`                     |
| `fwdgt_config(0x0FFF, FWDGT_PSC_DIV16)` + `fwdgt_enable()` | `iwdg_set_period_ms(2048) + iwdg_start()` |
| `fwdgt_counter_reload()`                       | `iwdg_reset()`                                            |
| `gpio_mode_set(PORT, MODE_OUTPUT, PUPD_NONE, PIN)` (F1x0 v2) | `gpio_mode_setup(PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, PIN)` |
| `gpio_output_options_set(PORT, OTYPE_PP, OSPEED_50MHZ, PIN)` (F1x0 v2) | `gpio_set_output_options(PORT, GPIO_OTYPE_PP, GPIO_OSPEED_HIGH, PIN)` (note: HIGH, not 50MHZ — see decisions/v0.5/GPIO.md) |
| `gpio_init(PORT, MODE_OUT_PP, OSPEED_50MHZ, PIN)` (F10x v1) | `gpio_set_mode(PORT, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, PIN)` |
| `timer_init(TIMER0, &init_struct)` | `timer_set_mode + timer_set_period + timer_set_prescaler + timer_set_repetition_counter` |
| `timer_event_software_generate(TIMER0, UPG)` | `timer_generate_event(TIM1, TIM_EGR_UG)` |
| `dma_init(DMA_CH0, &init_struct)` | `dma_set_peripheral_address + dma_set_memory_address + dma_set_number_of_data + dma_set_*_size + dma_set_priority` |
| `dma_circulation_enable(DMA_CH0)` | `dma_enable_circular_mode(DMA1, DMA_CHANNEL1)` |
| `dma_channel_enable(DMA_CH0)` | `dma_enable_channel(DMA1, DMA_CHANNEL1)` |
| `usart_baudrate_set(USART0, 115200)` | `usart_set_baudrate(USART1, 115200)` |
| `usart_word_length_set(USART0, USART_WL_8BIT)` | `usart_set_databits(USART1, 8)` |
| `usart_enable(USART0)` | `usart_enable(USART1)` |
| `adc_regular_channel_config(rank, ch, smptime)` | `adc_set_regular_sequence(ADC1, length, channels[]) + adc_set_sample_time(ADC1, ch, smptime)` |
| `adc_calibration_enable()` | `adc_calibrate(ADC1)` |
| `adc_data_alignment_config(ADC_DATAALIGN_RIGHT)` | `adc_set_right_aligned(ADC1)` |
| `adc_external_trigger_config(REGULAR, ENABLE)` | `adc_enable_external_trigger_regular(ADC1, ...)` |
| `i2c_clock_config(I2C0, 100000, DTCY_2)` | `i2c_set_clock_frequency(I2C1, freq) + i2c_set_ccr(I2C1, value)` |
| `i2c_enable(I2C0)` | `i2c_peripheral_enable(I2C1)` |

For per-call empirical equivalence evidence see the regtrace decision
documents at `~/dev/regtrace/decisions/v0.{1,2,5}/` and the captured
golden traces under `~/dev/regtrace/golden/`.

## Known divergences worth flagging in code review

1. **`GPIO_OSPEED_50MHZ` vs `GPIO_OSPEED_HIGH`**. libopencm3 defines
   `GPIO_OSPEED_50MHZ = 0x2` (the F2/F3/F4 50 MHz value), but on STM32F0
   (which gd32/f1x0 forwards to) the 50 MHz speed is encoded as `0x3`,
   exposed as `GPIO_OSPEED_HIGH`. Use `_HIGH` when targeting F1x0 via
   libopencm3, otherwise the OSPEEDR write is wrong. Surfaced
   empirically by regtrace's GPIO vector.

2. **NVIC sub-priority dropped**. gd-spl's `nvic_irq_enable(IRQn, p, s)`
   takes (preempt-priority, sub-priority); libopencm3's `nvic_set_priority`
   takes a single 8-bit value (left-shifted into the upper 4 bits on
   Cortex-M3/M4). The hoverboard always uses `PRE4_SUB0` (no sub-priority),
   so the porting drops the sub-priority arg.

3. **IWDG window mode not exposed**. The hoverboard's `Watchdog_init`
   calls `fwdgt_window_value_config` to gate reloads inside a window.
   libopencm3's iwdg_common_v2 doesn't expose this; the hoverboard's
   actual reload pattern (inside the main loop) sits well inside any
   conceivable window so the port drops it.

## What this PR does NOT do

- The vast majority of `Src/setup.c` (~28 of ~30 functions still STUB).
- `main.c` and any other `.c` file under `Src/`.
- A working ELF — only per-file compile is wired up.
- PlatformIO support. The build uses a hand-rolled Makefile against the
  libopencm3 fork; PlatformIO has no upstream libopencm3 framework.
- Bench validation. Trace-equivalence (proven by regtrace) is necessary
  but not sufficient.
