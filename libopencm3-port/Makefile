# Hoverboard libopencm3 port — proof-of-concept Makefile.
#
# Builds a minimal blink-and-watchdog firmware for either GD32F103 or
# GD32F130 against the libopencm3 fork at ../../libopencm3.
#
# Usage:
#   make TARGET=gd32f10x   # for GD32F103 (legacy v1 architecture)
#   make TARGET=gd32f1x0   # for GD32F130 (modern v2 architecture)
#
# This is the smallest demonstrable build harness for the libopencm3
# port. setup.c, main.c, the bldc/comms/imu source files etc. are NOT
# yet ported — see PORT.md for the workflow on extending this.

TARGET ?= gd32f10x

ifeq ($(TARGET),gd32f10x)
    OPENCM3_TARGET := gd32/f10x
    LIB_NAME       := opencm3_gd32f10x
    CHIP_DEFINE    := GD32F10X
    CPU_FLAGS      := -mcpu=cortex-m3 -mthumb
    LDSCRIPT       := link_gd32f103.ld
else ifeq ($(TARGET),gd32f1x0)
    OPENCM3_TARGET := gd32/f1x0
    LIB_NAME       := opencm3_gd32f1x0
    CHIP_DEFINE    := GD32F1X0
    CPU_FLAGS      := -mcpu=cortex-m3 -mthumb
    LDSCRIPT       := link_gd32f130.ld
else
    $(error Unknown TARGET '$(TARGET)'. Use gd32f10x or gd32f1x0.)
endif

OPENCM3_DIR ?= $(abspath ../../libopencm3)
PREFIX      ?= arm-none-eabi-
CC          := $(PREFIX)gcc
LD          := $(PREFIX)gcc
OBJCOPY     := $(PREFIX)objcopy
SIZE        := $(PREFIX)size

CFLAGS  := -Os -Wall -Wextra -ffunction-sections -fdata-sections \
           -fno-common -ffreestanding -fno-builtin \
           $(CPU_FLAGS) -D$(CHIP_DEFINE) \
           -I$(OPENCM3_DIR)/include -I.
LDFLAGS := $(CPU_FLAGS) -nostartfiles -L$(OPENCM3_DIR)/lib -T$(LDSCRIPT) \
           -Wl,--gc-sections -specs=nosys.specs

SRCS := main_blink.c
OBJS := $(SRCS:.c=.o)
BIN  := blink_$(TARGET)

all: $(BIN).elf $(BIN).bin

$(BIN).elf: $(OBJS) $(OPENCM3_DIR)/lib/lib$(LIB_NAME).a
	$(LD) $(LDFLAGS) -o $@ $(OBJS) -l$(LIB_NAME)
	$(SIZE) $@

%.bin: %.elf
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o *.elf *.bin

.PHONY: all clean
