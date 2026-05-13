#ifndef SHIFTER_MOTOR_H
#define SHIFTER_MOTOR_H

#include <stdint.h>
#include <stdbool.h>
#include "shifter.h"

#define MOTOR_STEPS_PER_GEAR   200u
#define MOTOR_STEP_HZ_DEFAULT  1500u

void  motor_init(void);
void  motor_run(shift_dir_t dir, uint32_t steps);
void  motor_stop(void);
bool  motor_busy(void);
uint32_t motor_steps_remaining(void);

void  motor_step_tick(void);

#endif /* SHIFTER_MOTOR_H */
