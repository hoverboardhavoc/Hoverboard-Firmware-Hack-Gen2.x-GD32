# libopencm3 port brief

Replace the GD32 SPL HAL calls in this firmware with libopencm3 calls.
Edit files in place; the port is a HAL swap underneath the existing
firmware behavior.


---

## Where the work lives

The HAL-touching files are `Src/setup.c`, `Src/it.c`, `Src/bldc.c`,
plus anything else that calls SPL directly. The exact work list is
discovered by Phase 1 below — don't try to enumerate it by grep up
front; the compiler does a more thorough job.

---

## Build

**End state: `make` builds the F130 target.** The canonical build is
a Makefile in `HoverBoardGigaDevice/`; PlatformIO is dropped. Users
install `arm-none-eabi-gcc` themselves (Homebrew: `brew install
--cask gcc-arm-embedded`; Linux: distro package or the Arm-published
tarball). Flashing and RTT continue to use the existing
`tool-openocd/` and the `rtt.bat` / `openocd_rtt_*.bat` /
`rtt_supervisor.sh` / `rtt_plot.py` scripts — none of those are
PIO-coupled.

The libopencm3 GD32 family support is only in the hoverboardhavoc
fork (sibling repo at `~/dev/c/libopencm3`, branch `master`), not
upstream.

**Build the fork before the first `make`** — the firmware links
against `~/dev/c/libopencm3/lib/libopencm3_gd32f1x0.a`,
and the fork doesn't ship a prebuilt archive:

```bash
cd ~/dev/c/libopencm3 && make TARGETS=gd32/f1x0
```

Re-run whenever the fork moves (new commit pulled, local change to
`lib/gd32/f1x0/*.c`, etc.). The firmware Makefile checks for the
archive's existence and fails with a pointed error message if it's
missing — don't let a generic linker error eat half an hour when the
diagnostic is one path check away.

The Makefile invokes `arm-none-eabi-gcc` directly. It compiles
`Src/*.c` with `-I Inc -I ../../libopencm3/include -D GD32F1X0`,
links against `~/dev/c/libopencm3/lib/libopencm3_gd32f1x0.a` via
`-L ../../libopencm3/lib -l:libopencm3_gd32f1x0.a`, and uses the
linker script described below. Outputs `build/firmware.elf` and
`build/firmware.bin`. No `framework = libopencm3` integration, no
`platform-gd32` package, no per-target board file — all of that
machinery existed because PIO needed it, and PIO is gone.

The linker script lives at `lib/libopencm3/link_gd32f130.ld`,
parallel to the existing `lib/spl/gd32f1x0/gd32f1x0.ld`. It defines
the SKU memory map and includes libopencm3's generic Cortex-M
script:

```ld
/* GD32F130C8: 64K flash, 8K RAM */
MEMORY {
    rom (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 8K
}
INCLUDE cortex-m-generic.ld
```

`cortex-m-generic.ld` (provides `_stack`, `reset_handler`, init/fini
arrays) is found via the `-L ../../libopencm3/lib` flag the Makefile
already passes to the linker.

The 64K/8K numbers above match the C8 SKU. If a board with a
different SKU (e.g. F130CB at 128K/16K) ever needs to build, that's
a new linker script — don't parameterize this one.

Single target: F130 (`-D GD32F1X0`, libopencm3 family `gd32/f1x0`).
F103 and E230 are out of scope for this port. Users on F103/E230
should pin to a pre-port commit.

No `#ifdef` ladders for cross-family code, no `HOVER_*` macro shims
that re-define libopencm3's RCC/NVIC names — those just re-create the
SPL abstraction layer the port is supposed to remove.

**Naming**: rename the setup functions being ported to snake_case
(`clock_init`, `gpio_init`, `watchdog_init`, `usart0_init`,
`pwm_init`, `adc_init`, `adc_trigger_timer_init`) and update every
callsite (`Src/main.c`, `Src/it.c`, anywhere else they're
referenced). This is a deliberate stylistic refactor riding
alongside the HAL swap, not an accident: the brief picks snake_case
because it's the C standard and matches libopencm3's own convention,
and renames in one pass rather than leaving a mixed `clock_init` /
`GPIO_init` codebase. Scope is exactly the enumerated setup
functions — not every function in `Src/setup.c`, and emphatically
not every function in `Src/bldc.c` or `Src/it.c`. Business-logic
functions and ISR handlers follow their own rules (the ISR handlers
get renamed to libopencm3's `*_isr` form per Phase 2's table, which
is a separate rule from the snake_case refactor).

