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

/* Resolve a channel's status and emit a notify (OEM 0x0802A2F0). */
void channel_notify_with_status(uint32_t channel_id);

#endif
