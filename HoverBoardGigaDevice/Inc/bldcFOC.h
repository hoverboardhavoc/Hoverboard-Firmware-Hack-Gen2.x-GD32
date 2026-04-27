#ifndef BLDCFOC_H
#define BLDCFOC_H

#include "bldc.h"

// bldcFOC.c implements the two virtual methods declared in bldc.h:
//   void InitBldc(void);
//   void bldc_get_pwm(int pwm, int pos, int *y, int *b, int *g);
// All FOC internals (PLL, Clarke/Park, PI, SVPWM, sine table) are
// file-local statics in bldcFOC.c — nothing else leaks into the API.

// Main-loop RTT log emitter; called from main.c when BLDC_FOC + RTT_REMOTE.
void RttMainPoll(void);

#endif // BLDCFOC_H
