/**
  * This file is part of the hoverboard-sideboard-hack project.
  *
  * Copyright (C) 2020-2021 Emanuel FERU <aerdronix@gmail.com>
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
#include <stdio.h>
#include <string.h>
#include "gd32f1x0.h"
#include "defines.h"
#include "config.h"
#include "setup.h"
#include "i2c.h"
#include "mpu6050.h"
#include "target.h"

#ifdef SELF_BALANCING_ENABLE
// Optical sensors variables
static FlagStatus   sensor1, sensor2;           // holds the sensor1 and sensor 2 values
static FlagStatus   sensor1_read, sensor2_read; // holds the instantaneous Read for sensor1 and sensor 2

// MPU variables
extern MPU_Data     mpu;                        // holds the MPU-6050 data
#if defined(MPU_SENSOR_ENABLE) || defined(SERIAL_CONTROL)
static ErrStatus    mpuStatus;                  // holds the MPU-6050 status: SUCCESS or ERROR
#endif

extern uint32_t     main_loop_counter;          // main loop counter to perform task scheduling inside main()

// MAIN I2C variables
volatile int8_t     i2c_status;
volatile i2c_cmd    i2c_ReadWriteCmd;
volatile uint8_t    i2c_regAddress;
volatile uint8_t    i2c_slaveAddress;
volatile uint8_t*   i2c_txbuffer;
volatile uint8_t*   i2c_rxbuffer;
volatile uint8_t    i2c_nDABytes;
volatile  int8_t    i2c_nRABytes;
volatile uint8_t    buffer[14];

#ifdef AUX45_USE_I2C
// AUX I2C variables
volatile int8_t     i2c_aux_status;
volatile i2c_cmd    i2c_aux_ReadWriteCmd;
volatile uint8_t    i2c_aux_regAddress;
volatile uint8_t    i2c_aux_slaveAddress;
volatile uint8_t*   i2c_aux_txbuffer;
volatile uint8_t*   i2c_aux_rxbuffer;
volatile uint8_t    i2c_aux_nDABytes;
volatile  int8_t    i2c_aux_nRABytes;
#endif



/* =========================== General Functions =========================== */

void consoleLog(char *message)
{
  #ifdef SERIAL_DEBUG
    log_i("%s", message);
  #endif
}


uint8_t switch_check(uint16_t ch, uint8_t type) {
    if (type) { // 3 positions switch
        if      (ch < 250) return 0;    // switch in position 0
        else if (ch < 850) return 1;    // switch in position 1
        else               return 2;    // switch in position 2
    } else {    // 2 positions switch
        return  (ch > 850);
    }
}

/* =========================== Input Initialization Function =========================== */

void input_init(void) {
    #ifdef MPU_SENSOR_ENABLE
        if(mpu_config()) {                              // IMU MPU-6050 config
            mpuStatus = ERROR;
            digitalWrite(LED_RED,SET);
            digitalWrite(LED_GREEN,RESET);

          //  BUZZER_MorseSOS();
           // TODO gpio_bit_set(LED1_GPIO_Port, LED1_Pin);     // Turn on RED LED - sensor enabled and NOT ok
        }
        else {
            mpuStatus = SUCCESS;
            digitalWrite(LED_RED, RESET);
            digitalWrite(LED_GREEN,SET);
           // BUZZER_MorseSOS();
           // TODO gpio_bit_set(LED2_GPIO_Port, LED2_Pin);     // Turn on GREEN LED - sensor enabled and ok
        }
    #endif
}


/* =========================== Handle Functions =========================== */

/*
 * Handle of the MPU-6050 IMU sensor
 */
void handle_mpu6050(void) {
#ifdef MPU_SENSOR_ENABLE
    // Get MPU data. Because the MPU-6050 interrupt pin is not wired we have to check DMP data by pooling periodically
    if (SUCCESS == mpuStatus) {
        mpu_get_data();
    } else if (ERROR == mpuStatus && main_loop_counter % 100 == 0) {
      // TODO  toggle_led(LED1_GPIO_Port, LED1_Pin);                    // Toggle the Red LED every 100 ms
    }
    // Print MPU data to Console
    #ifdef SERIAL_DEBUG
    if (main_loop_counter % 50 == 0) {
        mpu_print_to_console();
    }
    #endif
#endif
}


/* =========================== I2C WRITE Functions =========================== */

//------------------------------------------------------------------------------
// Blocking write of N bytes to device @devAddr, register @regAddr.
//------------------------------------------------------------------------------
int8_t i2c_writeBytes(uint8_t devAddr,
                      uint8_t regAddr,
                      uint8_t length,
                      uint8_t *data)
{
    uint32_t tmo = 0;

    // 1) wait for bus idle
    while (i2c_flag_get(MPU_I2C, I2C_FLAG_I2CBSY)) {
        if (++tmo > I2C_TIMEOUT) return I2C_ERR;
    }

    // 2) send START
    i2c_start_on_bus(MPU_I2C);
    tmo = 0;
    while (!i2c_flag_get(MPU_I2C, I2C_FLAG_SBSEND)) {
        if (++tmo > I2C_TIMEOUT) return I2C_ERR;
    }

    // 3) send slave address + write bit
    //     — GD32 uses I2C_TRANSMITTER / I2C_RECEIVER
    i2c_master_addressing(MPU_I2C, devAddr << 1, I2C_TRANSMITTER);
    tmo = 0;
    while (!i2c_flag_get(MPU_I2C, I2C_FLAG_ADDSEND)) {
        if (++tmo > I2C_TIMEOUT) {
            i2c_stop_on_bus(MPU_I2C);
            return I2C_ERR;
        }
    }
    // clear the address‐sent flag
    i2c_flag_clear(MPU_I2C, I2C_FLAG_ADDSEND);

    // 4) send register address
    i2c_data_transmit(MPU_I2C, regAddr);
    tmo = 0;
    while (!i2c_flag_get(MPU_I2C, I2C_FLAG_TBE)) {
        if (++tmo > I2C_TIMEOUT) {
            i2c_stop_on_bus(MPU_I2C);
            return I2C_ERR;
        }
    }

    // 5) send payload bytes
    for (uint8_t i = 0; i < length; i++) {
        i2c_data_transmit(MPU_I2C, data[i]);
        tmo = 0;
        while (!i2c_flag_get(MPU_I2C, I2C_FLAG_TBE)) {
            if (++tmo > I2C_TIMEOUT) {
                i2c_stop_on_bus(MPU_I2C);
                return I2C_ERR;
            }
        }
    }

    // 6) wait for transfer complete, then STOP
    tmo = 0;
    while (!i2c_flag_get(MPU_I2C, I2C_FLAG_BTC)) {
        if (++tmo > I2C_TIMEOUT) {
            i2c_stop_on_bus(MPU_I2C);
            return I2C_ERR;
        }
    }
    i2c_stop_on_bus(MPU_I2C);

    return I2C_OK;
}


