# libopencm3 port — decision log

## Current checkpoint state (2026-04-27)

| Phase 2 stage | Fork ext. | Vector | Trace OK | Firmware ported |
|---|---|---|---|---|
| 1. clock_init | ✅ rcc.h+rcc.c (MUL17..32, HSI_72MHZ) | ✅ | ✅ (decided) | ✅ |
| 2. gpio_init | ✅ none needed | ✅ pattern-validated (single pin) | ✅ | ✅ |
| 3. watchdog_init | ✅ none needed | ✅ libopencm3/gd32f1x0 leg added | ✅ match | ✅ |
| 4. usart0_init | ✅ none needed | ✅ existing usart+dma legs | ✅ benign CR3=0 | ✅ |
| 5. pwm_init | ⬜ (advanced timer) | ✅ (basic) | ⬜ | ⬜ |
| 6. adc_trigger_timer_init | ⬜ | missing | ⬜ | ⬜ |
| 7. adc_init | ⬜ (large) | partial | ⬜ | ⬜ |
| ISR rename in `it.c` | n/a | n/a | n/a | ⬜ |
| Delete lib/spl/, platformio.ini | n/a | n/a | n/a | ⬜ |
| `make` builds firmware | n/a | n/a | n/a | ⬜ |

Each row is its own multi-step sub-task (fork extension may need its
own regtrace vector author/refresh first). The tabular layout is the
"how far we got" snapshot — the per-stage entries below are the audit
trail.



This file records decisions taken during execution of `libopencm3_port_brief.md`
when the brief is silent or when an unforeseen obstacle warrants a documented
choice. Regtrace-comparator divergences live in
`~/dev/regtrace/decisions/<version>/<PERIPHERAL>.md`, not here — this log is
for firmware-side and build-system choices only.

Format: most recent first. Each entry has an ISO date, short title,
**Why**, **What I did**, and (if applicable) **Consequence**.

---

## 2026-04-27 — Pacing: complete the port in dependency order, commit per-peripheral

**Why:** The brief's Phase 2 sequence (clock → gpio → watchdog → usart0 → pwm
→ adc-trigger-timer → adc) is a strict dependency chain — each stage's
correctness depends on the prior stage. The libopencm3/gd32-f1x0 fork's
peripheral surface is also incomplete (only thin headers for several
peripherals; no setup helpers; no scan/DMA wiring), so most stages will hit
the Showstopper case at least once. Trying to land all stages in a single
edit-then-build pass would mean a long stretch where the firmware can't link
or trace, with no checkpoints to bisect against.

**What I did:** Treat each Phase 2 stage as a self-contained sub-task:
extend the fork (add header decls + lib body) → build the fork archive →
author/refresh the regtrace vector for that stage's `libopencm3/gd32f1x0`
body → port the firmware function. Build the firmware and run regtrace at
the end of each stage. The intermediate states keep `lib/spl/` on disk so
unported callsites still link; once every stage is ported, `lib/spl/` and
`platformio.ini` come out per Done condition #1.

