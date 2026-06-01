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
void FUN_0801156c(void);                      /* secondary init */
/* uart_flush (0x08016898) now in uart.c via the header */
void FUN_0800f258(void);                      /* state 0x17/0x18 entry */
void FUN_080093ac(void);                      /* cal 0x12 path */
void FUN_08009598(void);                      /* cal 0x13 path */
void FUN_0800aa98(void);                      /* cal 0x14 path */
void FUN_0800ac44(void);                      /* cal 0x15 path */
void FUN_08009aa4(int arg);                   /* normal boot, low counter */
/* boot_mode_enter (0x0800ede0) now in bms.c via the header */
/* bms_core_update (0x0800bc18) now in state_handlers.c via the header */
/* uart_rx_handler (0x08016688) now in uart.c via the header */
/* uart_tx_isr (0x080167f0) now in uart.c via the header */


/* Per-state routines, indexed by s_state (switch targets from the OEM jump
 * table @ 0x0801E6EC). Addresses retained in comments for later naming. */
void FUN_0801010c(void); /* 1  */  void FUN_08009600(void); /* 2  */
void FUN_0800acac(void); /* 3  */  void FUN_0800e9bc(void); /* 4  */
  void FUN_08010f50(void); /* 6  */
void FUN_080103c8(void); /* 7  */  void FUN_08010580(void); /* 8  */
void FUN_08015b1c(void); /* 9  */  void FUN_08015cd0(void); /* 10 */
void FUN_08008e88(void); /* 11 */  void FUN_08009058(void); /* 12 */
void FUN_0800a5ac(void); /* 13 */  void FUN_0800a77c(void); /* 14 */
void FUN_08010738(void); /* 15 */  void FUN_080108a0(void); /* 16 */
void FUN_08010a3c(void); /* 17 */  void FUN_08009228(void); /* 18 */
void FUN_08009414(void); /* 19 */  void FUN_0800a94c(void); /* 20 */
void FUN_0800ab00(void); /* 21 */  void FUN_0800f33c(void); /* 22 */

  

int main(void)
{
    uint8_t boot_mode;

    hal_bringup();
    FUN_0801156c();
    log_print(2, s_msg_iam_ap);
    uart_flush();

    if (s_idblk[1] == 6 && (s_idblk[2] == 0x17 || s_idblk[2] == 0x18)) {
        /* factory/aging entry */
        log_print(2, s_idblk[2] == 0x17 ? s_msg_rec_mosfail : s_msg_rec_ov2);
        *s_mode |= 0x2000;
        FUN_0800f258();
    } else {
        /* calibration command entry (bytes 0x12..0x15), else normal boot */
        if (s_idblk[2] == 0x12) {
            log_print(2, s_msg_rec_cotp);
            if (s_limit[1] > 0x51 || s_limit[2] > 0x51) {
                FUN_080093ac();
                goto loop;
            }
        } else if (s_idblk[2] == 0x13) {
            log_print(2, s_msg_rec_cutp);
            if (s_limit[1] <= 0x2b || s_limit[2] <= 0x2b) {
                FUN_08009598();
                goto loop;
            }
        } else if (s_idblk[2] == 0x14) {
            log_print(2, s_msg_rec_dotp);
            if (s_limit[1] > 0x63 || s_limit[2] > 0x63) {
                FUN_0800aa98();
                goto loop;
            }
        } else if (s_idblk[2] == 0x15) {
            log_print(2, s_msg_rec_dutp);
            if (s_limit[1] <= 0x17 || s_limit[2] <= 0x17) {
                FUN_0800ac44();
                goto loop;
            }
        }

        if (*s_bootcnt <= 0x752F) {
            FUN_08009aa4(0);
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
            case 1:  FUN_0801010c(); break;
            case 2:  FUN_08009600(); break;
            case 3:  FUN_0800acac(); break;
            case 4:  FUN_0800e9bc(); break;
            case 5:  bms_state_5(); break;
            case 6:  FUN_08010f50(); break;
            case 7:  FUN_080103c8(); break;
            case 8:  FUN_08010580(); break;
            case 9:  FUN_08015b1c(); break;
            case 10: FUN_08015cd0(); break;
            case 11: FUN_08008e88(); break;
            case 12: FUN_08009058(); break;
            case 13: FUN_0800a5ac(); break;
            case 14: FUN_0800a77c(); break;
            case 15: FUN_08010738(); break;
            case 16: FUN_080108a0(); break;
            case 17: FUN_08010a3c(); break;
            case 18: FUN_08009228(); break;
            case 19: FUN_08009414(); break;
            case 20: FUN_0800a94c(); break;
            case 21: FUN_0800ab00(); break;
            case 22: FUN_0800f33c(); break;
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
