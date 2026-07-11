#include <stdint.h>

#include "log.h"
#include "rtc.h"
#include "systick.h"   /* systick_now — wake-up timer disable poll timeout */
#include "panic.h"     /* Error_Handler */

/*
 * rtc.c — STM32 RTC <-> Unix-epoch helpers for the main controller.
 *
 * The HAL register I/O (HAL_RTC_Get/SetTime/Date) is kept as named externs; the
 * calendar<->epoch date math (rtc_calendar_to_epoch / rtc_epoch_to_calendar) and
 * its leaf helpers (is_leap_year / days_in_year) are sourced here. The OEM emits
 * those converters with magic-reciprocal multiplies for the /60,/3600,/86400,/7
 * and /100,/400 divisions; they are modelled as the plain divisions/modulos they
 * implement (behaviour-equivalent over the full uint32 range the RTC feeds in),
 * verified against the live disassembly at 0x08037F9C / 0x08038110.
 *
 * The shared RTC HAL handle lives at SRAM 0x200099E4. rtc_calendar_t (the wide
 * 0x18-byte broken-down calendar: hours@0/min@1/sec@2, month@0x15/day@0x16/
 * year@0x17) is declared in rtc.h and shared with log.c's log_buffer_dump.
 */

#define RTC_HANDLE           ((void *)0x200099e4u)
#define RTC_SECONDS_PER_DAY  86400u            /* OEM 0x08038024 */

/* RTC_DateTypeDef / RTC_TimeTypeDef heads (RTC_FORMAT_BIN: fields are binary), as
 * the CubeF4 HAL getters/setters read/write them. */
typedef struct { uint8_t weekday, month, date, year; } rtc_date_bin_t;
typedef struct {
    uint8_t  hours, minutes, seconds, time_format;
    uint32_t pad[4];   /* SubSeconds / SecondFraction / DayLightSaving / StoreOperation */
} rtc_time_bin_t;

