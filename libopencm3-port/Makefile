# Hoverboard libopencm3 port — build harness
#
# Currently only setup.c is partially ported. The Makefile compiles each
# .c file under Src/ individually (no link yet) so we can validate the
# include chain + API translations as we go.
#
# Usage:
#   make TARGET=gd32f10x       # GD32F103
#   make TARGET=gd32f1x0       # GD32F130

TARGET ?= gd32f1x0

ifeq ($(TARGET),gd32f10x)
    LIB_NAME    := opencm3_gd32f10x
    CHIP_DEFINE := GD32F10X
    CPU_FLAGS   := -mcpu=cortex-m3 -mthumb
else ifeq ($(TARGET),gd32f1x0)
    LIB_NAME    := opencm3_gd32f1x0
    CHIP_DEFINE := GD32F1X0
    CPU_FLAGS   := -mcpu=cortex-m3 -mthumb
else
    $(error Unknown TARGET '$(TARGET)'. Use gd32f10x or gd32f1x0.)
endif

OPENCM3_DIR ?= $(abspath ../../libopencm3)
PREFIX      ?= arm-none-eabi-
CC          := $(PREFIX)gcc

CFLAGS  := -Os -Wall -Wextra -ffunction-sections -fdata-sections \
           -fno-common -ffreestanding -fno-builtin \
           $(CPU_FLAGS) -D$(CHIP_DEFINE) \
           -I$(OPENCM3_DIR)/include -IInc

# Per-file compile (no link until enough is ported to have a coherent
# whole). Each ported .c gets a .o; building all .o validates the
# include + API translation work.
SRCS := Src/setup.c Src/it.c Src/main.c
OBJS := $(SRCS:.c=.o)

all: $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS)

.PHONY: all clean
