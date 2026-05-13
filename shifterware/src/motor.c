/* motor.c — shifter motor coil sequencer.
 *
 * The S3 shifter uses a small geared brushed DC motor driven through an
 * H-bridge on two GPIOs. "Stepping" here means cycling the bridge on/off
 * at the rate set by TIM3 until the requested step count is reached,
 * letting the higher-level state machine close the loop against the Hall
 * sensor.
 */

#include "motor.h"
#include "gpio.h"
#include "timer.h"
#include "mm32f031.h"

/* H-bridge control pins (PB0 = IN1, PB1 = IN2). */
#define MOTOR_PORT       GPIOB
#define MOTOR_PIN_IN1    0u
#define MOTOR_PIN_IN2    1u

static volatile shift_dir_t s_dir;
static volatile uint32_t    s_steps_left;
static volatile bool        s_phase;

void motor_init(void)
{
    gpio_port_clock_enable(MOTOR_PORT);
    gpio_pin_mode(MOTOR_PORT, MOTOR_PIN_IN1, GPIO_MODE_OUTPUT);
    gpio_pin_mode(MOTOR_PORT, MOTOR_PIN_IN2, GPIO_MODE_OUTPUT);
    gpio_pin_output_type(MOTOR_PORT, MOTOR_PIN_IN1, GPIO_OTYPE_PP);
    gpio_pin_output_type(MOTOR_PORT, MOTOR_PIN_IN2, GPIO_OTYPE_PP);
    gpio_pin_speed(MOTOR_PORT, MOTOR_PIN_IN1, GPIO_SPEED_HIGH);
    gpio_pin_speed(MOTOR_PORT, MOTOR_PIN_IN2, GPIO_SPEED_HIGH);

    motor_stop();
    tim3_step_init(MOTOR_STEP_HZ_DEFAULT);
}

static void motor_drive(shift_dir_t dir, bool on)
{
    if (!on || dir == SHIFT_DIR_NONE) {
        gpio_pin_clear(MOTOR_PORT, MOTOR_PIN_IN1);
        gpio_pin_clear(MOTOR_PORT, MOTOR_PIN_IN2);
        return;
    }
    if (dir == SHIFT_DIR_UP) {
        gpio_pin_set  (MOTOR_PORT, MOTOR_PIN_IN1);
        gpio_pin_clear(MOTOR_PORT, MOTOR_PIN_IN2);
    } else {
        gpio_pin_clear(MOTOR_PORT, MOTOR_PIN_IN1);
        gpio_pin_set  (MOTOR_PORT, MOTOR_PIN_IN2);
    }
}

void motor_run(shift_dir_t dir, uint32_t steps)
{
    if (dir == SHIFT_DIR_NONE || steps == 0u) {
        motor_stop();
        return;
    }
    s_dir        = dir;
    s_steps_left = steps;
    s_phase      = false;
    motor_drive(dir, true);
    tim3_step_start();
}

void motor_stop(void)
{
    tim3_step_stop();
    s_steps_left = 0u;
    s_dir        = SHIFT_DIR_NONE;
    motor_drive(SHIFT_DIR_NONE, false);
}

bool motor_busy(void)
{
    return s_steps_left != 0u;
}

uint32_t motor_steps_remaining(void)
{
    return s_steps_left;
}

void motor_step_tick(void)
{
    if (s_steps_left == 0u) {
        motor_stop();
        return;
    }
    s_phase = !s_phase;
    motor_drive(s_dir, s_phase);
    if (!s_phase) {
        s_steps_left--;
        if (s_steps_left == 0u) {
            motor_stop();
        }
    }
}
