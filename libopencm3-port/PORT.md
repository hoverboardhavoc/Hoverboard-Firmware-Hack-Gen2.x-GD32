# Hoverboard libopencm3 port — workflow + status

This subdirectory is the proof-of-concept that demonstrates the
hoverboard firmware *can* be built against libopencm3 (instead of the
GigaDevice SPL it uses today), targeting both the GD32F103 (F10x family)
and GD32F130 (F1x0 family) variants.

The actual port — rewriting the project's 11,719 lines of `.c` to
replace gd-spl API calls with libopencm3 equivalents — is **not yet
done**. This subdirectory is the build harness + a single
proof-of-concept file (LED blink + IWDG) to validate the toolchain
plumbing.

## What's here

- `Makefile` — minimal build harness. Builds `blink_<target>.elf` plus
  `.bin` for either target.
- `main_blink.c` — single-file demo using libopencm3 GPIO + IWDG. Same
  source compiles for both families (the GPIO API differs but is gated
  by a `#if defined(GD32F1X0)` block).
- `link_gd32f103.ld` / `link_gd32f130.ld` — minimal linker scripts
  (memory map per chip; vector table at the bottom of FLASH).

## How to build

Prereqs:
- `arm-none-eabi-gcc` on PATH.
- The libopencm3 fork at `~/dev/c/libopencm3` (sibling of this repo's
  parent) with the GD32F10x and GD32F1x0 stubs enabled. That's the
  `hoverboardhavoc/libopencm3` fork's `master` branch — already pushed.

```
$ make TARGET=gd32f10x   # for GD32F103
$ make TARGET=gd32f1x0   # for GD32F130
```

Both produce a ~320–360 byte `.elf`. Tested locally; not yet
bench-validated on real silicon.

## How to extend (i.e., port the actual hoverboard code)

The hoverboard uses ~50 distinct gd-spl API calls across `setup.c`,
`main.c`, the BLDC files, comms, and IMU drivers. Most have direct
libopencm3 equivalents. Roughly:

| gd-spl                                        | libopencm3                                               |
|---|---|
| `timer_init(TIMER0, &init_struct)`            | `timer_set_mode` + `timer_set_period` + `timer_set_prescaler` + `timer_set_repetition_counter` |
| `timer_event_software_generate(TIMER0, UPG)`  | `timer_generate_event(TIM1, TIM_EGR_UG)`                 |
| `dma_init(DMA_CH0, &init_struct)`             | `dma_set_peripheral_address` + `dma_set_memory_address` + `dma_set_number_of_data` + `dma_set_*_size` + `dma_set_priority` (multi-call) |
| `dma_circulation_enable(DMA_CH0)`             | `dma_enable_circular_mode(DMA1, DMA_CHANNEL1)`           |
| `dma_channel_enable(DMA_CH0)`                 | `dma_enable_channel(DMA1, DMA_CHANNEL1)`                 |
| `usart_baudrate_set(USART0, 115200)`          | `usart_set_baudrate(USART1, 115200)`                     |
| `usart_word_length_set(USART0, USART_WL_8BIT)`| `usart_set_databits(USART1, 8)`                          |
| `usart_enable(USART0)`                        | `usart_enable(USART1)`                                   |
| `gpio_mode_set(GPIOA, MODE_OUTPUT, PUPD_NONE, PIN_5)` (F1x0 v2) | `gpio_mode_setup(GPIOA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO5)` |
| `gpio_init(GPIOA, MODE_OUT_PP, OSPEED_50MHZ, PIN_5)` (F10x v1)  | `gpio_set_mode(GPIOA, OUTPUT_50_MHZ, OUTPUT_PUSHPULL, GPIO5)` |
| `fwdgt_config(0x0FFF, FWDGT_PSC_DIV16)`       | `iwdg_set_period_ms(2048)`                                |
| `fwdgt_enable()`                              | `iwdg_start()`                                            |
| `fwdgt_counter_reload()`                      | `iwdg_reset()`                                            |
| `adc_regular_channel_config(0, ch, smptime)`  | `adc_set_regular_sequence(ADC1, 1, &ch)` + `adc_set_sample_time(ADC1, ch, smptime)` |
| `adc_calibration_enable()`                    | `adc_calibrate(ADC1)`                                     |
| `i2c_clock_config(I2C0, 100000, DTCY_2)`      | `i2c_set_clock_frequency(I2C1, freq_mhz)` + `i2c_set_ccr(I2C1, value)` (v1 family) |
| `i2c_enable(I2C0)`                            | `i2c_peripheral_enable(I2C1)`                             |
| `nvic_irq_enable(IRQn, prio, sub_prio)` (SPL) | `nvic_enable_irq(IRQn)` + `nvic_set_priority(IRQn, prio_byte)` |

For per-call empirical equivalence evidence, see the regtrace decision
documents at `~/dev/regtrace/decisions/v0.{1,2,5}/` and the captured
golden traces under `~/dev/regtrace/golden/`. Each decision document
explains the share-or-split rationale for that peripheral.

The recommended order of porting:
1. **Setup the build first.** Take this Makefile and gradually add real
   source files to `SRCS`. Each compile-and-link iteration tells you
   what's still wired to gd-spl.
2. **GPIO then RCC.** Most-pervasive, lowest-risk APIs. Once GPIO compiles,
   the rest of the project's pin-toggle code is unblocked.
3. **TIMER.** Critical for FOC + the watchdog reload tick.
4. **DMA + ADC together.** They're tightly coupled (ADC streams via DMA).
5. **USART.** Required for comms (master-slave + steering).
6. **I2C.** For the IMU. Note the hoverboard already uses a bit-bang
   fallback (`i2c.c`) for some IMU reads — that path is gd-spl-free
   already and can stay as-is.
7. **Interrupt handlers** (`it.c`). Replace SPL-named handlers
   (`USART0_IRQHandler` etc.) with libopencm3-named ones (`usart1_isr`).

Each step is independent and incrementally testable. A good intermediate
goal is "LED + IWDG + USART comms" — that's enough to flash the board and
confirm RTT printf works, without needing the motor running.

## What this PR does NOT do

- Replace the gd-spl `framework = spl` with a libopencm3 framework in
  `platformio.ini`. PlatformIO has no upstream libopencm3 framework
  integration; using libopencm3 means stepping outside PlatformIO (or
  writing a custom `framework-libopencm3` package, which is its own
  body of work).
- Bench-validate anything. Trace-equivalence (proven by regtrace at the
  register level) is necessary but not sufficient — real silicon needs
  bench testing for timing, peripheral feedback, and interrupt handling.
- Touch the BLDC commutation code. That's the most timing-critical part
  of the firmware and should be the LAST thing ported, not the first.