**Consequence:** During the port, `Src/setup.c` cannot compile in a
mixed state — `defines.h` pulls in SPL headers (`gd32f1x0.h`,
`gd32f1x0_rcu.h`, etc.) for every translation unit, and SPL/libopencm3
share namespace on `gpio_*`, `timer_*`, `adc_*`, `dma_*` (collision on
include). The brief's escape hatch is per guardrail #4: replace each
unported function body with `#error "<fn> not ported"` so the build
fails loudly at the unported call site. Phase 2 then proceeds
function-by-function, but the firmware archive only links once every
stage in the dependency list is complete. The check is re-run at the
end (Phase 3 + Done condition #1).

This means the per-stage **regtrace** verification (run as each fork
extension lands) is the operative milestone — the firmware build is
green only at the very end. Each commit boundary should align with
"fork + vector + decisions" trinity for one stage, not with a green
firmware build.

---

## 2026-04-27 — Phase 2 stage 4: usart0_init ported (with DMA RX)

**Why:** Brief Phase 2 stage 4: `USART0_Init` + RX ISR + USART1_IRQ
NVIC enable. The brief and the firmware actually use DMA Channel 1/2
IRQ (not USART1 IRQ proper) because the RX path is DMA-driven, not
register-polled. Renaming the it.c handler from
`DMA_Channel1_2_IRQHandler` to libopencm3's `dma1_channel2_3_isr`
happens in the it.c rename stage.

**What I did:**

1. **`Src/setup.c::usart0_init`** (formerly `USART0_Init`): every
   peripheral, GPIO, USART, DMA and NVIC call inlined as direct
   libopencm3 calls — no `pinModeAF` / `AF_USART0_TX` / `TARGET_DMA_*` /
   `TARGET_nvic_irq_enable` / `dma_init_struct_usart` left.

2. **Naming translation**: GD32 `USART0` ↔ libopencm3 `USART1` (same
   APB2[14] peripheral, vendor-specific naming offset). GD `DMA_CH2`
   ↔ libopencm3 `DMA1, DMA_CHANNEL3` (libopencm3 numbers DMA channels
   from 1, GD from 0). GD `DMA_Channel1_2_IRQn` ↔ libopencm3
   `NVIC_DMA_CHANNEL2_3_IRQ`. Documented inline.

3. **DMA struct → individual setters**: SPL's
   `dma_init_struct_usart.{direction,memory_addr,memory_inc,...}`
   gather-then-init-once pattern replaced by libopencm3's
   per-attribute setter calls (`dma_set_peripheral_address`,
   `dma_set_memory_address`, `dma_set_number_of_data`,
   `dma_set_read_from_peripheral`, `dma_disable_peripheral_increment_mode`,
   `dma_enable_memory_increment_mode`, `dma_set_peripheral_size`,
   `dma_set_memory_size`, `dma_set_priority`, `dma_enable_circular_mode`,
   `dma_enable_transfer_complete_interrupt`, `dma_enable_channel`).
   Sequence is identical; bit positions in DMA_CCR3 / DMA_CNDTR3 /
   DMA_CPAR3 / DMA_CMAR3 are register-compatible per
   `~/dev/regtrace/decisions/v0.2/DMA.md`.

4. **NVIC priority encoding**: SPL `nvic_irq_enable(IRQn, 2, 0)` with
   PRIGROUP_NOSUB → libopencm3 `nvic_set_priority(IRQn, 2 << 4) +
   nvic_enable_irq(IRQn)`. The `<< 4` is because Cortex-M3 implements
   only the upper 4 bits of the 8-bit priority byte. With clock_init's
   `scb_set_priority_grouping(SCB_AIRCR_PRIGROUP_NOSUB)`, all 4 bits
   pre-empt — so `2 << 4 = 0x20` reads back as pre-emption priority 2.

5. **AF inlined**: USART0 TX/RX is `GPIO_AF0` if the pin is PB6/PB7
   else `GPIO_AF1` (per GD32F130 datasheet 2.6.7). Inlined as a
   ternary at the callsite. The active layout (defines_2-1-20.h) uses
   PB6/PB7 → AF0, but the ternary keeps the code working for layouts
   that put USART0 on PA9/PA10/PA14/PA15.

6. **Oversampling**: SPL's `usart_oversample_config(USART0,
   USART_OVSMOD_16)` was redundant — CR1.OVER8 is post-reset 0
   (= 16x oversampling). Dropped; libopencm3 also defaults to 16x.

7. **Vector verification**: `regtrace compare usart_init_115200_8n1`
   for `gd-spl/gd32f1x0` ↔ `libopencm3/gd32f1x0` reports
   "divergent (1 differences) — `libopencm3-only: W4 <USART1_BASE>+0x08
   0x00000000`". CR3 explicit-clear from `usart_set_flow_control(NONE)`.
   Decided-acceptable — CR3 final state is 0 in both;
   `~/dev/regtrace/decisions/v0.2/USART.md` covers it.

8. **`Src/main.c`, `Inc/setup.h`**: callsite + declaration renamed
   `USART0_Init` → `usart0_init`.

**Out of scope this stage**: `usart1_init` (the master/slave + steering
USART) — same pattern but different DMA channel (CH4 → libopencm3
DMA_CHANNEL5) and different NVIC IRQ (`NVIC_DMA_CHANNEL4_5_IRQ`).
The `usart1_rx_buf` global stays. Same for `usart2_rx_buf` (TARGET=2
only, F103, out of scope per brief).

The `dma_init_struct_usart` global at the top of setup.c is still
referenced by the unported USART1_Init; it goes when that stage
lands. Same for `dma_init_struct_adc`.

---

## 2026-04-27 — Phase 2 stage 3: watchdog_init ported

**Why:** Phase 2 stage 3 per dependency order. The IWDG is the only
boot-critical peripheral whose mistuning has no soft-recovery — get it
wrong and the firmware reboots in a loop. The vector
`vectors/iwdg/config_2sec_period.yaml` already pinned the configuration
intent in v0.5+, but the `libopencm3/gd32f1x0` implementation leg was
missing.

**What I did:**

1. **Regtrace vector**: added the `libopencm3/gd32f1x0` impl to
   `vectors/iwdg/config_2sec_period.yaml`. Body is identical to the
   `libopencm3/stm32f0` leg — `iwdg_set_period_ms(2048); iwdg_start();` —
   because the fork's `gd32/f1x0/iwdg.h` forwards directly to
   `stm32/common/iwdg_common_v2.h` (FWDGT is register-compatible with
   STM32F0 IWDG including WINR). `regtrace compare` against `gd-spl/gd32f1x0`
   in `final_state` mode → **match**.

2. **`Src/setup.c::watchdog_init`** (formerly `Watchdog_init`):
   - `iwdg_set_period_ms(2048) + iwdg_start()` replaces
     `fwdgt_config(0x0FFF, FWDGT_PSC_DIV16) + fwdgt_enable`. Final
     IWDG_PR / IWDG_RLR state is byte-identical (PR=2 = /16, RLR=0xFFF);
     verified by the regtrace vector in `final_state` mode. The
     `register_writes` divergence noted in the vector — libopencm3 emits
     a RELOAD key (0xAAAA) and a duplicate START key — is benign because
     the IWDG state machine treats them as no-ops once the counter is
     loaded.
   - Window-mode write dropped. SPL's `fwdgt_window_value_config(0x0FFF)`
     programmed IWDG_WINR to its post-reset default (0x0FFF = no window).
     `iwdg_set_period_ms` doesn't touch WINR, so the final state is the
     same. Avoids the dependency on libopencm3's missing window helper
     (would have been a Showstopper otherwise).
   - Reset-cause flag check ported as a direct register read:
     `if (RCC_CSR & RCC_CSR_IWDGRSTF) RCC_CSR |= RCC_CSR_RMVF;`. The
     SPL `rcu_flag_get(RCU_FLAG_FWDGTRST)` + `rcu_all_reset_flag_clear()`
     pair is two SPL helpers wrapping the same two writes; inlined in
     libopencm3 spelling. Diagnostic-only — the firmware doesn't take
     a different path on watchdog-reset.

3. **Inlined `TARGET_fwdgt_window_value_config(...)` shim** out: the
   F130 path of target.h had `#define TARGET_fwdgt_window_value_config(a)
   fwdgt_window_value_config(a)` (pass-through). Per guardrail #5 the
   shim layer goes; replaced the value with a documented "no-op the
   write" rationale.

4. **`Src/main.c`, `Inc/setup.h`**: callsite + declaration renamed
   `Watchdog_init` → `watchdog_init`.

**Vector coverage**: `vectors/iwdg/config_2sec_period.yaml` (already
covered IWDG; added gd32f1x0 leg). `final_state` match confirmed.

---

## 2026-04-27 — Phase 2 stage 2: gpio_init ported (inlined macros)

**Why:** Per Phase 2 dependency order, gpio_init follows clock_init.
The brief's guardrail #5 (no SPL-shaped shims) means the per-pin
config can't be carried by the existing `pinMode` / `pinModeAF` /
`AF_TIMER0_BLDC` macro family in `Inc/target.h` — those wrap the SPL
gpio_mode_set / gpio_output_options_set / gpio_af_set calls and would
need to either be retargeted or removed. User direction: inline.

**What I did:**

1. **`Src/setup.c::gpio_init`** (formerly `GPIO_init`): every pin's
   config inlined as direct libopencm3 calls — `gpio_mode_setup`,
   `gpio_set_output_options`, and (for AF pins) `gpio_set_af`. No use
   of pinMode/pinModeAF/AF_TIMER0_*/pinModePull/pinModeSpeed inside
   the function body. The firmware's packed pin-code convention
   `(GPIOx | n)` survives — extracted at each callsite via
   `& 0xffffff00U` for the port and `1U << (& 0xfU)` for the bit
   mask.

2. **AF map inlined too**: PWM channels (BLDC_GH/GL/BH/BL/YH/YL) get
   `GPIO_AF2` directly (TIMER0 alt-function on GD32F130 per datasheet
   2.6.7). USART AFs not touched at this stage — they'll inline at
   `usart0_init` time. The
   `AF_TIMER0_BLDC(pin)` / `AF_USART0_TX(pin)` / `AF_USART0_RX(pin)`
   pin-conditional macros in `target.h` still exist for unported
   callers; they get deleted at the end with the rest of `target.h`'s
   F130 path.

3. **Speed mapping**: `GPIO_OSPEED_2MHZ` → `GPIO_OSPEED_LOW`,
   `_10MHZ` → `_MED`, `_50MHZ` → `_HIGH`. Numeric values are
   identical (0/1/3) — libopencm3 just uses the speed-class names
   instead of the absolute-MHz names.

4. **Clock enable**: `rcu_periph_clock_enable(RCU_GPIOA..F)` →
   `rcc_periph_clock_enable(RCC_GPIOA..F)` for all four ports.

5. **`Src/main.c`, `Inc/setup.h`**: callsite + declaration renamed
   `GPIO_init` → `gpio_init`.

6. **`Makefile` build flags fixed**: removed `-D STM32F1`. The fork's
   `<libopencm3/stm32/gpio.h>` dispatcher is an `#elif` chain with
   STM32F1 *before* GD32F1X0 — keeping STM32F1 set would have routed
   the build to the v1 (CRL/CRH) GPIO instead of the v2 (MODER/PUPDR/
   OSPEEDR/AFRL/AFRH) GPIO that GD32F130 actually has. The fork's own
   gd32/f1x0/gpio.h forwards to stm32/f0/gpio.h (v2) which is
   correct. Added `-D GD32F130` and `-D TARGET=1` while there to
   match what `platformio.ini` and the uvision project define — the
   firmware's `Inc/target.h` and the active layout
   `Inc/defines/defines_2-1-${LAYOUT}.h` need both.

