#include <stdint.h>

#include "buttons.h"
#include "gpio.h"        /* gpio_pc0_is_low / gpio_pc1_is_low */
#include "scheduler.h"   /* scheduler_*, SCHED_SLOT_NONE */

/* Three physical push-buttons on the S3, each polled once per super-loop pass by
 * button_press_state_machines_step (OEM 0x08040380). Each button runs an identical
 * 6-state debounce/press-classify machine and writes a small event code that the
 * rest of the firmware consumes (status_process reactions + the BLE 0x5568 read):
 *
 *   Button   Pin    hold-tmr  debounce  timer names
 *   ------   ----   --------  --------  ----------------------------------------
 *   bell     PC0    500 ms    20 ms     button_horn_tmr / button_horn_debounce_tmr
 *   boost    PC1    150 ms    20 ms     button_boost_tmr / button_boost_debounce_tmr
 *   reset    PD2    500 ms    20 ms     button_reset_tmr / button_reset_debounce_tmr
 *
 * The "bell" button is the bike's horn/bell (see button_horn_* + "Button horn"),
 * so what the app calls the bell is this PC0 line. Bell/boost sense LOW = pressed
 * via gpio_pc0_is_low / gpio_pc1_is_low; reset reads PD2 directly (0 = pressed).
 *
 * State byte meaning (per-button, written to BTN_ST[event]):
 *   1 = short click (released before the hold timer)
 *   2 = held        (hold timer elapsed, button still down)
 *   4 = long hold   (extra 3 s elapsed while held)
 *   reset also: 3 (release-during-hold) and 6 (single-count completion latch).
 *
 * State/slot storage (absolute SRAM, matching the OEM):
 *   BTN_ST   0x20009360 : [0]=bell event [1]=boost event [2]=reset event
 *                         [3]=bell SM    [4]=boost SM     [5]=reset SM
 *                         [6]=reset press-count   (region shared with the
 *                         announcement-record block, which uses higher offsets)
 *   BTN_SLOT 0x20000102 : [0]/[1] bell hold/debounce, [2]/[3] boost hold/debounce,
 *                         [4] boost action, [5]/[6] reset hold/debounce, [7] reset action
 */

#define BTN_ST     ((uint8_t *)0x20009360u)
#define BTN_SLOT   ((uint8_t *)0x20000102u)

#define RESET_PORT ((void *)0x40020C00u)   /* GPIOD */
#define RESET_PIN  0x0004u                 /* PD2   */

/* OEM HAL_GPIO_ReadPin (1 = pin high), 0x08026AB8 — declared extern here as it is
 * in gpio.c / main.c (there is no shared HAL header). */
extern int HAL_GPIO_ReadPin(void *GPIOx, uint16_t pin_mask);

/* Fault-flag + time + state helpers used by the stuck-button debounce. */
extern int      state_flags_set();          /* 0x0802A268 */
extern int      state_flags_clear();        /* 0x0802A240 */
extern int      state_flags_test();         /* 0x0802A28C */
extern uint32_t rtc_now_epoch_seconds(void);/* 0x080380EC */
extern uint8_t  maybe_get_bike_state(void); /* 0x08029BA0 */

