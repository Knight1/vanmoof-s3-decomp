#include "powerbankware.h"

/*
 * rtc_set — OEM FUN_080122dc.
 *
 * Unpacks the two packed date words built by the "Set RTC" command
 * (lo = sec|min<<8|hour<<16, hi = day|month<<8|year<<16) into the HAL RTC
 * time/date structs and programs the RTC (handle @ 0x200006f0). The ms tick
 * counter (0x20002614) is zeroed alongside.
 *
 * rtc_set_time = HAL_RTC_SetTime, rtc_set_date = HAL_RTC_SetDate; format 0 = BIN.
 * The shared HAL leaves (rtc_byte_to_bcd / rtc_enter_init / rtc_wait_synchro) are
 * defined at the bottom of this file.
 */

void rtc_set(uint32_t lo, uint32_t hi)
{
    void * const hrtc = (void *)0x200006f0;

    /* RTC_TimeTypeDef is 0x14 bytes: SetTime reads DayLightSaving(+0xc) and
     * StoreOperation(+0x10) and OR-s them into CR even in BIN mode, so the struct
     * must be word-aligned (Cortex-M0 word load) and those two fields cleared. The
     * OEM clears only DayLightSaving; we also clear StoreOperation, which avoids
     * reading stack garbage with the same observable result for a basic set.
     * (Set the two fields explicitly rather than `= {0}` so no memset is emitted.) */
    uint32_t time_words[5];
    uint8_t *time = (uint8_t *)time_words;
    time[0] = (uint8_t)(lo >> 16);     /* hours   */
    time[1] = (uint8_t)(lo >> 8);      /* minutes */
    time[2] = (uint8_t)lo;             /* seconds */
    time_words[3] = 0;                 /* DayLightSaving (+0xc) */
    time_words[4] = 0;                 /* StoreOperation (+0x10) */

    /* RTC_DateTypeDef leading bytes, in the order SetDate reads them. */
    uint8_t date[4];
    date[0] = 1;                       /* weekday */
    date[1] = (uint8_t)(hi >> 8);      /* month   */
    date[2] = (uint8_t)hi;             /* day     */
    date[3] = (uint8_t)(hi >> 16);     /* year    */

    *(volatile uint32_t *)0x20002614 = 0;
    rtc_set_time(hrtc, time, 0);
    rtc_set_date(hrtc, date, 0);
}

/* bcd_to_bin — OEM FUN_0801c664. Packed-BCD byte -> binary. */
uint8_t bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)((bcd & 0x0f) + (bcd >> 4) * 10);
}

/*
 * rtc_timestamp_read — OEM FUN_080121f0.
 *
 * Snapshot the RTC clock: mask the raw RTC_TR/RTC_DR registers (handle Instance
 * @ 0x200006f0; TR mask 0x007F7F7F, DR mask 0x00FFFF3F) into a scratch pair at
 * 0x20000730, BCD-decode the six fields into an 8-byte buffer at 0x20000718
 * (sec, min, hour, 0, day, month&0x1F, year, 0), and copy those two words to
 * `out`. Used by the state dispatcher to timestamp its trace records.
 */
uint32_t *rtc_timestamp_read(uint32_t *out)
{
    volatile uint8_t *raw = (volatile uint8_t *)0x20000730;   /* scratch TR/DR */
    volatile uint8_t *bin = (volatile uint8_t *)0x20000718;   /* decoded fields */
    uint32_t inst = *(volatile uint32_t *)0x200006f0;         /* RTC Instance  */

    *(volatile uint32_t *)(0x20000730 + 4) =
        *(volatile uint32_t *)(inst + 4) & 0x00ffff3fu;       /* RTC_DR */
    *(volatile uint32_t *)0x20000730 =
        *(volatile uint32_t *)(inst + 0) & 0x007f7f7fu;       /* RTC_TR */

    bin[0] = bcd_to_bin(raw[0]);            /* seconds */
    bin[1] = bcd_to_bin(raw[1]);            /* minutes */
    bin[2] = bcd_to_bin(raw[2]);            /* hours   */
    bin[3] = 0;
    raw[3] = 0;
    bin[4] = bcd_to_bin(raw[4]);            /* day     */
    raw[5] &= 0x1f;
    bin[5] = bcd_to_bin(raw[5]);            /* month   */
    bin[6] = bcd_to_bin(raw[6]);            /* year    */
    bin[7] = 0;
    raw[7] = 0;

    out[0] = *(volatile uint32_t *)0x20000718;
    out[1] = *(volatile uint32_t *)(0x20000718 + 4);
    return out;
}