/*
 * write 1 byte to chip register
 */
int8_t i2c_writeByte(uint8_t slaveAddr, uint8_t regAddr, uint8_t data)
{
    return i2c_writeBytes(slaveAddr, regAddr, 1, &data);
}


/*
 * write one bit to chip register
 */
int8_t i2c_writeBit(uint8_t slaveAddr, uint8_t regAddr, uint8_t bitNum, uint8_t data) {
    uint8_t b;
    i2c_readByte(slaveAddr, regAddr, &b);
    b = (data != 0) ? (b | (1 << bitNum)) : (b & ~(1 << bitNum));
    return i2c_writeByte(slaveAddr, regAddr, b);
}



/* =========================== I2C READ Functions =========================== */

//------------------------------------------------------------------------------
// Blocking read of N bytes into data[] from device @devAddr, register @regAddr.
//------------------------------------------------------------------------------
int8_t i2c_readBytes(uint8_t devAddr,
                     uint8_t regAddr,
                     uint8_t length,
                     uint8_t *data)
{
    uint32_t tmo = 0;

    // 1) wait for bus idle
    while (i2c_flag_get(MPU_I2C, I2C_FLAG_I2CBSY)) {
        if (++tmo > I2C_TIMEOUT) return I2C_ERR;
    }

    // 2) send START + slave addr (write) + regAddr
    i2c_start_on_bus(MPU_I2C);
    tmo = 0;
    while (!i2c_flag_get(MPU_I2C, I2C_FLAG_SBSEND)) {
        if (++tmo > I2C_TIMEOUT) return I2C_ERR;
    }

    i2c_master_addressing(MPU_I2C, devAddr << 1, I2C_TRANSMITTER);
    tmo = 0;
    while (!i2c_flag_get(MPU_I2C, I2C_FLAG_ADDSEND)) {
        if (++tmo > I2C_TIMEOUT) {
            i2c_stop_on_bus(MPU_I2C);
            return I2C_ERR;
        }
    }
    i2c_flag_clear(MPU_I2C, I2C_FLAG_ADDSEND);

    i2c_data_transmit(MPU_I2C, regAddr);
    tmo = 0;
    while (!i2c_flag_get(MPU_I2C, I2C_FLAG_TBE)) {
        if (++tmo > I2C_TIMEOUT) {
            i2c_stop_on_bus(MPU_I2C);
            return I2C_ERR;
        }
    }

    // 3) repeated-start for read
    i2c_start_on_bus(MPU_I2C);
    tmo = 0;
    while (!i2c_flag_get(MPU_I2C, I2C_FLAG_SBSEND)) {
        if (++tmo > I2C_TIMEOUT) return I2C_ERR;
    }

    i2c_master_addressing(MPU_I2C, devAddr << 1, I2C_RECEIVER);
    tmo = 0;
    while (!i2c_flag_get(MPU_I2C, I2C_FLAG_ADDSEND)) {
        if (++tmo > I2C_TIMEOUT) {
            i2c_stop_on_bus(MPU_I2C);
            return I2C_ERR;
        }
    }
    i2c_flag_clear(MPU_I2C, I2C_FLAG_ADDSEND);

    // 4) read each byte
    for (uint8_t i = 0; i < length; i++) {
        // on last byte, disable ACK and send STOP
        if (i == length - 1) {
            i2c_ack_config(MPU_I2C, I2C_ACK_DISABLE);
            i2c_stop_on_bus(MPU_I2C);
        }
        tmo = 0;
        while (!i2c_flag_get(MPU_I2C, I2C_FLAG_RBNE)) {
            if (++tmo > I2C_TIMEOUT) return I2C_ERR;
        }
        data[i] = i2c_data_receive(MPU_I2C);
    }

    // re-enable ACK for next time
    i2c_ack_config(MPU_I2C, I2C_ACK_ENABLE);

    return I2C_OK;
}


/*
 * read 1 byte from chip register
 */
int8_t i2c_readByte(uint8_t slaveAddr, uint8_t regAddr, uint8_t *data)
{
    return i2c_readBytes(slaveAddr, regAddr, 1, data);
}


/*
 * read 1 bit from chip register
 */
int8_t i2c_readBit(uint8_t slaveAddr, uint8_t regAddr, uint8_t bitNum, uint8_t *data)
{
    uint8_t b;
    int8_t status = i2c_readByte(slaveAddr, regAddr, &b);
    *data = b & (1 << bitNum);
    return status;
}

#endif // SELF_BALANCING_ENABLE