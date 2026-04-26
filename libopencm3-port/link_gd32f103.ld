/* Minimal linker script for GD32F103C8 (64 KiB Flash, 20 KiB SRAM). */

ENTRY(main)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 64K
    SRAM  (rwx): ORIGIN = 0x20000000, LENGTH = 20K
}

_estack = ORIGIN(SRAM) + LENGTH(SRAM);

SECTIONS
{
    .vectors : { KEEP(*(.vectors)) } > FLASH
    .text    : { *(.text*) *(.rodata*) } > FLASH
    .data    : { _sdata = .; *(.data*) _edata = .; } > SRAM AT > FLASH
    .bss     : { _sbss = .; *(.bss*) *(COMMON) _ebss = .; } > SRAM
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) }
}
