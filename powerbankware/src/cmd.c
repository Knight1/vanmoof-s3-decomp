#include "powerbankware.h"

/*
 * Text command parser — OEM FUN_08014ae8.
 *
 * Parses an ASCII console line of the form "NAME" or "NAME=HEXVALUE", matches
 * NAME against the 20-entry command table (OEM flash @ 0x0801ed24, 47-byte
 * slots {idx, name_len, name[...]}), and dispatches to the per-command handler
 * via the jump table @ 0x0801f0d0. Parse errors and unknown commands fall to
 * the idx-0 slot, which (like the OEM error helper FUN_080159f2) is a no-op.
 *
 * Cross-checked vs batteryware command_parser + cmd_entry_t — same 47-byte
 * table-slot layout and name-match-then-dispatch shape.
 *
 * The OEM handlers are reached by a tail branch and share this function's
 * stack frame (the parsed value buffer). In C they're given that buffer
 * explicitly as (value, value_len); each lands in its own pass.
 */

/* The 20 per-command handlers (idx 1..20) are defined at the bottom of this
 * file. In the OEM they are continuations of FUN_08014ae8 sharing its stack
 * frame; here each takes the parsed (val, vlen). */
static void cmd_who(const char *val, uint8_t vlen);
static void cmd_reset_bms(const char *val, uint8_t vlen);
static void cmd_pf(const char *val, uint8_t vlen);
static void cmd_upgrade_ap(const char *val, uint8_t vlen);
static void cmd_upgrade_bl(const char *val, uint8_t vlen);
static void cmd_chg_cal_set(const char *val, uint8_t vlen);
static void cmd_chg_cal_get(const char *val, uint8_t vlen);
static void cmd_dsg_cal_set(const char *val, uint8_t vlen);
static void cmd_dsg_cal_get(const char *val, uint8_t vlen);
static void cmd_reset_esn(const char *val, uint8_t vlen);
static void cmd_set_rtc(const char *val, uint8_t vlen);
static void cmd_hw_version(const char *val, uint8_t vlen);
static void cmd_shipping(const char *val, uint8_t vlen);
static void cmd_v_offset(const char *val, uint8_t vlen);
static void cmd_i_offset(const char *val, uint8_t vlen);
static void cmd_set_rsoc(const char *val, uint8_t vlen);
static void cmd_set_fcc(const char *val, uint8_t vlen);
static void cmd_ts0_offset(const char *val, uint8_t vlen);
static void cmd_ts1_offset(const char *val, uint8_t vlen);
static void cmd_ts2_offset(const char *val, uint8_t vlen);

typedef struct {
    uint8_t     idx;
    uint8_t     name_len;
    const char *name;
} cmd_entry_t;

/* Command table (OEM 0x0801ed24), in slot order — idx == slot+1. */
static const cmd_entry_t s_cmd_table[20] = {
    {  1,  4, "Who?"       },
    {  2,  9, "Reset BMS"  },
    {  3,  2, "PF"         },
    {  4, 10, "Upgrade AP" },
    {  5, 10, "Upgrade BL" },
    {  6,  7, "CHG CAL"    },
    {  7,  8, "CHG CAL?"   },
    {  8,  7, "DSG CAL"    },
    {  9,  8, "DSG CAL?"   },
    { 10,  9, "Reset ESN"  },
    { 11,  7, "Set RTC"    },
    { 12, 10, "HW Version" },
    { 13,  8, "Shipping"   },
    { 14,  8, "V Offset"   },
    { 15,  8, "I Offset"   },
    { 16,  8, "Set RSOC"   },
    { 17,  7, "Set FCC"    },
    { 18, 10, "TS0 Offset" },
    { 19, 10, "TS1 Offset" },
    { 20, 10, "TS2 Offset" },
};

