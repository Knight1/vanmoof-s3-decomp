#ifndef MAINWARE_RTC_H
#define MAINWARE_RTC_H

#include <stdint.h>

/* STM32 RTC <-> Unix-epoch helpers (src/rtc.c). Thin wrappers over the calendar
 * <-> epoch converters and the CubeF4 RTC HAL; the RTC handle lives at SRAM
 * 0x200099E4. See docs/state-machine.md (tracking-time field, BLE 0x5567). */

/* Read the RTC and return the current wall-clock time as Unix epoch seconds.
 * OEM rtc_now_epoch_seconds at 0x080380EC. */
uint32_t rtc_now_epoch_seconds(void);

/* Convert a Unix epoch (seconds) to the calendar, echo "SET dd-mm-20yy hh:mm:ss"
 * to the console logger, then program the RTC date + time. The inverse of
 * rtc_now_epoch_seconds. OEM rtc_set_from_unix_time at 0x080381D0. */
void rtc_set_from_unix_time(uint32_t epoch);

#endif