Delete `platformio.ini` entirely, along with the PIO-only helper
scripts that have no purpose without it (`add_RTT_task.py`,
`add_RTT_task_gd32e230.py`, `add_RTT_console.py` — all three
register PlatformIO custom targets and do nothing standalone). The
HAL-agnostic RTT entry points users actually invoke (`rtt.bat`,
`openocd_rtt_*.bat`, `rtt_supervisor.sh`, `rtt_plot.py`) stay
untouched. If SPL behavior is needed for comparison, check out a
pre-port commit.

---

## Showstopper: missing libopencm3 functionality

The hoverboardhavoc/libopencm3 fork's GD32 family support was built
empirically as regtrace mined vectors. Coverage of `gd32/f1x0` is
**incomplete**. The firmware may hit a peripheral function, register,
IRQ symbol, or RCC clock-enable bit that the fork doesn't yet
provide.

When that happens, **the port stops** until the fork is extended.
Don't paper over a missing function with an inline register write in
`Src/setup.c` — that's the SPL HAL leaking back in under a different
name and it defeats the point of the port. Either:

- Add the function to libopencm3 (mine a regtrace vector if needed,
  commit to the fork), then resume the port.
- If the missing piece is genuinely out of scope for libopencm3
  upstream (vendor-specific quirk with no STM32 analogue), still add
  it to the fork as a `gd32/f1x0/`-local function — keep the
  in-firmware HAL surface uniform.

Expect this to happen at least once. F130 (`gd32/f1x0`) risks ADC v2
/ GPIO v2 gaps. Treat each gap as a normal sub-task: fork-side change
first, port resumes after the fork builds clean.

---

## Tooling: regtrace

Equivalence between the SPL and libopencm3 builds is established by
[regtrace](https://github.com/hoverboardhavoc/regtrace), checked out
at `~/dev/regtrace`. regtrace doesn't compare firmware ELFs — it
compares *vector snippets*: small standalone peripheral-config
fragments authored in `~/dev/regtrace/vectors/<peripheral>/<name>.yaml`,
each containing multiple implementation bodies (`gd-spl/gd32f1x0`,
`libopencm3/gd32f1x0`, …). The tool builds each body into an ELF,
emulates it under Unicorn, and diffs the resulting register-write
traces.

Workflow for one peripheral:

1. Activate: `source ~/dev/regtrace/.venv/bin/activate` (one-time
   setup: `regtrace selftest --bootstrap`).
2. Find or author the vector covering the function being ported. The
   vector's `gd-spl/gd32f1x0` body is the SPL ground truth (mirrors
   what `Src/setup.c` does today).
3. Iterate on the vector's `libopencm3/gd32f1x0` body until it
   compiles and traces identically to the SPL body:
   `regtrace compare <peripheral>/<name>` — exits 0 on match,
   non-zero on diff with the divergent register writes printed.
4. Once the vector matches, copy that exact `libopencm3/gd32f1x0`
   body into the corresponding `Src/setup.c` function. The body in
   the vector and the body in the firmware should be the same code.
5. If the trace can't be made to match cleanly because libopencm3 is
   missing functionality, that's the Showstopper case — extend the
   fork, then resume.
6. If the trace can't be made to match because of a deliberate
   behavioral difference (e.g. SPL writes a reset value that
   libopencm3 leaves at the post-reset default and the post-reset
   default is identical), record the rationale in
   `~/dev/regtrace/decisions/<version>/<PERIPHERAL>.md` per
   `decisions/TEMPLATE.md` and treat the vector as decided.
   `<version>` is the regtrace decisions-snapshot version, e.g.
   `v0.5+` (a `+` suffix means the working snapshot ahead of the
   next tag); pick the highest existing folder under
   `~/dev/regtrace/decisions/`. `<PERIPHERAL>` is uppercase
   (`RCC.md`, `ADC.md`, `IWDG.md`), matching the existing files.