/*
 * rtc_set_time — OEM FUN_0801c288 (HAL_RTC_SetTime).
 *
 * Program RTC_TR from `sTime` (RTC_TimeTypeDef: Hours[0], Minutes[1], Seconds[2],
 * TimeFormat[3], DayLightSaving[+0xc], StoreOperation[+0x10]). hrtc layout:
 * Instance@0, Lock@0x1c, State@0x1d, WPR@Instance+0x24. `format` 0 = BIN (encode
 * to BCD via RTC_ByteToBcd2), else the fields are already BCD. Returns HAL status
 * (0 OK, 1 ERROR, 2 BUSY). Widths/branches disasm-confirmed against the OEM image.
 */
int rtc_set_time(void *hrtc, uint8_t *sTime, int format)
{
    volatile uint8_t  *h    = (volatile uint8_t *)hrtc;
    volatile uint32_t *inst = (volatile uint32_t *)(*(uint32_t *)hrtc);
    uint32_t tr = 0;

    if (h[0x1c] == 1) {
        return 2;
    }
    h[0x1c] = 1;                                       /* Lock = LOCKED  */
    h[0x1d] = 2;                                       /* State = BUSY   */

    if (format == 0) {                                 /* BIN -> BCD */
        if ((inst[2] & 0x40) == 0) {                   /* CR.FMT: 24h */
            sTime[3] = 0;
        }
        tr = ((uint32_t)sTime[3] << 16)
           | (rtc_byte_to_bcd(sTime[0]) << 16)
           | (rtc_byte_to_bcd(sTime[1]) << 8)
           |  rtc_byte_to_bcd(sTime[2]);
    } else {                                           /* already BCD */
        if ((inst[2] & 0x40) == 0) {
            sTime[3] = 0;
        } else {
            bcd_to_bin(sTime[0]);                      /* OEM assert_param check */
        }
        tr = ((uint32_t)sTime[3] << 16)
           |  (uint32_t)sTime[2]
           | ((uint32_t)sTime[1] << 8)
           | ((uint32_t)sTime[0] << 16);
    }

    inst[9] = 0xca;                                    /* WPR: unlock */
    inst[9] = 0x53;
    if (rtc_enter_init(hrtc) != 0) {                     /* enter init mode */
        inst[9] = 0xff;
        h[0x1d] = 4;
        h[0x1c] = 0;
        return 1;
    }

    inst[0] = tr & 0x007f7f7fu;                        /* RTC_TR */
    inst[2] = inst[2] & 0xfffbffffu;                   /* CR &= ~BKP */
    inst[2] = *(uint32_t *)(sTime + 0xc) | *(uint32_t *)(sTime + 0x10) | inst[2];
    inst[3] = inst[3] & 0xffffff7fu;                   /* ISR: exit init */

    if ((inst[2] & 0x20) == 0) {                       /* !BYPSHAD */
        if (rtc_wait_synchro(hrtc) != 0) {
            inst[9] = 0xff;
            h[0x1d] = 4;
            h[0x1c] = 0;
            return 1;
        }
    }

    inst[9] = 0xff;                                    /* WPR: lock */
    h[0x1d] = 1;                                       /* State = READY */
    h[0x1c] = 0;                                       /* Lock = UNLOCKED */
    return 0;
}