void cmd_dispatch(uint8_t channel, void *line_ptr, uint8_t len)
{
    const char *line = (const char *)line_ptr;
    char    name[21];
    char    val[21];
    uint8_t name_len = 0;
    uint8_t val_len  = 0;
    char    state    = 0;       /* 0 parsing, 3 got name, 4 name-err, 5 done, 6 val-err */
    uint8_t i        = 0;

    (void)channel;

    /* Phase 1 — name up to '=' (or end of line). */
    while (i < len && state == 0) {
        uint8_t c = (uint8_t)line[i];
        if (c == '=') {
            i++;
            state = 3;
        } else if (c < 0x20 || (int8_t)c < 0) {
            state = 4;
        } else {
            name[name_len++] = line[i];
            if (name_len > 0x14) {
                state = 4;
            }
            i++;
            if (i == len) {
                state = 3;
            }
        }
    }

    /* Phase 2 — hex value after '='. */
    if (state == 3) {
        state = 0;
        if (i == len) {
            state = 5;                       /* name only, no value */
        } else {
            while (i < len && state == 0) {
                uint8_t c = (uint8_t)line[i];
                if ((c < '0' || c > '9') && (c < 'A' || c > 'F')) {
                    state = 6;
                } else {
                    val[val_len++] = line[i];
                    if (val_len > 0x14) {
                        state = 6;
                    }
                    i++;
                    if (i == len) {
                        state = 5;
                    }
                }
            }
        }
    }

    /* Look up NAME (errors leave idx 0 -> the no-op slot). */
    uint8_t idx = 0;
    for (uint8_t t = 0; t < 20; t++) {
        if (name_len != s_cmd_table[t].name_len) {
            continue;
        }
        uint8_t m = 0;
        for (uint8_t k = 0; k < name_len; k++) {
            if (name[k] == s_cmd_table[t].name[k]) {
                m++;
            }
        }
        if (m == name_len) {
            idx = s_cmd_table[t].idx;
            break;
        }
    }

    switch (idx) {
    case 1:  cmd_who(val, val_len);         break;
    case 2:  cmd_reset_bms(val, val_len);   break;
    case 3:  cmd_pf(val, val_len);          break;
    case 4:  cmd_upgrade_ap(val, val_len);  break;
    case 5:  cmd_upgrade_bl(val, val_len);  break;
    case 6:  cmd_chg_cal_set(val, val_len); break;
    case 7:  cmd_chg_cal_get(val, val_len); break;
    case 8:  cmd_dsg_cal_set(val, val_len); break;
    case 9:  cmd_dsg_cal_get(val, val_len); break;
    case 10: cmd_reset_esn(val, val_len);   break;
    case 11: cmd_set_rtc(val, val_len);     break;
    case 12: cmd_hw_version(val, val_len);  break;
    case 13: cmd_shipping(val, val_len);    break;
    case 14: cmd_v_offset(val, val_len);    break;
    case 15: cmd_i_offset(val, val_len);    break;
    case 16: cmd_set_rsoc(val, val_len);    break;
    case 17: cmd_set_fcc(val, val_len);     break;
    case 18: cmd_ts0_offset(val, val_len);  break;
    case 19: cmd_ts1_offset(val, val_len);  break;
    case 20: cmd_ts2_offset(val, val_len);  break;
    default: break;                          /* idx 0 — no-op (OEM FUN_080159f2) */
    }
}

/* ----------------------------------------------------------------------- *
 * Command handlers.  Most write into the BMS config/record struct at
 * 0x200004d0 and acknowledge via log_print on channel 2 (vestigial). The
 * OEM "error" tails are nop-slides to the function return, so every
 * validation failure here is simply `return`.
 */

#define CFG ((volatile uint8_t *)0x200004d0)
#define CFG8(off)  (*(volatile uint8_t  *)(CFG + (off)))
#define CFG16(off) (*(volatile uint16_t *)(CFG + (off)))
#define CFG32(off) (*(volatile uint32_t *)(CFG + (off)))

static volatile uint16_t * const s_mode = (volatile uint16_t *)0x200006a0;

extern const char s_iam_ap2[], s_done[], s_ok[], s_chg_cal_q[], s_dsg_cal_q[],
                  s_write_rtc[], s_charger_v_mv[];


/* '0'-'9'/'A'-'F' -> 0..15 (OEM FUN_08013686). */
static uint8_t hex_nibble(uint8_t c)
{
    return (uint8_t)(c < 0x3a ? (c & 0xf) : (c - 0x37));
}

/* Decimal accumulate (OEM FUN_08015a1c) — note hex digits are *allowed* by
 * the parser but combined base-10. */
static uint32_t cmd_atoi(const char *buf, uint8_t len)
{
    uint32_t r = hex_nibble((uint8_t)buf[0]);
    for (uint8_t i = 1; i < len; i++) {
        if (r != 0) {
            r *= 10;
        }
        r += hex_nibble((uint8_t)buf[i]);
    }
    return r;
}

