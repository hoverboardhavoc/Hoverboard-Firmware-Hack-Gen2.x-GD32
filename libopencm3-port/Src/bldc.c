/*
 * Hoverboard libopencm3 port — bldc.c
 *
 * Real port of the FOC commutation loop. Drops:
 *   - Conditional REMOTE_AUTODETECT (autodetect path)
 *   - PILOT_CALCULATE / TEST_HALL2LED debug paths
 *   - Driver() dispatch (PWM-mode only — drivingMode 0)
 *   - Speed/torque/odometer math (revs32, torque32, iOdom)
 * Keeps:
 *   - Block-commutation table + bldc_get_pwm
 *   - PWM output via libopencm3 timer_set_oc_value
 *   - Hall sensor read via libopencm3 gpio_get
 *   - Safety shut-off paths (timer break / break_main_output)
 *   - DC current limit + soft-brake low-pass filter
 *
 * Speed/torque/odometer porting is straightforward but voluminous —
 * left for a follow-up since the data structures it touches
 * (revs32_reg etc.) are libopencm3-agnostic.
 */

#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/gpio.h>
#include <stdint.h>

#define PWM_FREQ                16000U
#define BLDC_TIMER_PERIOD       2250
#define BLDC_TIMER_MID_VALUE    (BLDC_TIMER_PERIOD / 2)   /* 1125 */
#define BLDC_TIMER_MIN_VALUE    10
#define BLDC_TIMER_MAX_VALUE    (BLDC_TIMER_PERIOD - 10)
#define DC_CUR_LIMIT            20.0f                      /* amps */
#define MOTOR_AMP_CONV_DC_AMP   0.0044f
#define ADC_BATTERY_VOLT        0.024f
#define BAT_CELLS               10
#define FILTER_SHIFT            6

#define ABS(x)                  (((x) < 0) ? -(x) : (x))
#define CLAMP(x, lo, hi)        (((x) < (lo)) ? (lo) : (((x) > (hi)) ? (hi) : (x)))

/* Hoverboard hall pin assignment (shared between F130 and F103). */
#define HALL_A_PORT  GPIOB
#define HALL_A_PIN   GPIO10
#define HALL_B_PORT  GPIOB
#define HALL_B_PIN   GPIO11
#define HALL_C_PORT  GPIOB
#define HALL_C_PIN   GPIO12

/* ADC streaming buffer layout (matches setup.c's adc_buffer[]). */
#define ADC_VBATT       0
#define ADC_CURRENT_DC  1

extern volatile uint16_t adc_buffer[];
extern uint32_t          msTicks;
extern volatile int      timedOut;

float    batteryVoltage = BAT_CELLS * 3.6f;
float    currentDC = 0.0f;
float    realSpeed = 0.0f;

volatile int bldc_enable = 0;
int32_t      iBldcInput = 0;
int32_t      iDrivingMode = 0;

static uint8_t hall_a, hall_b, hall_c, hall;
static uint8_t pos, lastPos;
static int32_t bldc_inputFilterPwm;
static int32_t bldc_outputFilterPwm;
static int32_t filter_reg;
static uint8_t iFILTER_SHIFT = FILTER_SHIFT;
static uint16_t buzzerTimer;
static int16_t  offsetcount = 0;
static int16_t  offsetdc = 2000;

/* Hall code → commutation position. Ordering matches the hoverboard's
 * stock motor wiring; some board variants need this remapped. */
static const uint8_t hall_to_pos[8] = {
    0,   /* 000 — invalid */
    3,   /* 100 (a only) */
    5,   /* 010 (b only) */
    4,   /* 110 (a+b) */
    1,   /* 001 (c only) */
    2,   /* 101 (a+c) */
    6,   /* 011 (b+c) */
    0,   /* 111 — invalid */
};

/* Block PWM calculation per commutation position. */
static void bldc_get_pwm(int pwm, int pos_, int *y, int *b, int *g)
{
    switch (pos_) {
    case 1: *y = 0;    *b = pwm;  *g = -pwm; break;
    case 2: *y = -pwm; *b = pwm;  *g = 0;    break;
    case 3: *y = -pwm; *b = 0;    *g = pwm;  break;
    case 4: *y = 0;    *b = -pwm; *g = pwm;  break;
    case 5: *y = pwm;  *b = -pwm; *g = 0;    break;
    case 6: *y = pwm;  *b = 0;    *g = -pwm; break;
    default: *y = 0; *b = 0; *g = 0;
    }
}

