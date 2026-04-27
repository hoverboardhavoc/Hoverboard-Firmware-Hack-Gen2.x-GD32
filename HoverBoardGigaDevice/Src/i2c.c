/**
  * This file was part of the hoverboard-sideboard-hack project but got re-written
  * to remove the interrupts and use a blocking I2C implementation.
  *
  * Copyright (C) 2020-2021 Emanuel FERU <aerdronix@gmail.com>
  * Copyright (C) 2025 Hoverboard Havoc
  *
  * This program is free software: you can redistribute it and/or modify
  * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation, either version 3 of the License, or
  * (at your option) any later version.
  *
  * This program is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  * GNU General Public License for more details.
  *
  * You should have received a copy of the GNU General Public License
  * along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Includes
#include "../Inc/defines.h"
#include "../Inc/it.h"	// for Delay in dump_i2c_registers(uint8_t slaveAddr)
#include "../Inc/i2c.h"

#ifdef I2C_ENABLE

/* GD I2C0 = libopencm3 I2C1 (APB1[21]). 100 kHz standard-mode I2C on
 * PB6/PB7 (or PB8/PB9 if I2C_PB8PB9 selected) with AF1, open-drain,
 * pull-up enabled. */
void I2C_Init(void)
{
	rcc_periph_clock_enable(RCC_GPIOB);
	rcc_periph_clock_enable(MPU_RCU_I2C);

	#ifdef I2C_PB6PB7
		gpio_mode_setup(GPIOB, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO6 | GPIO7);
		gpio_set_output_options(GPIOB, GPIO_OTYPE_OD, GPIO_OSPEED_HIGH, GPIO6 | GPIO7);
		gpio_set_af(GPIOB, GPIO_AF1, GPIO6 | GPIO7);
	#else
		gpio_mode_setup(GPIOB, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO8 | GPIO9);
		gpio_set_output_options(GPIOB, GPIO_OTYPE_OD, GPIO_OSPEED_HIGH, GPIO8 | GPIO9);
		gpio_set_af(GPIOB, GPIO_AF1, GPIO8 | GPIO9);
	#endif

	/* Software-reset the peripheral via the RCC reset pulse, then disable
	 * the peripheral so subsequent CCR/TRISE/FREQ writes take effect
	 * (those registers are write-only when CR1.PE=0). */
	rcc_periph_reset_pulse(RST_I2C1);
	i2c_peripheral_disable(I2C1);

	/* APB1 = 36 MHz post-clock_init. Set FREQ field, then CCR for 100 kHz
	 * standard-mode (Tlow = Thigh = 5 µs → CCR = 5e-6 / (1/36e6) = 180),
	 * then TRISE = (FREQ + 1) for SM. */
	i2c_set_clock_frequency(I2C1, 36);
	i2c_set_ccr(I2C1, 180);
	i2c_set_trise(I2C1, 37);
	i2c_set_own_7bit_slave_address(I2C1, I2C_OWN_ADDRESS7);

	i2c_peripheral_enable(I2C1);
	i2c_enable_ack(I2C1);
}

void i2c_hardReset(uint32_t i2c_periph)
{
	/* I2C_CR1.SWRST=1 holds the peripheral in reset (clears all
	 * SR1/SR2/CR1/CR2 fields except SWRST itself), SWRST=0 releases.
	 * Same effect as the SPL i2c_software_reset_config(SET/RESET) pair.
	 * Followed by I2C_Init to reload the peripheral config. */
	I2C_CR1(i2c_periph) |= I2C_CR1_SWRST;
	for (volatile int d = 0; d < 1000; d++);
	I2C_CR1(i2c_periph) &= ~I2C_CR1_SWRST;
	I2C_Init();
}