/* Hex accumulate (OEM FUN_08015aa2) — used for the HW version + RTC pairs. */
static uint32_t cmd_htoi(const char *buf, uint8_t len)
{
    uint32_t r = hex_nibble((uint8_t)buf[0]);
    for (uint8_t i = 1; i < len; i++) {
        r <<= 4;
        r |= hex_nibble((uint8_t)buf[i]);
    }
    return r;
}

static void cmd_who(const char *val, uint8_t vlen)
{
    (void)val;
    if (vlen != 0) return;
    log_print(2, s_iam_ap2);
}

static void cmd_reset_bms(const char *val, uint8_t vlen)
{
    (void)val;
    if (vlen != 0) return;
    bms_config_reset();
    mem_zero((void *)0x200005b0, 0x40);
    for (int i = 0; i <= 0x3e7; i++) {
        errlog_erase(i);
    }
    log_print(2, s_done);
}

static void cmd_pf(const char *val, uint8_t vlen)
{
    if (vlen != 1) return;
    if (CFG8(0x5c) == 0) return;
    if (val[0] != '0') return;
    gpio_bit_write(0x48000400u, 0x80, 0);     /* GPIOB PB7 = 0 */
    CFG8(0x5c) = 0;
    if (*(volatile uint32_t *)0x2000042c > 0x4e1f) {
        boot_mode_enter(3);
    } else {
        boot_mode_enter(1);
    }
}

static void cmd_upgrade_ap(const char *val, uint8_t vlen)
{
    (void)val; (void)vlen;
    *s_mode |= 2;
    log_print(2, s_ok);
}

static void cmd_upgrade_bl(const char *val, uint8_t vlen)
{
    (void)val; (void)vlen;
    *s_mode |= 1;
    log_print(2, s_ok);
}

/* Shared CHG/DSG calibration-arm: latch the entered target and select the
 * cal mode via the mode bits (bit14 = CHG, bit15 = DSG). */
static void cmd_cal_set(const char *val, uint8_t vlen, uint8_t status_off,
                        volatile uint16_t *result_cell, int is_chg)
{
    if (vlen == 0) return;
    uint32_t v = cmd_atoi(val, vlen);
    CFG16(status_off) = 0;
    *result_cell = 0;
    *(volatile uint8_t  *)0x2000022c = 0;
    *(volatile uint32_t *)0x20000250 = 0;
    *(volatile uint32_t *)0x20000228 = v;
    if (is_chg) {
        *s_mode = (uint16_t)((*s_mode | 0x4000) & ~0x8000u);
    } else {
        *s_mode = (uint16_t)((*s_mode | 0x8000) & ~0x4000u);
    }
    log_print(2, s_ok);
}

static void cmd_chg_cal_set(const char *val, uint8_t vlen)
{
    cmd_cal_set(val, vlen, 0x56, (volatile uint16_t *)0x2000023e, 1);
}

static void cmd_chg_cal_get(const char *val, uint8_t vlen)
{
    (void)val;
    if (vlen != 0) return;
    log_print(2, s_chg_cal_q, *(volatile uint16_t *)0x2000023e);
}

static void cmd_dsg_cal_set(const char *val, uint8_t vlen)
{
    cmd_cal_set(val, vlen, 0x58, (volatile uint16_t *)0x2000023c, 0);
}

static void cmd_dsg_cal_get(const char *val, uint8_t vlen)
{
    (void)val;
    if (vlen != 0) return;
    log_print(2, s_dsg_cal_q, *(volatile uint16_t *)0x2000023c);
}

static void cmd_reset_esn(const char *val, uint8_t vlen)
{
    (void)val;
    if (vlen != 0) return;
    mem_zero((void *)0x2000052e, 14);
    mem_zero((void *)0x2000053c, 4);
    fedl5236_record_save();
    log_print(2, s_done);
}

static void cmd_set_rtc(const char *val, uint8_t vlen)
{
    if (vlen != 12) return;

    uint8_t date[8] = {0};      /* sec,min,hour,0,day,month,year,0 */
    date[6] = (uint8_t)(date[6] + cmd_htoi(val + 0,  2));   /* year  */
    date[5] = (uint8_t)(date[5] + cmd_htoi(val + 2,  2));   /* month */
    date[4] = (uint8_t)(date[4] + cmd_htoi(val + 4,  2));   /* day   */
    date[2] = (uint8_t)(date[2] + cmd_htoi(val + 6,  2));   /* hour  */
    date[1] = (uint8_t)(date[1] + cmd_htoi(val + 8,  2));   /* min   */
    date[0] = (uint8_t)(date[0] + cmd_htoi(val + 10, 2));   /* sec   */

    uint32_t lo = (uint32_t)date[0] | ((uint32_t)date[1] << 8) |
                  ((uint32_t)date[2] << 16) | ((uint32_t)date[3] << 24);
    uint32_t hi = (uint32_t)date[4] | ((uint32_t)date[5] << 8) |
                  ((uint32_t)date[6] << 16) | ((uint32_t)date[7] << 24);
    rtc_set(lo, hi);

    log_print(2, s_write_rtc, date[6], date[5], date[4], date[2], date[1], date[0]);
    CFG32(0) = lo;
    CFG32(4) = hi;
}