void button_press_state_machines_step(void)
{
    /* ── Button 1: bell / horn (PC0) ─────────────────────────────────────── */
    switch (BTN_ST[3]) {
    case 0:
        if (gpio_pc0_is_low() == 0) {                 /* released */
            scheduler_release(&BTN_SLOT[0]);
            scheduler_release(&BTN_SLOT[1]);
        } else {                                      /* pressed */
            if (BTN_SLOT[0] == SCHED_SLOT_NONE) {
                BTN_SLOT[0] = scheduler_alloc();
                scheduler_set_timer_name(BTN_SLOT[0], 500, "button_horn_tmr");
            }
            scheduler_start(BTN_SLOT[0], 500, 0);
            if (BTN_SLOT[1] == SCHED_SLOT_NONE) {
                BTN_SLOT[1] = scheduler_alloc();
                scheduler_set_timer_name(BTN_SLOT[1], 0x14, "button_horn_debounce_tmr");
                scheduler_start(BTN_SLOT[1], 0x14, 0);
            }
            if (scheduler_slot_is_idle(BTN_SLOT[1]) != 0) {
                BTN_ST[3] = 1;
            }
        }
        break;
    case 1:
        if (scheduler_slot_is_idle(BTN_SLOT[0]) == 0) {
            if (gpio_pc0_is_low() == 0) {
                BTN_ST[3] = 4;                        /* released early -> short click */
            }
        } else {
            scheduler_start(BTN_SLOT[0], 3000, 0);
            BTN_ST[3] = 2;                            /* hold confirmed */
        }
        break;
    case 2:
        BTN_ST[0] = 2;
        if (gpio_pc0_is_low() == 0) {
            BTN_ST[3] = 5;
        } else if (scheduler_slot_is_idle(BTN_SLOT[0]) != 0) {
            BTN_ST[3] = 3;                            /* 3 s long hold */
        }
        break;
    case 3:
        BTN_ST[0] = 4;
        BTN_ST[3] = 5;
        break;
    case 4:
        BTN_ST[0] = 1;
        BTN_ST[3] = 5;
        break;
    case 5:
        scheduler_release(&BTN_SLOT[0]);
        scheduler_release(&BTN_SLOT[1]);
        if (gpio_pc0_is_low() == 0) {
            BTN_ST[3] = 0;
        }
        break;
    }

    /* ── Button 2: boost (PC1) ───────────────────────────────────────────── */
    switch (BTN_ST[4]) {
    case 0:
        if (gpio_pc1_is_low() == 0) {
            scheduler_release(&BTN_SLOT[2]);
            scheduler_release(&BTN_SLOT[3]);
        } else {
            if (BTN_SLOT[2] == SCHED_SLOT_NONE) {
                BTN_SLOT[2] = scheduler_alloc();
                scheduler_set_timer_name(BTN_SLOT[2], 0x96, "button_boost_tmr");
            }
            scheduler_start(BTN_SLOT[2], 0x96, 0);
            if (BTN_SLOT[3] == SCHED_SLOT_NONE) {
                BTN_SLOT[3] = scheduler_alloc();
                scheduler_set_timer_name(BTN_SLOT[3], 0x14, "button_boost_debounce_tmr");
                scheduler_start(BTN_SLOT[3], 0x14, 0);
            }
        }
        if (scheduler_slot_is_idle(BTN_SLOT[3]) != 0) {
            BTN_ST[4] = 1;
        }
        break;
    case 1:
        if (scheduler_slot_is_idle(BTN_SLOT[2]) == 0) {
            if (gpio_pc1_is_low() == 0) {
                BTN_ST[4] = 4;
            }
        } else {
            scheduler_start(BTN_SLOT[2], 3000, 0);
            BTN_ST[4] = 2;
        }
        break;
    case 2:
        BTN_ST[1] = 2;
        if (gpio_pc1_is_low() == 0) {
            BTN_ST[4] = 5;
        } else if (scheduler_slot_is_idle(BTN_SLOT[2]) != 0) {
            BTN_ST[4] = 3;
        }
        break;
    case 3:
        BTN_ST[1] = 4;
        BTN_ST[4] = 5;
        break;
    case 4:
        BTN_ST[1] = 1;
        BTN_ST[4] = 5;
        if (BTN_SLOT[4] == SCHED_SLOT_NONE) {
            BTN_SLOT[4] = scheduler_alloc();
            scheduler_start(BTN_SLOT[4], 500, 0);
        } else {
            scheduler_release(&BTN_SLOT[4]);
            BTN_ST[1] = 5;
        }
        break;
    case 5:
        scheduler_release(&BTN_SLOT[2]);
        scheduler_release(&BTN_SLOT[3]);
        if (gpio_pc1_is_low() == 0) {
            BTN_ST[4] = 0;
        }
        break;
    }
    if (scheduler_slot_is_idle(BTN_SLOT[4]) != 0) {
        scheduler_release(&BTN_SLOT[4]);
    }

    /* ── Button 3: reset (PD2) ───────────────────────────────────────────── */
    switch (BTN_ST[5]) {
    case 0:
        if (HAL_GPIO_ReadPin(RESET_PORT, RESET_PIN) == 0) {   /* pressed (low) */
            if (BTN_SLOT[5] == SCHED_SLOT_NONE) {
                BTN_SLOT[5] = scheduler_alloc();
                scheduler_set_timer_name(BTN_SLOT[5], 500, "button_reset_tmr");
            }
            scheduler_start(BTN_SLOT[5], 500, 0);
            if (BTN_SLOT[6] == SCHED_SLOT_NONE) {
                BTN_SLOT[6] = scheduler_alloc();
                scheduler_set_timer_name(BTN_SLOT[6], 0x14, "button_reset_debounce_tmr");
                scheduler_start(BTN_SLOT[6], 0x14, 0);
            }
        } else {
            scheduler_release(&BTN_SLOT[5]);
            scheduler_release(&BTN_SLOT[6]);
        }
        if (scheduler_slot_is_idle(BTN_SLOT[6]) != 0) {
            BTN_ST[5] = 1;
        }
        break;
    case 1:
        if (scheduler_slot_is_idle(BTN_SLOT[5]) == 0) {
            if (HAL_GPIO_ReadPin(RESET_PORT, RESET_PIN) != 0) {   /* released early */
                BTN_ST[6] = BTN_ST[6] + 1;
                BTN_ST[5] = 4;
            }
        } else {
            scheduler_start(BTN_SLOT[5], 3000, 0);
            BTN_ST[5] = 2;
        }
        break;
    case 2:
        BTN_ST[2] = 2;
        if (HAL_GPIO_ReadPin(RESET_PORT, RESET_PIN) == 0) {
            if (scheduler_slot_is_idle(BTN_SLOT[5]) != 0) {
                BTN_ST[5] = 3;
            }
        } else {
            BTN_ST[2] = 3;
            BTN_ST[5] = 5;
        }
        break;
    case 3:
        BTN_ST[2] = 4;
        BTN_ST[5] = 5;
        break;
    case 4:
        BTN_ST[2] = 1;
        BTN_ST[5] = 5;
        if (BTN_SLOT[7] == SCHED_SLOT_NONE && BTN_ST[6] == 1) {
            BTN_SLOT[7] = scheduler_alloc();
            scheduler_start(BTN_SLOT[7], 500, 0);
        }
        break;
    case 5:
        scheduler_release(&BTN_SLOT[5]);
        scheduler_release(&BTN_SLOT[6]);
        if (HAL_GPIO_ReadPin(RESET_PORT, RESET_PIN) != 0) {
            BTN_ST[5] = 0;
        }
        break;
    }
    if (scheduler_slot_is_idle(BTN_SLOT[7]) != 0) {
        if (BTN_ST[6] == 1) {
            BTN_ST[2] = 6;
        }
        BTN_ST[6] = 0;
        scheduler_release(&BTN_SLOT[7]);
    }
}

