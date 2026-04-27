/*
 * Hoverboard libopencm3 port — led.c (API surface only)
 *
 * The original led.c is 397 lines of LED state-machine logic (battery
 * level display, blink patterns, master/slave coordination). The actual
 * peripheral access is just gpio_set / gpio_clear / gpio_toggle on a
 * handful of pins.
 *
 * Stubs for now so it.c and main.c can link.
 */

#include <libopencm3/stm32/gpio.h>
#include <stdint.h>

void CalculateLEDProgram(void) { /* TODO */ }
void SetUpperLEDMaster(int state) { (void)state; }
void SetLowerLEDMaster(int state) { (void)state; }
void ShowBatteryState(int8_t iLevel) { (void)iLevel; }
