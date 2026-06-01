#include "powerbankware.h"

/*
 * charger_afe_refresh — OEM FUN_0800BAD4.
 *
 * Slow-tick FEDL5236 housekeeping run while a charger may be attached. When the
 * SPI link is ready, read status reg 1 (2 bytes); if the FET-control nibble or
 * the protection bit don't read back as expected, re-run the full AFE bring-up
 * and re-apply the reg-9 FET byte, up to five attempts, verifying the read-back
 * each round. Persistent mismatch logs a FET-control error and hands off to the
 * AFE recovery path. On success, clear the sub-state byte and select channel 8.
 *
 * AFE read buffer: 0x20000614 (status nibble at +2, protection bits at +3).
 */

#define AFE   ((volatile uint8_t *)0x20000614)
#define FETB  (*(volatile uint8_t *)0x20000412)

extern const char s_fedl_proc_busy[];    /* "\nFEDL5236_Process()--> SPI Busy\r"          0x0801DCBC */
extern const char s_fedl_proc_fet_err[]; /* "\nFEDL5236_Process()--> FET Control Error\r" 0x0801DCE0 */

/* AFE recovery / halt path (deeper leaf, own pass). */


void charger_afe_refresh(void)
{
    if (spi_get_state((void *)0x20000634) != 1) {
        log_print(2, s_fedl_proc_busy);
        return;
    }

    if (fedl5236_read_data(1, 2) != 0 &&
        (((AFE[2] & 0xf) != 0xf) || ((AFE[3] & 2) != 2))) {
        uint8_t want = FETB;
        uint8_t attempt = 0;
        do {
            delay_ms(attempt == 0 ? 5 : 500);
            fedl5236_initialize();
            delay_ms(attempt == 0 ? 5 : 100);
            FETB = want;
            fedl5236_command_write(9, FETB);
            delay_ms(attempt == 0 ? 5 : 100);
            AFE[2] = 0;
            fedl5236_read_data(0xd, 1);
            attempt++;
            if (attempt > 4) {
                log_print(2, s_fedl_proc_fet_err);
                bms_enter_afe_fault();
                return;
            }
        } while (attempt < 5 && ((want ^ AFE[2]) & 3) != 0);
    }

    *(volatile uint8_t *)0x2000041A = 0;
    fedl5236_command_write(8, 0x91);
}
