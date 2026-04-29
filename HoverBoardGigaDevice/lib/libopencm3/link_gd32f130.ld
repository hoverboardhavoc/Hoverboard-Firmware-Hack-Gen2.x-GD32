/* GD32F130C8: 64K flash, 8K RAM */
MEMORY {
    rom (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 8K
}
INCLUDE cortex-m-generic.ld
