/* Linker script for GD32F130C8 (64 KiB Flash, 8 KiB SRAM).
 * Defines the chip's memory map and delegates the rest to libopencm3's
 * standard cortex-m-generic.ld (which sets up vectors, init/fini arrays,
 * data + bss sections, _stack symbol, etc). */

MEMORY
{
    rom (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 8K
}

INCLUDE cortex-m-generic.ld