A "missing vector" — a setup function this firmware uses but no
regtrace vector covers — is a normal sub-task: author the YAML in
regtrace first (with the SPL body matching what's currently in
`Src/setup.c`), then port. List current coverage with
`ls ~/dev/regtrace/vectors/`; expect gaps for any function in the
Phase 2 list that isn't covered there.

Note on `bootstrap.toml`: regtrace's `[repos.libopencm3]` is pinned
to the hoverboardhavoc fork (`hoverboardhavoc/libopencm3`, currently
`d498c397`), not upstream — upstream has no GD32 support so tracing
`libopencm3/gd32f1x0` against it would be meaningless. The fork is
what the firmware links against. **When the fork's master moves,
bump the pin in `~/dev/regtrace/bootstrap.toml` and re-capture
goldens** — otherwise regtrace will trace one revision and the
firmware will link another, and Done condition #2 stops being a
meaningful guarantee. The commit history of `bootstrap.toml`
doubles as the audit trail for those bumps.

## Done condition

Hardware-free. The port is done when:

1. `make` builds clean against the hoverboardhavoc/libopencm3 fork
   — no SPL headers included, no SPL function names referenced, no
   inline-register-write workarounds in `Src/`. The authoritative
   verifier is the build itself: with `lib/spl/` deleted, any
   surviving SPL `#include` fails the compile and any surviving SPL
   function call fails the link. No name-grep can match that — too
   many SPL identifiers collide with libopencm3 identifiers
   (`gpio_init`, `usart_init`, `timer_init`, `dma_init`) for a
   regex to be reliable. Verify by:
   - `lib/spl/` is deleted from the working tree. SPL remains
     reachable via `git checkout` of a pre-port commit if
     comparison is needed.
   - `platformio.ini` is deleted from the working tree, along with
     `add_RTT_task.py`, `add_RTT_task_gd32e230.py`, and
     `add_RTT_console.py` (PIO-only helpers).
   - `make clean && make` succeeds.
2. For every setup function this firmware uses (`Clock_init`,
   `Watchdog_init`, `GPIO_init`, `PWM_init`, `ADC_init`,
   `ADC_Trigger_Timer_init`, `USART0_Init`, the DMA setup inside
   `ADC_init`), there exists a regtrace vector whose
   `gd-spl/gd32f1x0` and `libopencm3/gd32f1x0` bodies trace
   identically (or are explicitly decided-acceptable in
   `~/dev/regtrace/decisions/`), **and** the firmware's
   `Src/setup.c` body for that function is a literal copy of the
   vector's `libopencm3/gd32f1x0` body. NVIC enables are folded
   into the peripheral they belong to — the regtrace vector for a
   peripheral covers its NVIC writes too. Identical peripheral
   writes ⇒ identical hardware behavior. Any setup function with no
   covering vector is a missing vector — author it before declaring
   done.
3. ISR symbols resolve to libopencm3's vector table entries with no
   silent fallback to the weak default handler. **The linker will
   not warn if a handler is misnamed** — a CMSIS-style
   `USART0_IRQHandler` left in `Src/it.c` after the port links
   cleanly, but its vector slot holds libopencm3's default handler
   and the interrupt silently does nothing. Verify two ways:
   (a) `nm` the .elf and confirm each handler symbol matches the
   libopencm3 name (lowercase `*_isr`); (b) cross-check every
   handler in `Src/it.c` against `libopencm3/include/libopencm3/stm32/f1/nvic.h`
   (or the F1x0 equivalent in the fork) — every entry that should
   fire must have a corresponding non-default symbol.

Bench validation isn't part of "done" — it's a separate effort that
follows. The port's job is "produces equivalent register writes";
proving "the wheel turns" is downstream.

---

## Phase 1 — establish the SPL ground truth in regtrace

The implementer's real gating constraint isn't "what SPL symbols does
`Src/setup.c` use" — the compiler discovers that for free during
Phase 2. The real blocker is "does a regtrace vector exist for every
function we need to port?" If not, Done condition #2 can't be
verified. Phase 1 closes that gap before any porting starts.

For each function in the Phase 2 dependency-order list (`clock_init`,
`gpio_init`, `watchdog_init`, `usart0_init`, `pwm_init`,
`adc_trigger_timer_init`, `adc_init`):