//------------------------------------------------------------------------------
// Blocking write of N bytes to device @devAddr, register @regAddr.
//------------------------------------------------------------------------------
int8_t i2c_writeBytes(
                      uint32_t i2c_periph,
                      uint8_t devAddr,
                      uint8_t regAddr,
                      uint8_t length,
                      uint8_t *data)
{
    uint32_t tmo = 0;

    // 1) wait for bus idle
    while ((I2C_SR2(i2c_periph) & I2C_SR2_BUSY)) {
        if (++tmo > I2C_TIMEOUT) return I2C_ERR;
    }

    // 2) send START
    i2c_send_start(i2c_periph);
    tmo = 0;
    while (!(I2C_SR1(i2c_periph) & I2C_SR1_SB)) {
        if (++tmo > I2C_TIMEOUT) return I2C_ERR;
    }

    // 3) send slave address + write bit
    //     — GD32 uses I2C_TRANSMITTER / I2C_RECEIVER
    i2c_send_7bit_address(i2c_periph, devAddr, I2C_WRITE);
    tmo = 0;
    while (!(I2C_SR1(i2c_periph) & I2C_SR1_ADDR)) {
        if (++tmo > I2C_TIMEOUT) {
            i2c_send_stop(i2c_periph);
            return I2C_ERR;
        }
    }
    // clear the address‐sent flag
    (void)I2C_SR1(i2c_periph); (void)I2C_SR2(i2c_periph);

    // 4) send register address
    i2c_send_data(i2c_periph, regAddr);
    tmo = 0;
    while (!(I2C_SR1(i2c_periph) & I2C_SR1_TxE)) {
        if (++tmo > I2C_TIMEOUT) {
            i2c_send_stop(i2c_periph);
            return I2C_ERR;
        }
    }

    // 5) send payload bytes
    for (uint8_t i = 0; i < length; i++) {
        i2c_send_data(i2c_periph, data[i]);
        tmo = 0;
        while (!(I2C_SR1(i2c_periph) & I2C_SR1_TxE)) {
            if (++tmo > I2C_TIMEOUT) {
                i2c_send_stop(i2c_periph);
                return I2C_ERR;
            }
        }
    }

    // 6) wait for transfer complete, then STOP
    tmo = 0;
    while (!(I2C_SR1(i2c_periph) & I2C_SR1_BTF)) {
        if (++tmo > I2C_TIMEOUT) {
            i2c_send_stop(i2c_periph);
            return I2C_ERR;
        }
    }
    i2c_send_stop(i2c_periph);

    return I2C_OK;
}


/*
 * write 1 byte to chip register
 */
int8_t i2c_writeByte(uint32_t i2c_periph, uint8_t slaveAddr, uint8_t regAddr, uint8_t data)
{
    return i2c_writeBytes(i2c_periph, slaveAddr, regAddr, 1, &data);
}


/*
 * write one bit to chip register
 */
int8_t i2c_writeBit(uint32_t i2c_periph, uint8_t slaveAddr, uint8_t regAddr, uint8_t bitNum, uint8_t data) {
    uint8_t b;
    i2c_readByte(i2c_periph, slaveAddr, regAddr, &b);
    b = (data != 0) ? (b | (1 << bitNum)) : (b & ~(1 << bitNum));
    return i2c_writeByte(i2c_periph, slaveAddr, regAddr, b);
}



/* =========================== I2C READ Functions =========================== */

int16_t i2cReadErrors = 0;     // Debug counter
int16_t i2cReadAddrErrors = 0; // Debug counter
int16_t i2cReadTimeout = 0;    // Debug counter

//------------------------------------------------------------------------------
// Blocking read of N bytes into data[] from device @devAddr, register @regAddr,
// with retries on read failure for @iTimeoutMillis ms (can happen due to noisy i2c bus)
//------------------------------------------------------------------------------
int8_t i2c_readBytesTimeout(uint16_t iTimeoutMillis, uint32_t i2c_periph, uint8_t slaveAddr, uint8_t regAddr, uint8_t length, uint8_t *data)
{
	uint32_t iTimeRetry = millis() + iTimeoutMillis;
	do
	{
		if (I2C_OK == i2c_readBytes(i2c_periph, slaveAddr, regAddr, length, data))	return I2C_OK;
		i2cReadErrors++;
		i2c_hardReset(i2c_periph);
	}while (millis() < iTimeRetry);
	i2cReadTimeout++;
	return I2C_ERR;
}

