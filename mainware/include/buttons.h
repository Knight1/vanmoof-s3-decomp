#ifndef MAINWARE_BUTTONS_H
#define MAINWARE_BUTTONS_H

/* Poll the three physical push-buttons — bell/horn (PC0), boost (PC1) and
 * reset (PD2) — and run each button's debounce/press-classify state machine.
 * Called once per super-loop pass from main (OEM 0x08040380). */
void button_press_state_machines_step(void);

#endif
