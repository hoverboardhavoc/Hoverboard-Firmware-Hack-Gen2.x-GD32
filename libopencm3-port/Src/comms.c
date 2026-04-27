/*
 * Hoverboard libopencm3 port — comms.c
 *
 * Two helpers used across the master/slave/steering comms protocols.
 * The CRC is libopencm3-agnostic (pure arithmetic). SendBuffer
 * translates from gd-spl's usart_data_transmit + usart_flag_get to
 * libopencm3's usart_send_blocking which does both in one call.
 */

#include <libopencm3/stm32/usart.h>
#include <stdint.h>

void SendBuffer(uint32_t usart_periph, uint8_t buffer[], uint8_t length)
{
    for (uint8_t i = 0; i < length; i++) {
        usart_send_blocking(usart_periph, buffer[i]);
    }
}

uint16_t CalcCRC(uint8_t *ptr, int count)
{
    uint16_t crc = 0;
    while (--count >= 0) {
        crc ^= ((uint16_t)*ptr++) << 8;
        for (uint8_t i = 0; i < 8; i++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}
