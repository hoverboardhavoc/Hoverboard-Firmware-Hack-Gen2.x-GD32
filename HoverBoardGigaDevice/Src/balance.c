#include "balance.h"
#include "MadgwickAHRS.h"
#include <math.h>
#include <stdint.h>
#include "string.h"

#include "defines.h"
#include "pid.h"
#include "bldc.h"
#include "comms.h"

#ifdef SELF_BALANCING_ENABLE

PID pid_balance;

// in initialization (e.g. main or board_init):


// scales
#define GYRO_SCALE   (1.0f/131.0f)    // LSB → °/s
#define ACCEL_SCALE  (1.0f/16384.0f)  // LSB → g
#define RAD_TO_DEG   (57.295779513f)  // rad→deg

// state & calibration
// MadgwickAHRSupdate() keeps its own static quaternion internally
static int16_t gyro_off_x = 0, gyro_off_y = 0, gyro_off_z = 0;
static int16_t accel_off_x = 0, accel_off_y = 0, accel_off_z = 0;

extern MPU_Data mpu;
//extern PID      pid_balance;
extern float    torque_command;

extern volatile float  q0, q1, q2, q3;

void balance_init() {
    PID_Init(&pid_balance,
            35.0f,   // Kp
            4.0f,    // Ki
            0.5f,    // Kd
            -1000.0f,// out_min
            +1000.0f // out_max
    );
}

int count = 0;

char sMessage[500]; // for debug output
// call this at a steady ~200 Hz from your scheduler or main loop
void balance_update(void) {
    // 1) read & scale sensor data
    //    gyro: deg/s → rad/s
    float gx = (mpu.gyro.x  - gyro_off_x)  * GYRO_SCALE * (M_PI/180.0f);
    float gy = (mpu.gyro.y  - gyro_off_y)  * GYRO_SCALE * (M_PI/180.0f);
    float gz = (mpu.gyro.z  - gyro_off_z)  * GYRO_SCALE * (M_PI/180.0f);

    //    accel: raw → g
    float ax = (mpu.accel.x - accel_off_x) * ACCEL_SCALE;
    float ay = (mpu.accel.y - accel_off_y) * ACCEL_SCALE;
    float az = (mpu.accel.z - accel_off_z) * ACCEL_SCALE;

    // 2) Madgwick fusion (no mag → pass zeros)
    //    signature: MadgwickAHRSupdate(gx,gy,gz, ax,ay,az, mx,my,mz);
   // 2) fuse — IMU only (no magnetometer)
    MadgwickAHRSupdateIMU(gx, gy, gz, ax, ay, az);

    // 3) extract pitch from the internal quaternion
    //    quaternion is held as static q0,q1,q2,q3 inside the library
    //    and exposed as extern floats (usually named q0..q3)
    float pitch = asinf(-2.0f*(q1*q3 - q0*q2)) * RAD_TO_DEG;

    float torque = PID_Update(&pid_balance,
                          0.0f,    // setpoint: upright = 0°
                          pitch,   // measured tilt
                          1.0f/1000 // dt = 5 ms
    );


    if (count == 1000) {
        count = 0;
        // Print the pitch and torque values to console
        sprintf(sMessage, "Pitch: %.2f, Torque: %.2f\n", pitch, torque);
        SendString(USART1, sMessage); // Assuming USART1 is used for console output
    } else {
        count++;
    }
    SetPWM((int16_t)torque);
    SetEnable(SET); // enable motor
  
    // 4) PID → torque command
    //torque_command = pid_update(&pid_balance, 0.0f, pitch, 1.0f/200.0f);
}

#endif // SELF_BALANCING_ENABLE