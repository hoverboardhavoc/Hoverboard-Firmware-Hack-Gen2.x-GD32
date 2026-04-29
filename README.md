# Hoverboard Firmware (libopencm3 port)

A proof-of-concept port of [RoboDurden/Hoverboard-Firmware-Hack-Gen2.x-GD32](https://github.com/RoboDurden/Hoverboard-Firmware-Hack-Gen2.x-GD32)
that drops the GD32 SPL HAL + Keil/IAR scaffolding and builds against
[libopencm3](https://github.com/libopencm3/libopencm3) instead. Two
build paths are wired up: a small hand-written `Makefile` and a
[meson](https://mesonbuild.com/) `meson.build`. Both produce identical
firmware images.

## Status

This is a **proof of concept**. It runs on a single hoverboard layout
(`LAYOUT 20`, the only one on the bench used to validate this), in
block-commutation mode (`BLDC_BC`).

| Target       | Status                                    |
|--------------|-------------------------------------------|
| GD32F130C8   | ✅ Tested on real hardware                |
| GD32F103     | ➖ Should be an easy win — see below      |
| STM32F103    | ➖ Should be an easy win — see below      |
| Other GD32   | ➖ Untested                                |

The GD32F130 path was the bring-up target. GD32F103 and STM32F103
share the same Cortex-M3 + STM32F1-shape peripherals, so the same
build setup should drop in with two changes:

1. Add the relevant family to libopencm3's meson dispatch — the patch
   in our libopencm3 fork's commit `406dd9fa` shows the pattern
   (~30 lines, mostly mirroring the existing `lib/stm32/f1/`
   meson.build for the GD32 case, or just selecting `stm32f1` for
   the STM32F103 case which is already supported).
2. Swap the Makefile / meson.build's `cpu_flags`, `DEFS`
   (`GD32F1X0`/`GD32F130` → `GD32F1`/`GD32F103` or `STM32F1`),
   linker-script memory layout (`link_gd32f130.ld` → equivalent for
   the new chip), and `lib/stm32/f1` build target.

Most peripheral access in `setup.c` is already register-accurate
against libopencm3 names, so few code changes should be needed.

## Build (Make)

```
git submodule update --init --recursive
cd HoverBoardGigaDevice
make            # → build/firmware.bin
make flash      # via openocd; honours $OPENOCD env var
```

## Build (meson)

```
git submodule update --init --recursive
cd HoverBoardGigaDevice
meson setup build --cross-file cross-arm-none-eabi.txt
ninja -C build              # → build/firmware.bin
ninja -C build flash        # same flash flow as make
```

## Toolchain

* `arm-none-eabi-gcc` (tested with 10.3-2021.10)
* `openocd` (any recent build, `0.12.x` recommended; **not** the
  `tool-openocd-gd32` shipped with PlatformIO — that build silently
  mis-programs flash on the GD32F130)
* `meson` + `ninja` if you take the meson path

## Layout / pin map

The active layout is selected via `Inc/config.h` (`LAYOUT 20`). Pin
maps live in `Inc/defines/defines_2-1-*.h` — adapting to a different
layout is just selecting the right header and matching the bench's
phase-current / battery-sense / hall pinout to its GPIO assignments.

## What's not in this branch

The original repo's [`foc`](https://github.com/RoboDurden/Hoverboard-Firmware-Hack-Gen2.x-GD32/tree/foc)
branch's FOC implementation is **not** included here. This branch is
deliberately scoped to the libopencm3 port; FOC, RTT plotting, and
bench-tuning notes live elsewhere. Block commutation (`BLDC_BC`) and
sine modulation (`BLDC_SINE`) compile and run.

## Upstream

For pre-built layouts, hardware photos, and the full SPL/Keil build
chain, see [RoboDurden/Hoverboard-Firmware-Hack-Gen2.x-GD32](https://github.com/RoboDurden/Hoverboard-Firmware-Hack-Gen2.x-GD32).
