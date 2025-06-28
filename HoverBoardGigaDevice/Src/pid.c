#include "pid.h"

// basic PID with anti-windup and output clamping
float PID_Update(PID *pid,
                 float setpoint,
                 float measurement,
                 float dt)
{
    // 1) error
    float err = setpoint - measurement;

    // 2) integral term (with simple anti-windup)
    pid->integral += err * dt;
    // clamp integral so Ki*integral stays in output bounds
    if (pid->integral * pid->Ki > pid->out_max) {
        pid->integral = pid->out_max / pid->Ki;
    } else if (pid->integral * pid->Ki < pid->out_min) {
        pid->integral = pid->out_min / pid->Ki;
    }

    // 3) derivative term
    float deriv = (err - pid->prev_err) / dt;
    pid->prev_err = err;

    // 4) PID output
    float out = pid->Kp * err
              + pid->Ki * pid->integral
              + pid->Kd * deriv;

    // 5) clamp output
    if (out > pid->out_max) out = pid->out_max;
    else if (out < pid->out_min) out = pid->out_min;

    return out;
}