1. Check whether a covering regtrace vector exists under
   `~/dev/regtrace/vectors/<peripheral>/` (`ls` it — don't trust a
   snapshot in this brief). A peripheral having *a* vector is not
   the same as having one that covers the function this firmware
   actually calls: e.g. a GPIO vector for one pin doesn't cover the
   full pin map, and a generic ADC init may not match the
   trigger-timer-driven configuration here. Expect to author several.
2. For each missing vector, author the YAML with at minimum the
   `gd-spl/gd32f1x0` implementation body. That body must faithfully
   mirror what `Src/setup.c` currently does for the function being
   covered — copy the SPL code in directly, don't paraphrase. This
   is the SPL ground truth Phase 2 will diff against.
3. Run `regtrace trace <peripheral>/<vector>` to confirm the
   SPL body emulates and produces a clean register-write trace.
   If it doesn't, the vector is wrong before Phase 2 even starts —
   fix it now, not later.
4. Leave the `libopencm3/gd32f1x0` body empty or stubbed (e.g.
   `// TODO: phase 2`). It gets filled in during Phase 2 and is
   the actual port work.

Phase 1 ends when every function in the Phase 2 list has a covering
vector whose SPL body traces clean. The list of "what needs porting"
is now expressed as "what `libopencm3/gd32f1x0` bodies are stubbed."

`Src/setup.c` is not edited during Phase 1.

---

## Phase 2 — implement, in dependency order

Replace the SPL calls with libopencm3 calls in this order, since
broken stuff cascades from earlier steps:

1. `Clock_init` — nothing else has the right SystemCoreClock without
   it.
2. `GPIO_init` — peripherals don't reach pins without it.
3. `Watchdog_init`.
4. `USART0_Init` + RX ISR + USART1_IRQ NVIC enable.
5. `PWM_init`, `ADC_Trigger_Timer_init`, `ADC_init`, DMA + ISRs +
   the NVIC enables for each (TIM1_BRK/UP, ADC1_2, DMA1_Channel1, …).

Every per-peripheral init contains `rcu_periph_clock_enable` calls;
substitute `rcc_periph_clock_enable` in place as part of porting
that peripheral's function. There's no separate clock-enables step.

**`Interrupt_init` is deleted.** Today it does one thing — sets
NVIC priority grouping to `PRE4_SUB0` (all 4 bits pre-empt, no
sub-priority) via `TARGET_nvic_priority_group_set`. NVIC enables
are *already* per-peripheral in `Src/setup.c`, so nothing needs to
be redistributed; that part of the design was always like this.
The priority-grouping call needs a new home — the libopencm3
equivalent is `scb_set_priority_grouping(SCB_AIRCR_PRIGROUP_NOSUB)`.
Fold it into `clock_init` (it runs once at boot, has no peripheral
affinity, and `clock_init` is the only function that runs
unconditionally before any IRQ-using peripheral comes up). Once
that's done, `Interrupt_init` and its `main.c` callsite can be
deleted. Each peripheral's regtrace vector covers the NVIC writes
for that peripheral too (the vector body for `adc_init` includes
`nvic_enable_irq(NVIC_ADC1_2_IRQ)` and the DMA channel enable,
etc.).

When a libopencm3 function is missing (Showstopper section above),
pause Phase 2, extend the fork, resume.

### ISR renaming

Every handler in `Src/it.c` uses CMSIS/SPL names
(`*_IRQHandler`, mixed case). libopencm3's vector table uses
lowercase `*_isr` — and follows STM32 numbering, not GD numbering,
because the fork's `gd32/f1x0` peripheral support is built on top
of the existing STM32 names. A few representative renames:

| `Src/it.c` (CMSIS) | libopencm3 vector symbol |
|---|---|
| `USART0_IRQHandler`     | `usart1_isr`           |
| `TIMER0_UP_IRQHandler`  | `tim1_up_isr`          |
| `TIMER0_BRK_IRQHandler` | `tim1_brk_isr`         |
| `DMA_Channel0_IRQHandler` | `dma1_channel1_isr`  |
| `ADC_IRQHandler`        | `adc1_2_isr`           |