/*
 * rtc_set_date — OEM FUN_0801c410 (HAL_RTC_SetDate).
 *
 * Program RTC_DR from `sDate` (RTC_DateTypeDef: WeekDay[0], Month[1], Day[2],
 * Year[3]). Same hrtc layout and status returns as rtc_set_time. In BIN mode it
 * also applies the HAL month-tens fix-up (`& 0x10` set -> strip and add 10).
 */
int rtc_set_date(void *hrtc, uint8_t *sDate, int format)
{
    volatile uint8_t  *h    = (volatile uint8_t *)hrtc;
    volatile uint32_t *inst = (volatile uint32_t *)(*(uint32_t *)hrtc);
    uint32_t dr = 0;

    if (h[0x1c] == 1) {
        return 2;
    }
    h[0x1c] = 1;
    h[0x1d] = 2;

    if (format == 0 && (sDate[1] & 0x10) != 0) {       /* month tens fix-up */
        sDate[1] = (uint8_t)((sDate[1] & 0xef) + 10);
    }

    if (format == 0) {                                 /* BIN -> BCD */
        dr = ((uint32_t)sDate[0] << 13)                /* WeekDay */
           | (rtc_byte_to_bcd(sDate[3]) << 16)            /* Year */
           | (rtc_byte_to_bcd(sDate[1]) << 8)             /* Month */
           |  rtc_byte_to_bcd(sDate[2]);                  /* Day */
    } else {                                           /* already BCD */
        bcd_to_bin(sDate[1]);                          /* OEM assert_param checks */
        bcd_to_bin(sDate[2]);
        dr = ((uint32_t)sDate[0] << 13)
           |  (uint32_t)sDate[2]
           | ((uint32_t)sDate[1] << 8)
           | ((uint32_t)sDate[3] << 16);
    }

    inst[9] = 0xca;                                    /* WPR: unlock */
    inst[9] = 0x53;
    if (rtc_enter_init(hrtc) != 0) {
        inst[9] = 0xff;
        h[0x1d] = 4;
        h[0x1c] = 0;
        return 1;
    }

    inst[1] = dr & 0x00ffff3fu;                        /* RTC_DR */
    inst[3] = inst[3] & 0xffffff7fu;                   /* ISR: exit init */

    if ((inst[2] & 0x20) == 0) {
        if (rtc_wait_synchro(hrtc) != 0) {
            inst[9] = 0xff;
            h[0x1d] = 4;
            h[0x1c] = 0;
            return 1;
        }
    }

    inst[9] = 0xff;
    h[0x1d] = 1;
    h[0x1c] = 0;
    return 0;
}

/* rtc_byte_to_bcd — OEM FUN_0801c622 (RTC_ByteToBcd2). Binary -> packed BCD via
 * repeated subtraction (no divide). Disasm-confirmed. */
uint32_t rtc_byte_to_bcd(uint8_t val)
{
    uint32_t tens = 0;
    while (val > 9) {
        tens++;
        val = (uint8_t)(val - 10);
    }
    return (uint8_t)((tens << 4) | val);
}

/*
 * rtc_enter_init — OEM FUN_0801c5ca (RTC_EnterInitMode).
 *
 * If the calendar isn't already in init mode (ISR.INITF, Instance +0xc bit6),
 * request it (ISR = 0xffffffff) and poll INITF with a 1000-tick (tick_get)
 * timeout. Returns 0 on success, 3 on timeout.
 */
int rtc_enter_init(void *hrtc)
{
    volatile uint32_t *inst = (volatile uint32_t *)(*(uint32_t *)hrtc);

    if ((inst[3] & 0x40) == 0) {                       /* ISR.INITF clear */
        inst[3] = 0xffffffffu;                         /* request init mode */
        uint32_t start = tick_get();
        while ((inst[3] & 0x40) == 0) {
            if ((uint32_t)(tick_get() - start) >= 0x3e9) {
                return 3;
            }
        }
    }
    return 0;
}

