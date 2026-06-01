#include "powerbankware.h"

/*
 * rtc_set — OEM FUN_080122dc.
 *
 * Unpacks the two packed date words built by the "Set RTC" command
 * (lo = sec|min<<8|hour<<16, hi = day|month<<8|year<<16) into the HAL RTC
 * time/date structs and programs the RTC (handle @ 0x200006f0). The ms tick
 * counter (0x20002614) is zeroed alongside.
 *
 * FUN_0801c288 = HAL_RTC_SetTime, FUN_0801c410 = HAL_RTC_SetDate (their own
 * pass); format arg 0 = BIN.
 */
extern int FUN_0801c288(void *hrtc, void *time, int format, void *h2,
                        uint32_t lo, uint32_t hi);
extern int FUN_0801c410(void *hrtc, void *date, int format);

void rtc_set(uint32_t lo, uint32_t hi)
{
    void * const hrtc = (void *)0x200006f0;

    /* HAL_RTC_TimeTypeDef leading bytes: hours, minutes, seconds. */
    uint8_t time[8];
    time[0] = (uint8_t)(lo >> 16);     /* hours   */
    time[1] = (uint8_t)(lo >> 8);      /* minutes */
    time[2] = (uint8_t)lo;             /* seconds */

    /* HAL_RTC_DateTypeDef: weekday, month, day, year. */
    uint8_t date[8];
    date[0] = 1;                       /* weekday */
    date[1] = (uint8_t)(hi >> 16);     /* year    */
    date[2] = (uint8_t)(hi >> 8);      /* month   */
    date[3] = (uint8_t)hi;             /* day     */

    *(volatile uint32_t *)0x20002614 = 0;
    FUN_0801c288(hrtc, time, 0, hrtc, lo, hi);
    FUN_0801c410(hrtc, date, 0);
}
