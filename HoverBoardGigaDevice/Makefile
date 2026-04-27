PROJECT  = firmware
BUILD    = build

LIBOPENCM3 = ../../libopencm3
LIBOPENCM3_ARCHIVE = $(LIBOPENCM3)/lib/libopencm3_gd32f1x0.a
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

.PHONY: all clean fork-check

all: fork-check $(BUILD)/$(PROJECT).bin

# Sanity-check the libopencm3 fork is built before we attempt to link.
# A pointed message saves the implementer chasing a generic linker error
# when the archive simply isn't there.
fork-check:
	@test -f $(LIBOPENCM3_ARCHIVE) || { \
	  echo "error: $(LIBOPENCM3_ARCHIVE) not found." ; \
	  echo "       build the fork first: cd $(LIBOPENCM3) && make TARGETS=gd32/f1x0" ; \
	  exit 1 ; }

$(BUILD):
	@mkdir -p $@

$(BUILD)/%.o: Src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/$(PROJECT).elf: $(OBJS) $(LIBOPENCM3_ARCHIVE) $(LDSCRIPT)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	$(SIZE) $@

$(BUILD)/$(PROJECT).bin: $(BUILD)/$(PROJECT).elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(BUILD)

-include $(DEPS)
