#ifndef PID_H
#define PID_H

typedef struct {
    float Kp;        // proportional gain
    float Ki;        // integral    gain
    float Kd;        // derivative  gain

    float integral;  // accumulated error
    float prev_err;  // error at previous step

    float out_min;   // output clamp low
    float out_max;   // output clamp high
} PID;

// initialize a PID instance
static inline void PID_Init(PID *pid,
                            float Kp, float Ki, float Kd,
                            float out_min, float out_max)
{
    pid->Kp       = Kp;
    pid->Ki       = Ki;
    pid->Kd       = Kd;
    pid->integral = 0.0f;
    pid->prev_err = 0.0f;
    pid->out_min  = out_min;
    pid->out_max  = out_max;
}

// compute one PID step
float PID_Update(PID *pid,
                 float setpoint,
                 float measurement,
                 float dt);

#endif //