//------------------------------------------------------------------------------
// Blocking read of N bytes into data[] from device @devAddr, register @regAddr.
//------------------------------------------------------------------------------
int8_t i2c_readBytes(uint32_t i2c_periph,
                     uint8_t devAddr,
                     uint8_t regAddr,
                     uint8_t length,
                     uint8_t *data)
{
    uint32_t tmo = 0;

    // 1) wait for bus idle
    while ((I2C_SR2(i2c_periph) & I2C_SR2_BUSY)) {
        if (++tmo > I2C_TIMEOUT) return I2C_ERR;
    }

    // 2) send START + slave addr (write) + regAddr
    i2c_send_start(i2c_periph);
    tmo = 0;
    while (!(I2C_SR1(i2c_periph) & I2C_SR1_SB)) {
        if (++tmo > I2C_TIMEOUT) return I2C_ERR;
    }

    i2c_send_7bit_address(i2c_periph, devAddr, I2C_WRITE);
    tmo = 0;
    while (!(I2C_SR1(i2c_periph) & I2C_SR1_ADDR)) {
        if (++tmo > I2C_TIMEOUT) {
            i2c_send_stop(i2c_periph);
            i2cReadAddrErrors++;
            return I2C_ERR;
        }
    }
    (void)I2C_SR1(i2c_periph); (void)I2C_SR2(i2c_periph);

    i2c_send_data(i2c_periph, regAddr);
    tmo = 0;
    while (!(I2C_SR1(i2c_periph) & I2C_SR1_TxE)) {
        if (++tmo > I2C_TIMEOUT) {
            i2c_send_stop(i2c_periph);
            return I2C_ERR;
        }
    }

    // 3) repeated-start for read
    i2c_send_start(i2c_periph);
    tmo = 0;
    while (!(I2C_SR1(i2c_periph) & I2C_SR1_SB)) {
        if (++tmo > I2C_TIMEOUT) return I2C_ERR;
    }

    i2c_send_7bit_address(i2c_periph, devAddr, I2C_READ);
    tmo = 0;
    while (!(I2C_SR1(i2c_periph) & I2C_SR1_ADDR)) {
        if (++tmo > I2C_TIMEOUT) {
            i2c_send_stop(i2c_periph);
            return I2C_ERR;
        }
    }
    (void)I2C_SR1(i2c_periph); (void)I2C_SR2(i2c_periph);

    // 4) read each byte
    for (uint8_t i = 0; i < length; i++) {
        // on last byte, disable ACK and send STOP
        if (i == length - 1) {
            i2c_disable_ack(i2c_periph);
            i2c_send_stop(i2c_periph);
        }
        tmo = 0;
        while (!(I2C_SR1(i2c_periph) & I2C_SR1_RxNE)) {
            if (++tmo > I2C_TIMEOUT) return I2C_ERR;
        }
        data[i] = i2c_get_data(i2c_periph);
    }

    // re-enable ACK for next time
    i2c_enable_ack(i2c_periph);

    return I2C_OK;
}


/*
 * read 1 byte from chip register
 */
int8_t i2c_readByte(uint32_t i2c_periph, uint8_t slaveAddr, uint8_t regAddr, uint8_t *data)
{
    return i2c_readBytes(i2c_periph, slaveAddr, regAddr, 1, data);
}


/*
 * read 1 bit from chip register
 */
int8_t i2c_readBit(uint32_t i2c_periph, uint8_t slaveAddr, uint8_t regAddr, uint8_t bitNum, uint8_t *data)
{
    uint8_t b;
    int8_t status = i2c_readByte(i2c_periph, slaveAddr, regAddr, &b);
    *data = b & (1 << bitNum);
    return status;
}


int8_t iFound = -1;
int8_t aiFound[10] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};		// room for 10 defices found. Monitor with StmStudio
uint8_t i2c_scanner(void) 
{
	iFound = 0;
	for (uint8_t addr = 0x08; addr <= 0x77; addr++) 
	{
		int8_t result = i2c_writeByte(I2C_PERIPH, addr, 0x00, 0x00);	// Try to write a dummy byte to the device
		if (result == 0) // Success means device ACK'd
		{
            if (iFound <10) aiFound[iFound] = addr;
            iFound++;
            RTT_PRINTF2(64,"\n%i found at: 0x%02X\n",iFound,addr)
		}
		for (volatile int i = 0; i < 1000; i++);	// Short delay between probes (adjust based on system clock)
	}
    RTT_PRINTF(64,"I2c scan complete. Found: %i\n",iFound)
	return iFound>0 ? aiFound[(iFound-1)] : 0;	//printf("Scan complete. Found %d device(s).\r\n", found);
}

#define MAX_REGISTERS 0x88
int16_t aiDump[MAX_REGISTERS];	// monitor with StmStudio
void dump_i2c_registers(uint8_t slaveAddr) 
{
	uint8_t data;
	int8_t result;
	for (uint8_t regAddr= 0; regAddr< MAX_REGISTERS; regAddr++) 
	{
		result = i2c_readByte(I2C_PERIPH, slaveAddr, regAddr, &data);
		aiDump[regAddr] = (result == 0) ? data : -1;
        //RTT_PRINTF2(32,"%02X = %02X\n",regAddr,aiDump[regAddr])
        RTT_PRINTF2(32,((regAddr%16<15) ? "%02X=%02X  " : "%02X: %02X\n"),regAddr,aiDump[regAddr])
		Delay(1);
	}
}

#endif // I2C_ENABLE