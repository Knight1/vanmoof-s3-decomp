/* main.c — application entry and super-loop.
 *
 * The shifter MCU runs a small cooperative loop:
 *   1. Pump the UART → protocol parser.
 *   2. Service incoming commands (shift, status, version).
 *   3. Drive the motor toward the target gear.
 *   4. Periodically report state.
 *   5. Surface latched faults.
 *   6. Kick the watchdog.
 */

#include "shifter.h"
#include "gpio.h"
#include "uart.h"
#include "timer.h"
#include "adc.h"
#include "i2c.h"
#include "motor.h"
#include "sensor.h"
#include "protocol.h"
#include "watchdog.h"
#include "nvm.h"
#include "fault.h"

#define STATE_REPORT_PERIOD_MS   100u
#define WATCHDOG_TIMEOUT_MS      500u
#define SUPPLY_UV_MV             3300u
#define SUPPLY_OV_MV             5800u

static shifter_state_t s_state;
static uint8_t         s_current_gear;
static uint8_t         s_target_gear;
static uint32_t        s_last_report_ms;
static uint32_t        s_shift_start_ms;

static void shifter_home(void)
{
    const nvm_calib_t *cal = nvm_get();
    s_state = SHIFTER_STATE_HOMING;
    motor_run(SHIFT_DIR_DOWN, cal->home_max_steps);
    while (motor_busy()) {
        const uint8_t g = sensor_position_gear();
        if (g == SHIFTER_GEAR_MIN) {
            motor_stop();
            s_current_gear = SHIFTER_GEAR_MIN;
            break;
        }
        watchdog_kick();
    }
    if (s_current_gear == 0u) {
        fault_set(FAULT_HOMING_TIMEOUT);
        s_state = SHIFTER_STATE_FAULT;
    } else {
        s_state = SHIFTER_STATE_IDLE;
    }
}

static void shifter_drive_to_target(void)
{
    if (s_state == SHIFTER_STATE_SHIFTING) {
        if (motor_busy()) {
            const uint32_t elapsed = systick_millis() - s_shift_start_ms;
            if (elapsed > 1500u) {
                motor_stop();
                fault_set(FAULT_MOTOR_STALL);
                s_state = SHIFTER_STATE_FAULT;
            }
            return;
        }
        s_state = SHIFTER_STATE_IDLE;
    }
    if (s_state != SHIFTER_STATE_IDLE) return;
    if (s_target_gear == s_current_gear || s_target_gear == 0u) return;

    const nvm_calib_t *cal = nvm_get();
    const shift_dir_t  dir =
        (s_target_gear > s_current_gear) ? SHIFT_DIR_UP : SHIFT_DIR_DOWN;
    motor_run(dir, cal->steps_per_gear);
    s_state          = SHIFTER_STATE_SHIFTING;
    s_shift_start_ms = systick_millis();
}

static void handle_frame(const proto_frame_t *f)
{
    switch (f->id) {
    case MSG_PING:
        protocol_send(MSG_PONG, NULL, 0u);
        break;

    case MSG_SHIFT:
        if (f->len >= 1u
            && f->payload[0] >= SHIFTER_GEAR_MIN
            && f->payload[0] <= SHIFTER_GEAR_MAX) {
            s_target_gear = f->payload[0];
        }
        break;

    case MSG_VERSION: {
        const uint32_t v = SHIFTERWARE_VERSION_WORD;
        const uint8_t  payload[4] = {
            (uint8_t)(v        & 0xFFu),
            (uint8_t)((v >> 8) & 0xFFu),
            (uint8_t)((v >> 16) & 0xFFu),
            (uint8_t)((v >> 24) & 0xFFu),
        };
        protocol_send(MSG_VERSION, payload, sizeof payload);
        break;
    }

    default:
        break;
    }
}

static void maybe_report_state(void)
{
    const uint32_t now = systick_millis();
    if ((now - s_last_report_ms) < STATE_REPORT_PERIOD_MS) return;
    s_last_report_ms = now;

    const uint16_t mv = sensor_supply_mv();
    if (mv < SUPPLY_UV_MV) fault_set(FAULT_SUPPLY_UNDERVOLT);
    if (mv > SUPPLY_OV_MV) fault_set(FAULT_SUPPLY_OVERVOLT);

    const uint8_t payload[5] = {
        (uint8_t)s_state,
        s_current_gear,
        s_target_gear,
        (uint8_t)(mv      & 0xFFu),
        (uint8_t)((mv >> 8) & 0xFFu),
    };
    protocol_send(MSG_STATE, payload, sizeof payload);

    const uint32_t pending = fault_take_pending();
    if (pending != 0u) {
        const uint8_t fp[4] = {
            (uint8_t)(pending        & 0xFFu),
            (uint8_t)((pending >>  8) & 0xFFu),
            (uint8_t)((pending >> 16) & 0xFFu),
            (uint8_t)((pending >> 24) & 0xFFu),
        };
        protocol_send(MSG_FAULT, fp, sizeof fp);
    }
}

int main(void)
{
    s_state          = SHIFTER_STATE_BOOT;
    s_current_gear   = 0u;
    s_target_gear    = 0u;
    s_last_report_ms = 0u;
    s_shift_start_ms = 0u;

    systick_init();
    fault_init();
    nvm_init();
    sensor_init();
    i2c1_init();
    motor_init();
    protocol_init();
    watchdog_init(WATCHDOG_TIMEOUT_MS);

    shifter_home();

    for (;;) {
        protocol_poll();

        proto_frame_t f;
        while (protocol_recv(&f)) {
            handle_frame(&f);
        }

        shifter_drive_to_target();

        const uint8_t observed = sensor_position_gear();
        if (observed != 0u) {
            s_current_gear = observed;
            if (s_state == SHIFTER_STATE_FAULT && fault_active() == 0u) {
                s_state = SHIFTER_STATE_IDLE;
            }
        }

        maybe_report_state();
        watchdog_kick();
    }
}