/* charger_and_pc1_sense_debounce (OEM 0x08040788) — "stuck button" watchdog for
 * the horn (PC0) and boost (PC1) lines. If a button reads pressed continuously for
 * >20 s (while PD2 is high) it latches a fault flag (0x100 = horn, 0x200 = boost)
 * so the rest of the firmware stops honouring that button ("Horn stuck cannot use
 * backup code"); the flag auto-clears after a 60 s "end_stuck_horn" timer once the
 * button is released. `state` points at session_ctx+0x310: +0x10 = horn press
 * epoch, +0x14 = boost press epoch. The boost side is skipped while riding
 * (bike state 0x0C). Timer slots are BTN_SLOT[8]/[9]. Called each super-loop. */
void charger_and_pc1_sense_debounce(void *state)
{
    uint8_t *st = (uint8_t *)state;

    /* ── Horn (PC0) stuck detection, fault flag 0x100 ─────────────────────── */
    if (gpio_pc0_is_low() == 0) {
        *(uint32_t *)(st + 0x10) = 0;
    }
    if (state_flags_test(0, 0x100) != 0) {
        if (gpio_pc0_is_low() == 0) {
            if (BTN_SLOT[8] == SCHED_SLOT_NONE) {
                BTN_SLOT[8] = scheduler_alloc();
                scheduler_start(BTN_SLOT[8], 60000, 0);
                scheduler_set_timer_name(BTN_SLOT[8], 60000, "end_stuck_horn");
            }
            if (scheduler_slot_is_idle(BTN_SLOT[8]) != 0) {
                scheduler_release(&BTN_SLOT[8]);
                state_flags_clear(0, 0x100);
            }
        } else {
            scheduler_release(&BTN_SLOT[8]);
        }
    }
    if (*(uint32_t *)(st + 0x10) == 0 && gpio_pc0_is_low() != 0) {
        *(uint32_t *)(st + 0x10) = rtc_now_epoch_seconds();
    }
    if (*(uint32_t *)(st + 0x10) != 0 && HAL_GPIO_ReadPin(RESET_PORT, RESET_PIN) != 0) {
        if (*(uint32_t *)(st + 0x10) + 0x14 < rtc_now_epoch_seconds()) {
            state_flags_set(0, 0x100);
        }
    }

    /* ── Boost (PC1) stuck detection, fault flag 0x200 ────────────────────── */
    if (gpio_pc1_is_low() == 0) {
        *(uint32_t *)(st + 0x14) = 0;
    }
    if (state_flags_test(0, 0x200) != 0) {
        if (gpio_pc1_is_low() == 0) {
            if (BTN_SLOT[9] == SCHED_SLOT_NONE) {
                BTN_SLOT[9] = scheduler_alloc();
                scheduler_start(BTN_SLOT[9], 60000, 0);
                scheduler_set_timer_name(BTN_SLOT[9], 60000, "end_stuck_horn");
            }
            if (scheduler_slot_is_idle(BTN_SLOT[9]) != 0) {
                scheduler_release(&BTN_SLOT[9]);
                state_flags_clear(0, 0x200);
            }
        } else {
            scheduler_release(&BTN_SLOT[9]);
        }
    }
    if (maybe_get_bike_state() != 0xc) {
        if (*(uint32_t *)(st + 0x14) == 0 && gpio_pc1_is_low() != 0) {
            *(uint32_t *)(st + 0x14) = rtc_now_epoch_seconds();
        }
        if (*(uint32_t *)(st + 0x14) != 0 && HAL_GPIO_ReadPin(RESET_PORT, RESET_PIN) != 0) {
            if (*(uint32_t *)(st + 0x14) + 0x14 < rtc_now_epoch_seconds()) {
                state_flags_set(0, 0x200);
            }
        }
    }
}