Look up the rest in `libopencm3/include/libopencm3/stm32/f1/nvic.h`
(or whichever header the fork provides for `gd32/f1x0`) — the
`NVIC_*_IRQ` numeric IDs and the `*_isr` weak symbols are defined
in lockstep.

Yes, calling GD's TIMER0/USART0/ADC by their STM32 names in the
firmware is awkward. We accept it: rewriting libopencm3's naming
to match GD's is more work than the port saves, and any per-name
shim layer (`#define USART0_IRQHandler usart1_isr`) is exactly
the SPL-shaped re-abstraction that guardrail #5 forbids. Treat
the STM32-style handler names as a libopencm3 convention we adopt,
not a vendor claim about the silicon.

The linker does **not** warn about misnamed handlers; the symptom
is "interrupt silently does nothing" at runtime. Catch it at port
time by renaming every handler when you touch `Src/it.c`, and at
done time per the cross-check in Done condition #3.

---

## Phase 3 — verify against done condition

For each function ported in Phase 2, run
`regtrace compare <peripheral>/<vector>` against the vector covering
it. Investigate every divergence: either the libopencm3 body in the
vector is wrong (fix it, then re-copy into `Src/setup.c`), or the
divergence is a deliberate behavioral difference we accept (record
the rationale in `~/dev/regtrace/decisions/<version>/<PERIPHERAL>.md`
per `decisions/TEMPLATE.md`; see the version/casing notes in the
regtrace tooling section).

---

## Regtrace divergences

When `regtrace compare` flags a divergence, check
`~/dev/regtrace/decisions/<version>/<peripheral>.md` first — known
divergences are recorded there with rationale. Unknown divergences
become new decision entries (template at
`~/dev/regtrace/decisions/TEMPLATE.md`). The decision log is the
authoritative record; this brief deliberately doesn't duplicate it.

The one exception worth pre-flagging here, because it's likely the
first Showstopper the implementer hits: F130's SPL baseline is
`__SYSTEM_CLOCK_72M_PLL_IRC8M_DIV2` (PLL = `IRC8M / 2 × 18` = 72 MHz),
and the multiplier 18 requires `PLLMF[4]` (RCU_CFG0 bit 27) — a high
bit STM32F1's PLLMUL field doesn't have. A `libopencm3/stm32f1`-
forwarder RCC will silently cap at 64 MHz and diverge on RCU_CFG0
bit 27. See `~/dev/regtrace/vectors/rcc/irc8m_pll_72mhz.yaml`. Do
not paper over with a 64 MHz fallback.

---

## Behavioral guardrails for the implementing agent

1. **"Compiles" is not done.** Done is the build green plus
   `regtrace compare` agreeing on every in-scope vector. A clean
   build with regtrace divergences is not done.

2. **Don't ask permission mid-port.** The plan is in this document.
   Don't ask "should I continue with `GPIO_init`?" after finishing
   `Clock_init`. The user has said "don't stop" repeatedly — that's a
   standing instruction.

3. **No LED demos. No toolchain validators. No separate test
   directories.** The firmware is a single artifact. Building a
   "minimal blink test" or a "toolchain sanity check" project is not
   progress on the port. If the toolchain is broken, the port build
   will tell you.

4. **No stub functions that link silently.** An empty `clock_init`
   links cleanly and traces as zeros — divergent in regtrace, but
   only after you've wasted a build cycle and possibly flashed a
   firmware that boots into nothing. With `lib/spl/` deleted, the
   "leave the SPL call in place" escape hatch is gone too. If a
   partial commit is unavoidable mid-port, the placeholder must be
   loud: `#error "clock_init not ported — see vector rcc/irc8m_pll_72mhz.yaml"`.
   The build fails at the unported function instead of silently
   producing a broken binary.

5. **No SPL-shaped shims.** No `HOVER_RCC_*` macros, no inline
   register writes that re-create an SPL function under a new name.
   If two families need different code, write two functions in two
   per-family files. Macro shims defeat the point of the port.

6. **Don't write "what this is NOT" sections in status updates.**
   Report what works, what's failing, and the next concrete action.
   Disclaimers about what isn't done yet are a tell that the work is
   under-done; if so, just say so.

7. **F130 only.** F103 and E230 are out of scope; do not add envs,
   per-family splits, or `gd32/f10x` library links for them.
