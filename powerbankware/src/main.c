#include "powerbankware.h"

/*
 * main (OEM 0x0800f52c) — powerbankware application entry.
 *
 * Faithful reconstruction from the OEM disassembly. Brings up the HAL, prints
 * the boot banner, runs a boot-mode pre-check (factory/calibration entry), then
 * enters the mode-gated state-machine super-loop. Structurally identical to
 * batteryware's main: a 28-way switch on the current-state byte, gated on the
 * low three bits of the mode word, with two per-iteration tick calls.
 *
 * Globals (verified absolute addresses):
 *   s_mode    0x200006A0  u16  mode/cfg word (bits 0/1/2 gate the dispatch)
 *   s_state   0x200005AC  u8   current state (switch index, valid < 0x1c)
 *   s_idblk   0x20000724  u8[] identity/selector block (boot-mode bytes [1],[2])
 *   s_limit   0x20000218  u8[] cal limit block ([1],[2] checked vs thresholds)
 *   s_bootcnt 0x200003CE  u16  boot counter vs 0x752F threshold
 */
static volatile uint16_t * const s_mode    = (volatile uint16_t *)0x200006A0;
static volatile uint8_t  * const s_state   = (volatile uint8_t  *)0x200005AC;
static volatile uint8_t  * const s_idblk   = (volatile uint8_t  *)0x20000724;
static volatile uint8_t  * const s_limit   = (volatile uint8_t  *)0x20000218;
static volatile uint16_t * const s_bootcnt = (volatile uint16_t *)0x200003CE;

/* Boot banner + fault-record-mode log strings (flash; defined in strings.c).
 * The idblk[2] selector picks a "Record <fault> Mode" factory/test entry. */
extern const char s_msg_iam_ap[];      /* 0x0801E05C "\nI am VM-BATT AP\r" (entry) */
extern const char s_msg_rec_mosfail[]; /* 0x0801E070 "\nRecord MOS Failure Mode\r" (sel 0x17) */
extern const char s_msg_rec_ov2[];     /* 0x0801E08C "\nRecord OV 2nd Mode\r"      (sel 0x18) */
extern const char s_msg_rec_cotp[];    /* 0x0801E0A4 "\nRecord COTP Mode\r"        (sel 0x12) */
extern const char s_msg_rec_cutp[];    /* 0x0801E0B8 "\nRecord CUTP Mode\r"        (sel 0x13) */
extern const char s_msg_rec_dotp[];    /* 0x0801E0CC "\nRecord DOTP Mode\r"        (sel 0x14) */
extern const char s_msg_rec_dutp[];    /* 0x0801E0E0 "\nRecord DUTP Mode\r"        (sel 0x15) */

void hal_bringup(void);                       /* 0x080114dc */
/* log_print now declared in powerbankware.h (variadic, src/uart.c) */
/* uart_flush (0x08016898) now in uart.c via the header */
/* boot_mode_enter (0x0800ede0) now in bms.c via the header */
/* bms_core_update (0x0800bc18) now in state_handlers.c via the header */
/* uart_rx_handler (0x08016688) now in uart.c via the header */
/* uart_tx_isr (0x080167f0) now in uart.c via the header */


/* Per-state routines, states 1..22, are declared in powerbankware.h
 * (src/states.c) and dispatched by the switch below. */

int main(void)
{
    uint8_t boot_mode;

    hal_bringup();
    bms_system_init();
    log_print(2, s_msg_iam_ap);
    uart_flush();

    if (s_idblk[1] == 6 && (s_idblk[2] == 0x17 || s_idblk[2] == 0x18)) {
        /* factory/aging entry */
        log_print(2, s_idblk[2] == 0x17 ? s_msg_rec_mosfail : s_msg_rec_ov2);
        *s_mode |= 0x2000;
        bms_enter_ov2_record();
    } else {
        /* calibration command entry (bytes 0x12..0x15), else normal boot */
        if (s_idblk[2] == 0x12) {
            log_print(2, s_msg_rec_cotp);
            if (s_limit[1] > 0x51 || s_limit[2] > 0x51) {
                bms_enter_cotp();
                goto loop;
            }
        } else if (s_idblk[2] == 0x13) {
            log_print(2, s_msg_rec_cutp);
            if (s_limit[1] <= 0x2b || s_limit[2] <= 0x2b) {
                bms_enter_cutp();
                goto loop;
            }
        } else if (s_idblk[2] == 0x14) {
            log_print(2, s_msg_rec_dotp);
            if (s_limit[1] > 0x63 || s_limit[2] > 0x63) {
                bms_enter_dotp();
                goto loop;
            }
        } else if (s_idblk[2] == 0x15) {
            log_print(2, s_msg_rec_dutp);
            if (s_limit[1] <= 0x17 || s_limit[2] <= 0x17) {
                bms_enter_dutp();
                goto loop;
            }
        }

        if (*s_bootcnt <= 0x752F) {
            boot_enter_operating(0);
            goto loop;
        }
        boot_mode = ((*s_mode >> 8) & 1) ? 1 : 3;
        boot_mode_enter(boot_mode);
    }

loop:
    uart_flush();
    for (;;) {
        if ((*s_mode & 7) == 0) {        /* mode bits 0,1,2 all clear */
            switch (*s_state) {
            case 1:  bms_state_1();  break;
            case 2:  bms_state_2();  break;
            case 3:  bms_state_3();  break;
            case 4:  bms_state_4();  break;
            case 5:  bms_state_5();  break;
            case 6:  bms_state_6();  break;
            case 7:  bms_state_7();  break;
            case 8:  bms_state_8();  break;
            case 9:  bms_state_9();  break;
            case 10: bms_state_10(); break;
            case 11: bms_state_11(); break;
            case 12: bms_state_12(); break;
            case 13: bms_state_13(); break;
            case 14: bms_state_14(); break;
            case 15: bms_state_15(); break;
            case 16: bms_state_16(); break;
            case 17: bms_state_17(); break;
            case 18: bms_state_18(); break;
            case 19: bms_state_19(); break;
            case 20: bms_state_20(); break;
            case 21: bms_state_21(); break;
            case 22: bms_state_22(); break;
            case 23: case 24: case 25: bms_state_fault(); break;
            case 26: bms_state_fault(); break;
            case 27: bms_state_shipping_wait(); break;
            default: bms_state_idle(); break;  /* state 0 or > 0x1b */
            }
        } else {
            bms_core_update();
        }
        uart_rx_handler();
        uart_tx_isr();
    }
}
