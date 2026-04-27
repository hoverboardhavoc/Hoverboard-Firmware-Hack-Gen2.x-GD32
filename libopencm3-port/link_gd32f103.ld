/* Linker script for GD32F103C8 (64 KiB Flash, 20 KiB SRAM).
 * Same template as link_gd32f130.ld with a different SRAM size. */

MEMORY
{
    rom (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

INCLUDE cortex-m-generic.ld