/* Days per month, [is_leap][month] — OEM table @ flash 0x0804F340 (0xC stride). */
static const uint8_t k_days_in_month[2][12] = {
    /* non-leap */ { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
    /* leap     */ { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
};

/* CubeF4 RTC HAL (RTC_FORMAT_BIN == 0). OEM: HAL_RTC_GetTime 0x0802312E,
 * HAL_RTC_GetDate 0x0802317E, HAL_RTC_SetDate 0x08023042, HAL_RTC_SetTime
 * 0x08022F44. All return 0 on success. */
extern int hal_rtc_get_time(void *hrtc, void *stime, uint32_t fmt);
extern int hal_rtc_get_date(void *hrtc, void *sdate, uint32_t fmt);
extern int hal_rtc_set_date(void *hrtc, const void *sdate, uint32_t fmt);
extern int hal_rtc_set_time(void *hrtc, const void *stime, uint32_t fmt);

/* Gregorian leap-year predicate (OEM 0x08037F48): divisible by 4 and (not by 100
 * or by 400). The OEM strength-reduces the %100/%400 to the 0x51EB851F reciprocal. */
int is_leap_year(uint32_t year)
{
    if ((year & 3u) != 0u) {
        return 0;
    }
    if ((year % 100u) != 0u) {
        return 1;
    }
    return (year % 400u) == 0u ? 1 : 0;
}

/* Days in a Gregorian year (OEM 0x08037F88): 366 if leap else 365. */
int days_in_year(uint32_t year)
{
    return is_leap_year(year) ? 366 : 365;
}

/* Read the RTC into the wide 0x18-byte calendar window (OEM 0x080380A4):
 * buf[0]=hours, buf[1]=minutes, buf[2]=seconds, buf[0x15]=month, buf[0x16]=day,
 * buf[0x17]=year(2-digit). Consumed by log_print_timestamp_prefix and reloaded by
 * rtc_now_epoch_seconds as the 7 by-value words handed to the converter. */
void rtc_fill_time_fields(uint8_t *buf)
{
    rtc_time_bin_t t;
    rtc_date_bin_t d;

    hal_rtc_get_time(RTC_HANDLE, &t, 0);   /* format 0 == RTC_FORMAT_BIN */
    hal_rtc_get_date(RTC_HANDLE, &d, 0);

    buf[0x16] = d.date;     /* day */
    buf[0x15] = d.month;
    buf[0x17] = d.year;
    buf[0]    = t.hours;
    buf[1]    = t.minutes;
    buf[2]    = t.seconds;
}

/* Packed RTC calendar window -> Unix epoch seconds (OEM 0x08037F9C). Only words 0
 * (time) and 5 (date) are consumed; the other five are kept as parameters to
 * preserve the OEM 7-word by-value ABI. w0: byte0=hours, byte1=minutes,
 * byte2=seconds. w5: byte1=month(1..12), byte2=day(1..31), byte3=year-2000. */
uint32_t rtc_calendar_to_epoch(uint32_t w0, uint32_t w1, uint32_t w2,
                               uint32_t w3, uint32_t w4, uint32_t w5,
                               uint32_t w6)
{
    (void)w1; (void)w2; (void)w3; (void)w4; (void)w6;

    uint16_t month_idx = (uint16_t)(((w5 >> 8) & 0xff) - 1);     /* month - 1 */
    uint16_t day_m1    = (uint16_t)(((w5 >> 0x10) & 0xff) - 1);  /* day - 1   */
    uint16_t year      = (uint16_t)(((w5 >> 0x18) & 0xff) + 2000);

    int32_t secs = (int32_t)RTC_SECONDS_PER_DAY * (int32_t)day_m1
                 + (int32_t)(w0 & 0xff) * 0xe10          /* hours   * 3600 */
                 + (int32_t)((w0 >> 8) & 0xff) * 0x3c    /* minutes * 60   */
                 + (int32_t)((w0 >> 0x10) & 0xff);       /* seconds        */

    /* whole months elapsed this year */
    while (month_idx != 0) {
        month_idx = (uint16_t)(month_idx - 1);
        secs += (int32_t)RTC_SECONDS_PER_DAY
              * (int32_t)k_days_in_month[is_leap_year(year)][month_idx];
    }
    /* whole years from 1970 up to (year-1) */
    while (year > 0x7b2u) {                  /* 0x7b2 == 1970 */
        year = (uint16_t)(year - 1);
        secs += (int32_t)RTC_SECONDS_PER_DAY * (int32_t)days_in_year(year);
    }
    return (uint32_t)secs;
}

/* Unix epoch seconds -> wide broken-down calendar (OEM 0x08038110), the gmtime-
 * like inverse of rtc_calendar_to_epoch. out fields: [0]hours [1]minutes
 * [2]seconds [0x14]weekday((days+4)%7, 0=Sunday) [0x15]month(1-based)
 * [0x16]day(1-based) [0x17]year as (char)((char)walked_year + '0') — the OEM's
 * exact 2-digit-year byte-truncation quirk (e.g. 2021 -> 0x15). */
void rtc_epoch_to_calendar(rtc_calendar_t *out, uint32_t epoch)
{
    uint8_t *o    = (uint8_t *)out;
    uint32_t days = epoch / RTC_SECONDS_PER_DAY;
    uint32_t secs = epoch - days * RTC_SECONDS_PER_DAY;
    uint32_t year;
    uint32_t dim;
    uint8_t  month;

    o[2]    = (uint8_t)(secs % 60u);            /* seconds */
    o[1]    = (uint8_t)((secs % 3600u) / 60u);  /* minutes */
    o[0]    = (uint8_t)(secs / 3600u);          /* hours   */
    o[0x14] = (uint8_t)((days + 4u) % 7u);      /* weekday, 0 = Sunday */

    year = 0x7b2u;                              /* 1970 */
    while ((uint32_t)days_in_year(year) <= days) {
        days -= (uint32_t)days_in_year(year);
        year  = (uint16_t)(year + 1);
    }
    o[0x17] = (uint8_t)((char)year + '0');      /* 2-digit-year truncation quirk */

    o[0x15] = 0;
    for (;;) {
        month = o[0x15];
        dim   = k_days_in_month[is_leap_year(year)][month];
        if (days < dim) {
            break;
        }
        days   -= dim;
        o[0x15] = (uint8_t)(month + 1);
    }
    o[0x15] = (uint8_t)(month + 1);             /* month, 1-based */
    o[0x16] = (uint8_t)((uint8_t)days + 1);     /* day-of-month, 1-based */
}

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

    days    = epoch / 86400u;
    weekday = (days + 4u) % 7u;   /* 1970-01-01 was a Thursday (4) */

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

/* ── RTC MSP / wake-up glue ─────────────────────────────────────────────────── */

extern void nvic_set_priority(int32_t irq_n, uint32_t preempt, uint32_t sub); /* 0x08027078 */
extern void nvic_enable_irq(int32_t irq_n);                                   /* 0x080270E0 */

/* RTC base + the peripheral bit-band alias of RCC_BDCR bit 15 (RTCEN). The
 * RTC_WKUP line is NVIC IRQ 3 on the STM32F4. */
#define RTC_INSTANCE       0x40002800u
#define RCC_BDCR_RTCEN_BB  (*(volatile uint32_t *)0x42470E3Cu)
#define RTC_WKUP_IRQn      3

/* rtc_init (OEM 0x0803802C — the HAL_RTC_Init wrapper): set Instance = RTC and the
 * calendar prescalers (AsynchPrediv 127, SynchPrediv 249 -> 1 Hz ck_spre from a
 * 32.768 kHz clock; 24 h format, no calibration output), then HAL_RTC_Init (which
 * chains rtc_msp_init). Fatal Error_Handler on failure. */
extern int HAL_RTC_Init(void *hrtc);   /* 0x08022E6A */

void rtc_init(void)
{
    volatile uint32_t *hrtc = (volatile uint32_t *)RTC_HANDLE;

    hrtc[0] = 0x40002800u;   /* Instance = RTC */
    hrtc[1] = 0;             /* Init.HourFormat = 24h */
    hrtc[2] = 0x7f;          /* Init.AsynchPrediv = 127 */
    hrtc[3] = 0xf9;          /* Init.SynchPrediv  = 249  */
    hrtc[4] = 0;             /* Init.OutPut         = NONE */
    hrtc[5] = 0;             /* Init.OutPutPolarity = HIGH */
    hrtc[6] = 0;             /* Init.OutPutType     = OPENDRAIN */
    if (HAL_RTC_Init((void *)RTC_HANDLE) != 0) {
        Error_Handler();
    }
}

/* HAL_RTC_MspInit (OEM 0x0803805C), called from HAL_RTC_Init for the RTC handle.
 * Enables the RTC peripheral clock and the RTC wake-up interrupt line. */
void rtc_msp_init(void *hrtc)
{
    if (*(volatile uint32_t *)hrtc != RTC_INSTANCE) {   /* hrtc->Instance == RTC */
        return;
    }
    RCC_BDCR_RTCEN_BB = 1u;
    nvic_set_priority(RTC_WKUP_IRQn, 0, 0);
    nvic_enable_irq(RTC_WKUP_IRQn);
}

/* RTC wake-up event callback (OEM 0x08038A04), invoked from rtc_wakeup_irq_handler.
 * Sets the "wake-up fired" flag the super-loop polls. The flag's address is held
 * in field +0x24 of the RTC app context at 0x20000094. */
void rtc_wakeup_event_cb(void)
{
    *(*(volatile uint32_t **)(0x20000094u + 0x24u)) = 1u;
}

/* HAL_RTCEx_SetWakeUpTimer_IT (OEM 0x08026EA0) — program RTC_WUTR + enable the
 * wake-up timer/interrupt; kept as a named extern (HAL core, body deferred). */
extern int rtc_wakeup_timer_set(void *hrtc, uint16_t counter, uint32_t clock_sel);

/* rtc_wakeup_timer_disable (OEM 0x08026FB0 — HAL_RTCEx_DeactivateWakeUpTimer):
 * unlock RTC write protection, clear RTC_CR.WUTE/WUTIE, then wait (≤1 s) for
 * RTC_ISR.WUTWF before re-locking. hrtc layout: +0 Instance, +0x1C Lock,
 * +0x1D State. Returns 0 = OK, 2 = busy/locked, 3 = timeout. */
int rtc_wakeup_timer_disable(void *hrtc)
{
    uint8_t *h = (uint8_t *)hrtc;
    volatile uint32_t *rtc = *(volatile uint32_t **)hrtc;   /* hrtc->Instance (RTC) */
    uint32_t start;

    if (h[0x1c] == 1) {                 /* HAL_LOCKED */
        return 2;
    }
    h[0x1c] = 1;                        /* lock */
    h[0x1d] = 2;                        /* state = BUSY */
    rtc[0x24 / 4] = 0xca;               /* RTC_WPR: disable write protection */
    rtc[0x24 / 4] = 0x53;
    rtc[0x08 / 4] &= ~0x400u;           /* RTC_CR.WUTE  = 0 (stop wake-up timer) */
    rtc[0x08 / 4] &= ~0x4000u;          /* RTC_CR.WUTIE = 0 (mask wake-up IRQ)   */
    start = systick_now();
    do {
        if (rtc[0x0c / 4] & 4u) {       /* RTC_ISR.WUTWF: safe to reprogram */
            rtc[0x24 / 4] = 0xff;       /* re-enable write protection */
            h[0x1d] = 1;                /* state = READY */
            h[0x1c] = 0;                /* unlock */
            return 0;
        }
    } while (systick_now() - start < 0x3e9u);   /* ~1 s */
    rtc[0x24 / 4] = 0xff;
    h[0x1d] = 3;                        /* state = TIMEOUT */
    h[0x1c] = 0;
    return 3;
}

/* rtc_wakeup_irq_handler (OEM 0x08027020 — HAL_RTCEx_WakeUpTimerIRQHandler): on
 * RTC_ISR.WUTF, run the wake-up callback and clear the flag, then clear the RTC
 * wake-up EXTI pending bit (line 22) and return the handle to READY. */
void rtc_wakeup_irq_handler(void *hrtc)
{
    uint8_t *h = (uint8_t *)hrtc;
    volatile uint32_t *rtc = *(volatile uint32_t **)hrtc;   /* hrtc->Instance (RTC) */

    if (rtc[0x0c / 4] & 0x400u) {                            /* RTC_ISR.WUTF */
        rtc_wakeup_event_cb();
        rtc[0x0c / 4] = (rtc[0x0c / 4] & 0xffu) | 0xfffffb7fu;   /* clear WUTF */
    }
    *(volatile uint32_t *)(0x40013c00u + 0x14u) = 0x400000u;     /* EXTI->PR line 22 */
    h[0x1d] = 1;                                             /* state = READY */
}

/* rtc_set_wakeup_seconds (OEM 0x08038088) — reprogram the RTC wake-up timer to
 * fire in `seconds` (1 Hz ck_spre clock, sel = 4): deactivate, then re-arm. */
void rtc_set_wakeup_seconds(uint16_t seconds)
{
    rtc_wakeup_timer_disable(RTC_HANDLE);
    rtc_wakeup_timer_set(RTC_HANDLE, seconds, 4);
}