static void cmd_hw_version(const char *val, uint8_t vlen)
{
    if (vlen != 4) return;
    uint8_t hi = (uint8_t)cmd_htoi(val + 0, 2);
    uint8_t lo = (uint8_t)cmd_htoi(val + 2, 2);
    CFG16(0x54) = (uint16_t)(lo | (hi << 8));
    fedl5236_record_save();
    log_print(2, s_done);
}

static void cmd_shipping(const char *val, uint8_t vlen)
{
    (void)val;
    if (vlen != 0) return;
    uint32_t charger = *(volatile uint32_t *)0x2000042c;
    if (charger > 0x4e1f) {
        log_print(2, s_charger_v_mv, charger);
    } else {
        shipping_enter();
    }
}

/* Shared V/I offset calibration: offset = 2000 - base*1000/v, into cfg[off]. */
static void cmd_offset(const char *val, uint8_t vlen, uint16_t base, uint8_t cfg_off)
{
    if (vlen == 0) return;
    uint16_t v = (uint16_t)cmd_atoi(val, vlen);
    if (v != 0) {
        uint32_t off = (uint32_t)base * 1000u;
        off = off / v;
        off = 2000u - off;
        CFG16(cfg_off) = (uint16_t)(CFG16(cfg_off) + off);
    } else {
        CFG16(cfg_off) = 1000;
    }
    fedl5236_record_save();
    log_print(2, s_done);
}

static void cmd_v_offset(const char *val, uint8_t vlen)
{
    cmd_offset(val, vlen, *(volatile uint16_t *)0x20000202, 0x70);
}

static void cmd_i_offset(const char *val, uint8_t vlen)
{
    cmd_offset(val, vlen, *(volatile uint16_t *)0x200001ae, 0x72);
}

static void cmd_set_rsoc(const char *val, uint8_t vlen)
{
    if (vlen == 0) return;
    uint32_t v = cmd_atoi(val, vlen);
    if (v > 100) return;
    CFG32(0x20) = CFG32(0x1c);
    CFG32(0x20) = CFG32(0x20) * v;
    CFG32(0x20) = CFG32(0x20) / 100u;
    CFG32(0x18) = CFG32(0x20) * 14400u;
    log_print(2, s_done);
}

static void cmd_set_fcc(const char *val, uint8_t vlen)
{
    if (vlen == 0) return;
    uint32_t v = cmd_atoi(val, vlen);
    if (v > 100) return;
    CFG32(0x1c) = 0x25e4;                 /* 9700 mAh base */
    CFG32(0x1c) = CFG32(0x1c) * v;
    CFG32(0x1c) = CFG32(0x1c) / 100u;
    log_print(2, s_done);
}

/* Shared TS offset: clamp <=255 into cfg byte + a mirror cell, persist. */
static void cmd_ts_offset(const char *val, uint8_t vlen, uint8_t cfg_off,
                          volatile uint8_t *mirror)
{
    if (vlen == 0) return;
    uint32_t v = cmd_atoi(val, vlen);
    if (v > 255) return;
    CFG8(cfg_off) = 0;
    CFG8(cfg_off) = (uint8_t)(CFG8(cfg_off) + v);
    *mirror = CFG8(cfg_off);
    fedl5236_record_save();
    log_print(2, s_done);
}

static void cmd_ts0_offset(const char *val, uint8_t vlen)
{
    cmd_ts_offset(val, vlen, 8, (volatile uint8_t *)0x2000020e);
}

static void cmd_ts1_offset(const char *val, uint8_t vlen)
{
    cmd_ts_offset(val, vlen, 9, (volatile uint8_t *)0x2000021b);
}

static void cmd_ts2_offset(const char *val, uint8_t vlen)
{
    cmd_ts_offset(val, vlen, 0xa, (volatile uint8_t *)0x20000205);
}
