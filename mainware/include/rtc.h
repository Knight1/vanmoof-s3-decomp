#ifndef MAINWARE_RTC_H
#define MAINWARE_RTC_H

#include <stdint.h>

/* STM32 RTC <-> Unix-epoch helpers (src/rtc.c). Thin wrappers over the calendar
 * <-> epoch converters and the CubeF4 RTC HAL; the RTC handle lives at SRAM
 * 0x200099E4. See docs/state-machine.md (tracking-time field, BLE 0x5567). */

/* Wide broken-down calendar (0x18 bytes) produced by the epoch<->calendar
 * converters. Shared by rtc.c and the log-buffer timestamp reformatter in log.c
 * (log_buffer_dump). */
typedef struct {
    uint8_t hours;        /* +0x00 */
    uint8_t minutes;      /* +0x01 */
    uint8_t seconds;      /* +0x02 */
    uint8_t _pad[0x12];   /* +0x03..+0x14 */
    uint8_t month;        /* +0x15 */
    uint8_t day;          /* +0x16 */
    uint8_t year;         /* +0x17  (2-digit, +2000) */
} rtc_calendar_t;

/* Epoch seconds -> broken-down calendar (wide layout). OEM converter at
 * 0x08038110 (date math, not yet sourced). */
void rtc_epoch_to_calendar(rtc_calendar_t *out, uint32_t epoch);

/* Read the RTC and return the current wall-clock time as Unix epoch seconds.
 * OEM rtc_now_epoch_seconds at 0x080380EC. */
uint32_t rtc_now_epoch_seconds(void);

/* Convert a Unix epoch (seconds) to the calendar, echo "SET dd-mm-20yy hh:mm:ss"
 * to the console logger, then program the RTC date + time. The inverse of
 * rtc_now_epoch_seconds. OEM rtc_set_from_unix_time at 0x080381D0. */
void rtc_set_from_unix_time(uint32_t epoch);

/* Initialise the RTC handle (Instance + calendar prescalers for 1 Hz) and run
 * HAL_RTC_Init. OEM rtc_init at 0x0803802C. */
void rtc_init(void);

/* HAL_RTC_MspInit (OEM 0x0803805C): enable the RTC peripheral clock (RCC_BDCR.
 * RTCEN) and the RTC_WKUP NVIC line (IRQ 3). Guarded on hrtc->Instance == RTC. */
void rtc_msp_init(void *hrtc);

/* RTC wake-up event callback (OEM 0x08038A04): latch the "wake-up fired" flag
 * polled by the super-loop. Called from rtc_wakeup_irq_handler. */
void rtc_wakeup_event_cb(void);

/* RTC wake-up timer (RTC_WKUP, NVIC IRQ 3 / EXTI line 22). */
void rtc_set_wakeup_seconds(uint16_t seconds);   /* 0x08038088 */
int  rtc_wakeup_timer_disable(void *hrtc);        /* 0x08026FB0 (HAL deactivate) */
void rtc_wakeup_irq_handler(void *hrtc);          /* 0x08027020 (HAL IRQ handler) */

#endif