**Vector coverage**: the existing `vectors/gpio/output_pa5_pp_50mhz.yaml`
covers ONE pin (PA5 push-pull output) with `gd-spl/gd32f1x0` ↔
`libopencm3/gd32f1x0` validated to match (modulo a benign
explicit-zero on F10x). The hoverboard `gpio_init` is N applications
of the same per-pin pattern — no new fork APIs touched, no register
sequencing differs from the validated single-pin case. A "covering"
vector that does all ~20 pins would be useful but redundant given the
inlined body is the same calls × N. Recorded as "pattern-validated"
rather than "function-covered" in the status table; if regtrace
caches a per-pin failure mode I'd need to re-evaluate, but the F1x0
v2 GPIO is straightforward bitfield-per-pin.

**Consequence**: gpio_init no longer goes through any helper layer.
The pinMode/pinModeAF/etc. macros in `Inc/target.h` survive for now
because other source files (`bldc.c`, `led.c`, `comms*.c`,
`remote*.c`, `main.c`) still call them. They'll be inlined out of
those files in their own ports — or deleted en bloc when target.h's
F130 path is dismantled at the end. The firmware build remains
broken at link time on the unported peripheral inits (watchdog,
USART, PWM, ADC), as expected per guardrail #4.

---

## 2026-04-27 — Phase 2 stage 1: clock_init ported