void SetEnable(int setEnable) { bldc_enable = setEnable ? 1 : 0; }
void SetPWM(int16_t setPwm) {
    int32_t v = (int32_t)BLDC_TIMER_MID_VALUE * setPwm / 1000;
    bldc_inputFilterPwm = CLAMP(v, -BLDC_TIMER_MID_VALUE, BLDC_TIMER_MID_VALUE);
}
void SetBldcInput(int32_t input) {
    iBldcInput = input;
    /* PWM-mode only in this port. */
    int32_t v = (int32_t)BLDC_TIMER_MID_VALUE * input / 1000;
    bldc_inputFilterPwm = CLAMP(v, -BLDC_TIMER_MID_VALUE, BLDC_TIMER_MID_VALUE);
}
void SetFilter(uint8_t iNew) {
    if (iFILTER_SHIFT != iNew) {
        iFILTER_SHIFT = iNew;
        filter_reg = bldc_outputFilterPwm << iFILTER_SHIFT;
    }
}

static inline uint8_t hall_read(uint32_t port, uint16_t pin)
{
    return (gpio_get(port, pin) != 0) ? 1 : 0;
}

void CalculateBLDC(void)
{
    int y = 0, b = 0, g = 0;

    /* Calibrate ADC current-sense offset for the first 1000 cycles. */
    if (offsetcount < 1000) {
        offsetcount++;
        offsetdc = (adc_buffer[ADC_CURRENT_DC] + offsetdc) / 2;
        return;
    }

    /* Battery voltage low-pass, every 100 cycles. */
    if ((buzzerTimer % 100) == 0) {
        batteryVoltage = batteryVoltage * 0.999f
                       + ((float)adc_buffer[ADC_VBATT] * ADC_BATTERY_VOLT) * 0.001f;
    }

    buzzerTimer++;

    /* DC current. */
    currentDC = ABS(((int32_t)adc_buffer[ADC_CURRENT_DC] - offsetdc)) * MOTOR_AMP_CONV_DC_AMP;

    /* Safety shut-off: over-current, disabled, or timed-out. */
    if (currentDC > DC_CUR_LIMIT || !bldc_enable || timedOut) {
        bldc_inputFilterPwm = iBldcInput = 0;
        SetFilter(14);  /* Soft brake. */
        if (ABS(bldc_outputFilterPwm) < 100) {
            timer_disable_break_main_output(TIM1);
        }
    } else {
        timer_enable_break_main_output(TIM1);
        SetFilter(FILTER_SHIFT);
    }

    /* Read hall sensors. */
    hall_a = hall_read(HALL_A_PORT, HALL_A_PIN);
    hall_b = hall_read(HALL_B_PORT, HALL_B_PIN);
    hall_c = hall_read(HALL_C_PORT, HALL_C_PIN);
    hall = hall_a | (hall_b << 1) | (hall_c << 2);

    pos = hall_to_pos[hall];

    if (pos == 0) {
        /* Invalid hall reading (000 or 111) — disable PWM. */
        timer_disable_break_main_output(TIM1);
        return;
    }

    /* Low-pass filter on PWM input. */
    filter_reg = filter_reg - (filter_reg >> iFILTER_SHIFT) + bldc_inputFilterPwm;
    bldc_outputFilterPwm = filter_reg >> iFILTER_SHIFT;

    /* Block commutation: pick which two of the three phases to drive. */
    bldc_get_pwm(bldc_outputFilterPwm, pos, &y, &b, &g);

    /* Write the 3 PWM channels. PWM_FREQ centred at MID_VALUE; offset by
     * the per-phase value computed above; clamp to [MIN, MAX]. */
    timer_set_oc_value(TIM1, TIM_OC1, CLAMP(y + BLDC_TIMER_MID_VALUE, BLDC_TIMER_MIN_VALUE, BLDC_TIMER_MAX_VALUE));
    timer_set_oc_value(TIM1, TIM_OC2, CLAMP(b + BLDC_TIMER_MID_VALUE, BLDC_TIMER_MIN_VALUE, BLDC_TIMER_MAX_VALUE));
    timer_set_oc_value(TIM1, TIM_OC3, CLAMP(g + BLDC_TIMER_MID_VALUE, BLDC_TIMER_MIN_VALUE, BLDC_TIMER_MAX_VALUE));

    lastPos = pos;
}