/*
 * rtc_wait_synchro — OEM FUN_0801c578 (RTC_WaitForSynchro).
 *
 * Clear ISR.RSF (RTC_RSF_MASK 0xffffff5f, which also drops INIT), then poll for
 * the shadow registers to resynchronise (ISR.RSF, Instance +0xc bit5) with a
 * 1000-tick timeout. Returns 0 on success, 3 on timeout.
 */
int rtc_wait_synchro(void *hrtc)
{
    volatile uint32_t *inst = (volatile uint32_t *)(*(uint32_t *)hrtc);

    inst[3] = inst[3] & 0xffffff5fu;                   /* clear RSF */
    uint32_t start = tick_get();
    while ((inst[3] & 0x20) == 0) {                    /* wait for RSF */
        if ((uint32_t)(tick_get() - start) >= 0x3e9) {
            return 3;
        }
    }
    return 0;
}

/* rtc_msp_init — OEM FUN_0801c278 (HAL_RTC_MspInit): empty weak stub. */
void rtc_msp_init(void *hrtc)
{
    (void)hrtc;
}

/*
 * hal_rtc_init — OEM FUN_0801c150 (HAL_RTC_Init).
 *
 * On first use run the MSP init, then unlock the write protection (WPR 0xCA/
 * 0x53), enter init mode, program CR (HourFormat|OutPut|OutPutPolarity) and PRER
 * (Sync | Async<<16), exit init mode (clear ISR.INIT) and — unless CR.BYPSHAD is
 * set — wait for the shadow registers to resync; finally set the output type
 * (TAFCR), re-lock and mark the handle READY. Handle byte offsets: Lock +0x1c,
 * State +0x1d, Init.HourFormat +0x04, AsynchPrediv +0x08, SynchPrediv +0x0c,
 * OutPut +0x10, OutPutPolarity +0x14, OutPutType +0x18. RTC registers: CR +0x08,
 * ISR +0x0c, PRER +0x10, WPR +0x24, TAFCR +0x40. Returns 0 = OK, 1 = ERROR.
 * Offsets/widths/masks disasm-confirmed against the OEM image.
 */
int hal_rtc_init(void *handle)
{
    if (handle == NULL) {
        return 1;
    }
    uint32_t *hrtc = (uint32_t *)handle;
    uint8_t  *h    = (uint8_t  *)handle;

    if (h[0x1d] == 0) {                          /* State == RESET */
        h[0x1c] = 0;                             /* Lock = UNLOCKED */
        rtc_msp_init(handle);                    /* HAL_RTC_MspInit */
    }
    h[0x1d] = 2;                                 /* State = BUSY */

    volatile uint32_t *inst = (volatile uint32_t *)hrtc[0];   /* RTC Instance */
    inst[9] = 0xca;                              /* WPR (+0x24): unlock sequence */
    inst[9] = 0x53;

    if (rtc_enter_init(handle) != 0) {
        inst[9] = 0xff;
        h[0x1d] = 4;                             /* State = ERROR */
        return 1;
    }

    inst[2] &= 0xff8fffbfu;                      /* CR (+0x08): clear FMT|OSEL|POL */
    inst[2] |= hrtc[1] | hrtc[4] | hrtc[5];      /* CR |= HourFormat|OutPut|OutPutPolarity */
    inst[4]  = hrtc[3];                          /* PRER (+0x10) = SynchPrediv */
    inst[4] |= hrtc[2] << 16;                    /* PRER |= AsynchPrediv << 16 */
    inst[3] &= 0xffffff7fu;                      /* ISR (+0x0c): clear INIT (exit init mode) */

    if ((inst[2] & 0x20u) == 0 && rtc_wait_synchro(handle) != 0) {   /* !BYPSHAD */
        inst[9] = 0xff;
        h[0x1d] = 4;
        return 1;
    }

    inst[16] &= 0xfffbffffu;                     /* TAFCR (+0x40): clear output-type bit */
    inst[16] |= hrtc[6];                         /* TAFCR |= OutPutType */
    inst[9]   = 0xff;                            /* WPR: re-lock */
    h[0x1d]   = 1;                               /* State = READY */
    return 0;
}