**Why:** RCC fork is now ready (entry below). The Phase 2 dependency chain
starts here and nothing downstream gets a SystemCoreClock/rcc_ahb_frequency
that's correct without it.

**What I did:**
1. **`Src/setup.c`**: replaced `Clock_init`'s F130 body (which was a
   diagnostic-only stub relying on SystemInit having configured the PLL
   before main) with `rcc_clock_setup_pll(&rcc_hsi_configs[RCC_CLOCK_HSI_72MHZ])`,
   the libopencm3 path that produces the regtrace-validated PLL config.
   Renamed `Clock_init` → `clock_init` (snake_case per brief).
2. **Folded `Interrupt_init` into `clock_init`**: the priority-grouping
   call became `scb_set_priority_grouping(SCB_AIRCR_PRIGROUP_NOSUB)` —
   the libopencm3 spelling of NVIC_PRIGROUP_PRE4_SUB0 (4 pre-empt bits,
   0 sub-priority bits). Deleted the `Interrupt_init` function and its
   declaration in `setup.h`.
3. **Diagnostic globals preserved**: `dev_id` reads DBGMCU_IDCODE; `pll_mul`
   now reads PLLMUL via `RCC_CFGR_PLLMUL_0_3 / RCC_CFGR_PLLMUL_4` — the
   libopencm3 spelling — so it correctly captures the wide 5-bit field
   instead of the legacy 4-bit one. After
   `rcc_clock_setup_pll(...HSI_72MHZ)` it should read 0x11 (MUL18).
