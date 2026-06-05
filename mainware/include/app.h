#ifndef MAINWARE_APP_H
#define MAINWARE_APP_H

#include <stdint.h>

/* Application-core state primitives. The super-loop `main()` (OEM 0x0803DEA8)
 * and the rest of the application state machine will join this module as their
 * callees are decoded; for now it holds the small leaf helpers the console and
 * the loop share. See docs/progress.md for the decoded super-loop map. */

/* Request a subsystem firmware-update mode (OEM update_mode_request,
 * 0x080313E4). The mode byte is overwritten only when the updater is idle
 * (current value 2), so an update already in progress can't be preempted.
 * Called from the super-loop state machine (`FUN_08031900`), the motor-message
 * handler (`FUN_0803A278`), and the "motorupdate" console command (mode 4). */
void update_mode_request(uint8_t mode);

/* Mark a broadcast channel dirty (OEM announce_mark, 0x0802F1C0). channel 0
 * and 1 set their request flags; any other value is a no-op. Called from the
 * super-loop (`FUN_0802AAF8`), the config processor (`FUN_08043B28`), and
 * console_soc_set — which passes 2, a deliberate no-op (the SOC override is
 * instead consumed directly by the loop each iteration). */
void announce_mark(int channel);

/* Set an FSM state byte unless it is currently 6 or 7 (sticky/locked).
 * OEM maybe_set_state_if_unlocked at 0x08029B88. */
void maybe_set_state_if_unlocked(uint8_t new_state);

/* Read the FSM state byte (getter twin; OEM 0x08029BA0). */
uint8_t maybe_get_bike_state(void);

/* Latch a requested value + pending flag (OEM 0x0803B738). */
void maybe_set_pending_request(int32_t value);

/* Resolve a channel's status from the app-context priority bitmasks (OEM
 * 0x0802A2B0); returns 1..3 by priority, or 0 if the channel bit is unset. */
uint32_t channel_resolve_status(uint32_t channel_id);

/* Emit a channel/sound notification: optional sound/clocking scheduler path,
 * amp volume apply, log line, and a 2-byte BLE notify (OEM 0x0802A064). */
void channel_notify_emit(uint32_t channel_code, int32_t volume_index);

/* Resolve a channel's status and emit a notify (OEM 0x0802A2F0). */
void channel_notify_with_status(uint32_t channel_id);

/* Generic state/mode flag byte at SRAM 0x20000083 (OEM 0x08036B8C/0x08036B80). */
uint8_t state_flag_get(void);
void    state_flag_set(uint8_t value);

/* Global mode/sub-mode state byte at SRAM 0x20000068 (OEM 0x0802F0F8/0x0802E7F4). */
uint8_t aux_mode_byte_get(void);
void    set_mode_state_byte(uint8_t mode);

/* Enter "show"/display mode 3 and (re)arm its 4000-tick periodic task (OEM 0x0802F104). */
void enter_mode3_arm_show_timer(void);

/* I2C3 bus recovery: pulse SCL (PA8) until SDA (PC9) releases; returns the pulse
 * count logged as "Clocking %d" (OEM 0x0803C8F4). */
uint8_t clock_pulse_gpioa8_until_pc9(void);

/* Read the subsystem update-mode / link-ready byte at SRAM 0x20000076 (OEM
 * 0x080313D8) — getter twin of update_mode_request; 2 = idle/ready. */
uint8_t update_mode_get(void);

/* Reset the request-context dual buffers + flags at 0x20008230 (OEM 0x0803B780). */
void reset_dual_buffers_and_flags(void);

#endif
