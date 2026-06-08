#include <stdint.h>

#include "log.h"
#include "rtc.h"

/*
 * rtc.c — STM32 RTC <-> Unix-epoch helpers for the main controller.
 *
 * Both functions are thin wrappers: the OEM delegates the calendar<->epoch date
 * math to a pair of converters (0x08037F9C / 0x08038110) and the register
 * programming to the CubeF4 RTC HAL. Those callees + the RTC handle are kept as
 * to-be-decoded externs (HAL-heavy); only the wrapper control flow is sourced,
 * verified against the live disassembly at 0x080380EC / 0x080381D0.
 *
 * The shared RTC HAL handle lives at SRAM 0x200099E4. The broken-down calendar
 * uses the wide layout that rtc_fill_time_fields / rtc_epoch_to_calendar emit:
 * [0]=hours [1]=minutes [2]=seconds, [0x15]=month [0x16]=day [0x17]=year(2-digit).
 */

#define RTC_HANDLE  ((void *)0x200099e4u)

/* Wide broken-down calendar (0x18 bytes) produced by both converters. */
typedef struct {
    uint8_t hours;        /* +0x00 */
    uint8_t minutes;      /* +0x01 */
    uint8_t seconds;      /* +0x02 */
    uint8_t _pad[0x12];   /* +0x03..+0x14 */
    uint8_t month;        /* +0x15 */
    uint8_t day;          /* +0x16 */
    uint8_t year;         /* +0x17  (2-digit, +2000) */
} rtc_calendar_t;

/* RTC_DateTypeDef / RTC_TimeTypeDef heads (RTC_FORMAT_BIN: fields are binary). */
typedef struct { uint8_t weekday, month, date, year; } rtc_date_bin_t;
typedef struct {
    uint8_t  hours, minutes, seconds, time_format;
    uint32_t pad[4];   /* SubSeconds/SecondFraction/DayLightSaving/StoreOperation */
} rtc_time_bin_t;

/* Read the RTC into the 7-word (28-byte) calendar window (HAL GetTime+GetDate,
 * BCD->bin). OEM rtc_fill_time_fields, 0x080380A4 — also feeds the log prefix. */
extern void rtc_fill_time_fields(uint8_t *buf);

/* Calendar -> epoch seconds. The OEM reloads the filled window as 7 by-value
 * words and tail-calls this. OEM converter at 0x08037F9C (date math, unsourced). */
extern uint32_t rtc_calendar_to_epoch(uint32_t w0, uint32_t w1, uint32_t w2,
                                      uint32_t w3, uint32_t w4, uint32_t w5,
                                      uint32_t w6);

/* Epoch seconds -> broken-down calendar (wide layout). OEM converter at 0x08038110. */
extern void rtc_epoch_to_calendar(rtc_calendar_t *out, uint32_t epoch);

/* CubeF4 RTC HAL date/time setters (RTC_FORMAT_BIN == 0). OEM HAL_RTC_SetDate
 * (0x08023042) / HAL_RTC_SetTime (0x08022F44). Return 0 on success. */
extern int hal_rtc_set_date(void *hrtc, const void *sdate, uint32_t fmt);
extern int hal_rtc_set_time(void *hrtc, const void *stime, uint32_t fmt);

uint32_t rtc_now_epoch_seconds(void)
{
    uint32_t buf[7];   /* the 28-byte calendar window, reloaded as 7 words */

    rtc_fill_time_fields((uint8_t *)buf);
    return rtc_calendar_to_epoch(buf[0], buf[1], buf[2], buf[3],
                                 buf[4], buf[5], buf[6]);
}

void rtc_set_from_unix_time(uint32_t epoch)
{
    rtc_calendar_t cal;
    rtc_date_bin_t date;
    rtc_time_bin_t time;
    uint32_t days, weekday;

    rtc_epoch_to_calendar(&cal, epoch);

    /* echo the human-readable date + time to the console (before the RTC write) */
    g_log_func("SET %02d-%02d-20%02d ", cal.day, cal.month, cal.year);
    g_log_func("%02d:%02d:%02d\r\n", cal.hours, cal.minutes, cal.seconds);

    /* weekday is derived straight from the epoch: 1970-01-01 was a Thursday (4) */
    days    = epoch / 86400u;
    weekday = (days + 4u) % 7u;

    date.weekday = (uint8_t)weekday;
    date.month   = cal.month;
    date.date    = cal.day;
    date.year    = cal.year;

    time.hours       = cal.hours;
    time.minutes     = cal.minutes;
    time.seconds     = cal.seconds;
    time.time_format = 0;
    time.pad[0] = 0;
    time.pad[1] = 0;
    time.pad[2] = 0;
    time.pad[3] = 0;

    if (hal_rtc_set_date(RTC_HANDLE, &date, 0) != 0) {
        g_log_func(" ERR set date\r\n");
    }
    if (hal_rtc_set_time(RTC_HANDLE, &time, 0) != 0) {
        g_log_func(" ERR set time\r\n");
    }
}