4. **`Src/main.c`**: callsite renamed to `clock_init()`; the
   `Interrupt_init()` call removed (folded).
5. **`Src/setup.c` includes `<libopencm3/cm3/scb.h>` and
   `<libopencm3/gd32/f1x0/rcc.h>`** alongside the existing SPL umbrella.
   Safe because libopencm3's `rcc_*` and `scb_*` namespaces don't collide
   with SPL's `rcu_*`. Subsequent stages will need to swap collision-
   prone headers (`gpio_*`, `timer_*`, `adc_*`, `dma_*`) and that's where
   each follow-on stage's first job is.

**Consequence:** The firmware as a whole still uses SPL for everything
else — by design (per the staging consequence below). The `make` build
will fail at link time on every other init function until those land.
That's the brief's intended state per guardrail #4: loud, not silent.
Phase 2 stage 2 (`gpio_init`) is the next concrete sub-task; it will
need fork extensions for the GPIO v2 alternate-function helper that the
firmware's `pinModeAF` macro currently provides via SPL.

---

## 2026-04-27 — RCC fork extension (Showstopper #1 resolved): 72 MHz IRC8M

**Why:** Phase 2 stage 1 (`Clock_init` → `clock_init`) needs to set up the
firmware's canonical 72 MHz IRC8M / 2 × 18 PLL. The libopencm3 GD32F1x0
fork at `~/dev/c/libopencm3` (branch `master`, rev `d498c397`) only
shipped 48 MHz and 64 MHz IRC8M configs; the 72 MHz one needs PLLMF[4] at
RCU_CFG0 bit 27, which STM32F1 doesn't have. This is the predicted first
Showstopper (per brief's "F130 risks ADC v2 / GPIO v2 gaps" + RCC.md
decision file at `~/dev/regtrace/decisions/v0.5+/RCC.md`).

**What I did:**
1. **Fork header** (`include/libopencm3/gd32/f1x0/rcc.h`): added
   `RCC_CFGR_PLLMUL_PLL_CLK_MUL17..MUL32` constants. The encoding has a
   discontinuity — when PLLMF[4]=1 the low nibble restarts at 0
   (MUL17=0x10, MUL18=0x11, …), matching the SPL macros
   `RCU_PLL_MUL17..32` in `gd32f1x0_rcu.h`. This was *not* obvious from
   the regtrace decisions doc — the doc misstated MUL18 as
   `PLLMF[3:0]=0b0000 + PLLMF[4]=1`; the actual SPL trace at step [13]
   writes `0x08040008` which is PLLMF[4]+PLLMF[0], i.e. encoding=0x11.
   Caught this by regrep'ing `RCU_PLL_MUL18` in the GD32 SPL source —
   first attempt at adding the constants used encoding `0x10` for
   MUL18 and traced as 64 MHz; the fix was a 1-bit shift across all
   added MUL constants.
