PROJECT  = firmware
BUILD    = build

LIBOPENCM3 = libopencm3
LIBOPENCM3_ARCHIVE = $(LIBOPENCM3)/lib/libopencm3_gd32f1x0.a
LIBOPENCM3_TARGET = gd32/f1x0
LDSCRIPT = lib/libopencm3/link_gd32f130.ld

CROSS    = arm-none-eabi-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
SIZE     = $(CROSS)size

CPUFLAGS = -mcpu=cortex-m3 -mthumb
# GD32F1X0 selects the fork's gd32/f1x0 family in libopencm3's stm32/<peripheral>.h
# dispatchers — those pull in stm32/f0/<peripheral>.h for v2 GPIO/ADC/etc. Don't
# also define STM32F1 here: that's v1 GPIO/etc. and would win the dispatcher
# #elif chain over GD32F1X0, routing to the wrong family.
# TARGET=1 selects the GD32F130 path through Inc/target.h and the active
# Inc/defines/defines_2-1-${LAYOUT}.h pin map.
DEFS     = -D GD32F1X0 -D GD32F130 -D TARGET=1
WARNINGS = -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
           -Wno-unused-variable -Wno-unused-but-set-variable \
           -Wno-missing-field-initializers
INCLUDES = -I Inc -I $(LIBOPENCM3)/include
OPT      = -Os -ffunction-sections -fdata-sections -fno-common
CFLAGS   = $(CPUFLAGS) $(DEFS) $(WARNINGS) $(INCLUDES) $(OPT) -std=gnu11 -g3
LDFLAGS  = $(CPUFLAGS) -nostartfiles \
           -L $(LIBOPENCM3)/lib -T $(LDSCRIPT) \
           -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(PROJECT).map \
           --specs=nosys.specs --specs=nano.specs
LIBS     = -l:libopencm3_gd32f1x0.a

SRCS := $(wildcard Src/*.c)
OBJS := $(patsubst Src/%.c,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# OpenOCD config for flashing.
#   tool-openocd (generic) — works correctly on GD32F130.
# Override on the command line if openocd isn't on PATH or you want to use
# a specific build, e.g. `make flash OPENOCD=$HOME/.platformio/packages/tool-openocd/bin/openocd`.
# Do NOT point this at PlatformIO's `tool-openocd-gd32` — it silently mis-
# programs flash on GD32F130 (vector-table bytes diverge from the .bin
# even though "Verify" reports OK).
OPENOCD      ?= openocd
OPENOCD_FLAGS = -f interface/stlink.cfg \
                -c "transport select hla_swd" -f target/stm32f1x.cfg

.PHONY: all clean flash

all: $(BUILD)/$(PROJECT).bin

flash: $(BUILD)/$(PROJECT).elf
	@pkill -f openocd 2>/dev/null; sleep 0.3; \
	$(OPENOCD) $(OPENOCD_FLAGS) -c "program $< verify reset exit"

# Build the libopencm3 archive on demand. The archive is a real prereq of
# the firmware elf so this fires automatically on first build.
$(LIBOPENCM3_ARCHIVE):
	@test -f $(LIBOPENCM3)/Makefile || { \
	  echo "error: libopencm3 submodule not initialized." ; \
	  echo "       run: git submodule update --init --recursive" ; \
	  exit 1 ; }
	$(MAKE) -C $(LIBOPENCM3) TARGETS=$(LIBOPENCM3_TARGET)

$(BUILD):
	@mkdir -p $@

$(BUILD)/%.o: Src/%.c | $(BUILD) $(LIBOPENCM3_ARCHIVE)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/$(PROJECT).elf: $(OBJS) $(LIBOPENCM3_ARCHIVE) $(LDSCRIPT)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	$(SIZE) $@

$(BUILD)/$(PROJECT).bin: $(BUILD)/$(PROJECT).elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(BUILD)

-include $(DEPS)