2. **Fork lib** (`lib/gd32/f1x0/rcc.c`): added a third entry to
   `rcc_hsi_configs[]` for 72 MHz (pllmul=MUL18, hpre=NODIV, ppre1=DIV2,
   ppre2=NODIV). New enum value `RCC_CLOCK_HSI_72MHZ` joins
   `RCC_CLOCK_HSI_48MHZ` / `RCC_CLOCK_HSI_64MHZ` in `enum rcc_clock_hsi`.
3. **Fork archive rebuilt:** `cd ~/dev/c/libopencm3 && make TARGETS=gd32/f1x0`.
4. **Regtrace vector** `rcc/irc8m_pll_72mhz.yaml`: `libopencm3/gd32f1x0`
   body now calls `rcc_clock_setup_pll(&rcc_hsi_configs[RCC_CLOCK_HSI_72MHZ])`
   (was a placeholder calling the nonexistent 64MHz helper). Added
   `libopencm3/cm3/common.h` to includes so `uint32_t` resolves.
5. **`regtrace clean --libs && regtrace compare rcc_irc8m_pll_72mhz`**:
   end state on RCU_CFG0 matches gd-spl bit-identically (0x0000000A);
   PLLMF write at step [6] in the libopencm3 trace produces the same
   0x08040008 the gd-spl trace produces at step [13].
6. **`~/dev/regtrace/decisions/v0.5+/RCC.md`** updated: the predicted
   "build leg failed" status is resolved; the 14 remaining trace-position
   differences are decided-acceptable (gd-spl extra reset-defaults
   writes that touch GD-only registers RCU_CFG2/CFG3/CTL1/INT;
   piecewise vs batched RCU_CFG0 updates; explicit-zero HPRE write).

**Consequence:** Phase 2 stage 1 (`clock_init`) is unblocked.
`Src/setup.c::Clock_init` can now be ported by replacing the F130 path's
`SystemCoreClockUpdate()`-only body (which relied on SystemInit running
PLL setup before main) with a direct call to
`rcc_clock_setup_pll(&rcc_hsi_configs[RCC_CLOCK_HSI_72MHZ])`. The
`scb_set_priority_grouping(SCB_AIRCR_PRIGROUP_NOSUB)` call from the
deleted `Interrupt_init` folds into the same function per brief.

The diagnostic globals `dev_id` and `pll_mul` (`Src/setup.c:1152-1153`)
that read `DBGMCU_IDCODE` and `RCU_CFG0_PLLMF` should be kept — they're
McuViewer/StmStudio probes, not HAL state. They reference SPL macros for
register addresses, which need to be re-spelled in libopencm3 terms
(`SCS_DEMCR` / `(uint32_t*)0xE0042000` / `RCC_CFGR & RCC_CFGR_PLLMUL_*`).

---

## 2026-04-27 — Build system: Makefile + libopencm3 link script per brief

**Why:** Brief explicitly mandates `make`, `arm-none-eabi-gcc` directly,
linker script at `lib/libopencm3/link_gd32f130.ld`, no PIO. Just executing
the brief.

**What I did:** Wrote `HoverBoardGigaDevice/Makefile` and
`HoverBoardGigaDevice/lib/libopencm3/link_gd32f130.ld`. The Makefile passes
`-D GD32F1X0` and `-D STM32F1` (the latter because the fork's gd32/f1x0
headers `#include <libopencm3/stm32/...>` for shared peripherals), links
the fork's prebuilt archive, and includes `cortex-m-generic.ld` per the
fork's convention. A `fork-check` target prints a pointed message if
`libopencm3_gd32f1x0.a` is missing.
