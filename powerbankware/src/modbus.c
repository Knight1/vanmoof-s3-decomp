#include "powerbankware.h"

/*
 * Modbus RTU host link.
 *
 * The byte-fed frame processor is OEM FUN_080168c4 — a single ~6.5 KB
 * monolithic function: its telemetry cascade and command dispatch are inlined
 * continuations that share its stack frame (reached by internal branches, not
 * calls), so it cannot be split into clean sub-functions. It is the largest
 * routine in the image and is reserved for its own dedicated pass; only the
 * self-contained CRC it depends on is translated here.
 *
 * Frame layout (per channel, ch-1 indexed):
 *   state    @ 0x2000260c + (ch-1)*2   (0 = idle, 1 = got sync, then byte count)
 *   buffer   @ 0x20001b00 + (ch-1)*0x100
 * RX state machine: byte 0 must be sync 0xAA; byte 1 the function code
 * (0x03 read / 0x06 write-single / 0x10 write-multiple); bytes accumulate to
 * the 8-byte header, whose trailing 2 bytes are the big-endian CRC-16 checked
 * against modbus_crc16(buffer, 6). A valid header then drives the register
 * read/telemetry cascade or the write path.
 */

/*
 * Modbus CRC-16 (OEM FUN_08019094): init 0xFFFF, reflected poly 0xA001, no
 * final XOR — the standard Modbus RTU CRC. Identical to batteryware's
 * crc16_calc.
 */
uint16_t modbus_crc16(const uint8_t *data, int16_t len)
{
    uint16_t crc = 0xffff;

    while (len-- != 0) {
        crc ^= *data++;
        for (uint8_t bit = 0; bit < 8; bit++) {
            if ((crc & 1) == 0) {
                crc >>= 1;
            } else {
                crc = (uint16_t)((crc >> 1) ^ 0xa001u);
            }
        }
    }
    return crc;
}

/* ----------------------------------------------------------------------- *
 * Telemetry read response (Modbus function 0x03), OEM FUN_080168c4's read
 * cascade. The framing layer (own pass) validates a request frame, sets the
 * register base + count, then this emits each holding register from `base`
 * onward while the count remains. Every register is gated on the running
 * count at 0x20002610; the push helpers decrement it.
 */

#define RESP_COUNT (*(volatile uint16_t *)0x20002610)

/* push a 16-bit register value big-endian (OEM FUN_08019130). */
static void mb_push16(uint8_t hi, uint8_t lo)
{
    volatile uint8_t  * const buf = (volatile uint8_t  *)0x20001d00;
    volatile uint16_t * const wr  = (volatile uint16_t *)0x20002608;
    volatile uint16_t * const remain = (volatile uint16_t *)0x20002610;
    volatile uint16_t * const nreg   = (volatile uint16_t *)0x20001e00;

    buf[*wr] = hi; (*wr)++; (*remain)--;
    buf[*wr] = lo; (*wr)++; (*remain)--;
    (*nreg)++;
}

static void mb_emit16(uint16_t v)
{
    mb_push16((uint8_t)(v >> 8), (uint8_t)v);
}

/* push a thermistor reading as a scaled signed value (OEM FUN_080191c0):
 * (raw - 40) * 10 + 0xAAB. */
static void mb_push_temp(uint8_t raw)
{
    int16_t t = (raw < 0x28) ? (int16_t)((0x28 - raw) * -10)
                             : (int16_t)((raw - 0x28) * 10);
    t = (int16_t)(t + 0xaab);
    mb_push16((uint8_t)((uint16_t)t >> 8), (uint8_t)t);
}

/* |v|>199 -> sign*(|v|/10), else 0  (the current registers' scaling). */
static uint16_t mb_scale_current(int32_t v)
{
    if (v < 0) {
        uint32_t a = (uint32_t)(-v);
        return (a > 199) ? (uint16_t)(-(int32_t)(a / 10)) : 0;
    }
    return ((uint32_t)v > 199) ? (uint16_t)((uint32_t)v / 10) : 0;
}

/* Register 2 = a 16-bit BMS-state status word. For state in [7, 0x1c) the OEM
 * jump table (0x801f124) maps the state to a status bitmask; outside that range
 * it is 0 (or 8 when state 3 is "ready to ship"). */
static uint16_t modbus_state_reg2(uint8_t state)
{
    switch (state) {
    case 0x07: return 0x80;
    case 0x08: return 0x40;
    case 0x09: return 0x20;
    case 0x0a: return 0x10;
    case 0x0b: return 0x200;
    case 0x0c: return 0x100;
    case 0x0d: return 0x800;
    case 0x0e: return 0x400;
    case 0x11: return 0x01;
    case 0x12: return 0x2000;
    case 0x13: return 0x1000;
    case 0x14: return 0x8000;
    case 0x15: return 0x4000;
    case 0x16: return 0x02;
    case 0x17: return 0xffff;
    case 0x18: return 0xc0;
    case 0x19: return 0x30;
    case 0x1a: return 0x08;
    case 0x1b: return 0x04;
    default:   return 0;       /* states 0x0f / 0x10 */
    }
}

void modbus_telemetry(uint16_t base)
{
    volatile uint8_t  * const cfg  = (volatile uint8_t  *)0x200004d0;
    volatile uint8_t  * const err  = (volatile uint8_t  *)0x200005b0;
    volatile uint8_t  * const temp = (volatile uint8_t  *)0x20000218;
    volatile uint16_t * const cell = (volatile uint16_t *)0x20000380;
    const uint8_t state = *(volatile uint8_t *)0x200005ac;

#define REG(N) if (base < (N) + 1 && RESP_COUNT != 0)

    REG(0)  mb_push16(1, 0);
    REG(1)  mb_push16(0, 1);
    REG(2) {
        uint16_t status;
        if (state - 7u < 0x15u) {
            status = modbus_state_reg2(state);
        } else if (state == 3 &&
                   *(volatile uint16_t *)0x20000202 <= 0x4e1f &&
                   *(volatile uint16_t *)0x200001ae < 500) {
            status = 8;
        } else {
            status = 0;
        }
        mb_emit16(status);
    }
    REG(3)  mb_push_temp((temp[2] < temp[1]) ? temp[1] : temp[2]);
    REG(4)  mb_emit16(*(volatile uint16_t *)0x200003ce);              /* total voltage */
    REG(5)  mb_push16(0, cfg[0x5a]);                                  /* SOC */
    REG(6)  mb_emit16(mb_scale_current(*(volatile int32_t *)0x20000424)); /* current */
    REG(7) {
        uint16_t v = (state == 2) ? 2 : (state == 3) ? 1 : 0;
        if ((int32_t)((uint32_t)*(volatile uint16_t *)0x200006a0 << 0x13) < 0) {
            v = 3;
        }
        mb_push16(0, (uint8_t)v);
    }
    REG(8)  mb_push16(0, (uint8_t)(*(volatile uint8_t *)0x20000412 & 1));
    REG(9)  mb_push16(0, 0);
    REG(0xa) mb_emit16(*(volatile uint16_t *)(cfg + 0x54));           /* HW version */
    REG(0xb) {                                                        /* image version (hi word) */
        uint32_t ver = *(const volatile uint32_t *)0x08008004;
        mb_push16((uint8_t)(ver >> 24), (uint8_t)(ver >> 16));
    }
    /* cfg serial/FW byte-pairs 0x5e..0x6f -> registers 0xc..0x14 */
    for (uint8_t r = 0xc; r <= 0x14; r++) {
        REG(r) mb_push16(cfg[0x5e + (r - 0xc) * 2 + 1], cfg[0x5e + (r - 0xc) * 2]);
    }
    REG(0x15) mb_emit16(0x25e4);                                      /* FCC base */
    REG(0x16) mb_emit16((uint16_t)*(volatile uint32_t *)(cfg + 0x1c));/* FCC */
    REG(0x17) mb_emit16((uint16_t)*(volatile uint32_t *)(cfg + 0x20));/* remaining cap */
    REG(0x18) mb_push16(0, cfg[0x5b]);                                /* RSOC */
    REG(0x19) mb_emit16(*(volatile uint16_t *)(cfg + 0x50));
    REG(0x1a) mb_push16(0, (uint8_t)((*(volatile uint8_t *)0x20000412 & 2) == 2));
    /* 10 cell voltages 0x20000380[0..9] -> registers 0x1b..0x24 */
    for (uint8_t r = 0x1b; r <= 0x24; r++) {
        REG(r) mb_emit16(cell[r - 0x1b]);
    }
    REG(0x25) mb_push_temp(temp[1]);                                  /* TS0 */
    REG(0x26) mb_push_temp(temp[2]);                                  /* TS1 */
    REG(0x27) mb_push_temp(*(volatile uint8_t *)0x20000218);          /* TS2 (raw) */
    REG(0x28) mb_emit16(*(volatile uint16_t *)0x200006a6);
    REG(0x29) mb_emit16(*(volatile uint16_t *)0x200003a2);            /* cell max */
    REG(0x2a) mb_emit16(*(volatile uint16_t *)0x200003d2);            /* cell min */
    REG(0x2b) mb_emit16(*(volatile uint16_t *)0x20000202);           /* V base */
    REG(0x2c) {                                                       /* I base (abs) */
        int16_t v = *(volatile int16_t *)0x200001ae;
        if (v < 0) v = (int16_t)-v;
        mb_emit16((uint16_t)v);
    }
    REG(0x30) mb_emit16(*(volatile uint16_t *)(err + 0xe));           /* errlog cycle count */
    REG(0x31) mb_push_temp(err[0x2a]);
    REG(0x32) mb_push_temp(err[0x2b]);
    REG(0x33) mb_push_temp(err[0xb]);
    REG(0x34) mb_emit16(*(volatile uint16_t *)(err + 0x26));
    REG(0x35) mb_emit16((uint16_t)(int16_t)(*(volatile int32_t *)err / 10)); /* errlog current (/10) */
    REG(0x36) mb_emit16((uint16_t)*(volatile uint32_t *)(err + 4));
    REG(0x37) mb_emit16((uint16_t)*(volatile uint32_t *)(err + 8));
    /* errlog snapshot pairs */
    REG(0x38) mb_push16(0, err[0x28]);
    REG(0x39) mb_push16(0, err[0x29]);
    for (uint8_t r = 0x3a; r <= 0x44; r++) {
        REG(r) mb_emit16(*(volatile uint16_t *)(err + 0x10 + (r - 0x3a) * 2));
    }
    /* cfg protection thresholds 0x2c..0x4f -> registers 0x45..0x56 */
    for (uint8_t r = 0x45; r <= 0x56; r++) {
        REG(r) mb_emit16(*(volatile uint16_t *)(cfg + 0x2c + (r - 0x45) * 2));
    }
    REG(0x57) mb_emit16(*(volatile uint16_t *)(cfg + 0x74));          /* V offset */
    REG(0x58) mb_emit16(*(volatile uint16_t *)(cfg + 0x76));          /* I offset */
    REG(0x59) mb_push_temp(cfg[0x78]);                                /* TS0 offset */
    REG(0x5a) mb_push_temp(cfg[0x79]);
    REG(0x5b) mb_push_temp(cfg[0x7a]);

#undef REG
}

/* ----------------------------------------------------------------------- *
 * Frame processor (OEM FUN_080168c4 framing) — fed one RX byte at a time per
 * channel. Runs the sync/function/accumulate state machine, validates the
 * CRC-16, and for a read request (fn 0x03) builds + transmits the telemetry
 * response. Write requests (fn 0x06 single / 0x10 multiple) hand off to their
 * own handlers (own pass).
 *
 * Per channel: state @ 0x2000260c+(ch-1)*2, request frame @ 0x20001b00+(ch-1)*0x100.
 * Response is built at 0x20001d00 (header 0xAA, fn, byte-count) and sent over
 * the TX ring.
 */
void modbus_write_single(uint8_t channel); /* FUN_08018410 (fn 0x06) — below */
extern void modbus_write_multi(uint8_t channel); /* FUN_080187e4 (fn 0x10) */

void modbus_process(uint8_t channel, uint8_t b)
{
    const int c = channel - 1;
    volatile uint16_t * const state = (volatile uint16_t *)(0x2000260c + c * 2);
    volatile uint8_t  * const frame = (volatile uint8_t  *)(0x20001b00 + c * 0x100);

    /* sync byte */
    if (*state == 0) {
        if (b == 0xAA) {
            frame[0] = 0xAA;
            *state = 1;
        } else {
            *state = 0;
        }
        return;
    }
    /* function code */
    if (*state == 1) {
        if (b == 3 || b == 6 || b == 0x10) {
            frame[1] = b;
            *state = 2;
            for (int i = 2; i <= 6; i++) {
                frame[i] = 0;
            }
        } else {
            *state = 0;
        }
        return;
    }

    uint8_t fn = frame[1];
    if (fn != 6) {
        if (fn == 0x10) {
            modbus_write_multi(channel);
        }
        if (fn != 3) {
            *state = 0;
            return;
        }
    }

    /* accumulate the 8-byte header */
    frame[*state] = b;
    (*state)++;
    if (*state < 8) {
        return;
    }

    *(volatile uint16_t *)0x20001e00 = 0;                 /* emitted register count */
    uint16_t crc_rx = (uint16_t)((frame[7] << 8) | frame[6]);
    if (modbus_crc16((const uint8_t *)frame, 6) != crc_rx) {
        *state = 0;
        return;
    }

    if (fn != 3) {
        modbus_write_single(channel);                     /* fn 0x06 */
        return;
    }

    /* function 0x03 read response */
    volatile uint16_t * const wr   = (volatile uint16_t *)0x20002608;
    volatile uint8_t  * const resp = (volatile uint8_t  *)0x20001d00;

    *wr = 3;
    uint16_t base = (uint16_t)((frame[2] << 8) | frame[3]);
    uint16_t cnt  = (uint16_t)((frame[4] << 8) | frame[5]);
    resp[0] = 0xAA;
    resp[1] = 3;
    cnt = (uint16_t)(cnt * 2);                            /* register count -> byte count */
    RESP_COUNT = cnt;
    resp[2] = (uint8_t)cnt;

    modbus_telemetry(base);

    uint16_t crc = modbus_crc16((const uint8_t *)resp, (int16_t)*wr);
    mb_push16((uint8_t)crc, (uint8_t)(crc >> 8));         /* append lo, hi */
    for (uint16_t i = 0; i < *wr; i++) {
        uart_putchar(resp[i]);
    }
    *state = 0;
}

/* ----------------------------------------------------------------------- *
 * Write-single handler (Modbus function 0x06), OEM FUN_08018410. Decodes the
 * register address (frame[2..3]) + value (frame[4..5]) and applies it:
 *   0x0080  enter-upgrade: when mode bit2 is set, drop the FET, reset the AFE,
 *           latch a reboot-persistent upgrade flag in the RTC backup regs,
 *           echo the request and system-reset; otherwise just ack.
 *   0x0001  (value 0) -> shipping mode.
 *   0x0009/0x000a/0x0095  read-only / no-op (ack only).
 *   0xF020/0xF021/0xF022   write TS0/1/2 offset (value outside [0x14,0xeb]),
 *           mirror it, and persist.
 * A successful write echoes the 8-byte request frame back as the ack.
 */
extern void FUN_08013be4(void);                       /* GPIO pulse */
extern void rtc_backup_write(void *hrtc, int idx, uint8_t val); /* FUN_0801c6a2 */
extern void FUN_08013b94(void);                       /* system reset */
extern void FUN_080161b4(void);                       /* TX flush (ch 1) */

static void mb_echo_frame(volatile uint8_t *frame)
{
    for (int i = 0; i < 8; i++) {
        uart_putchar(frame[i]);
    }
}

void modbus_write_single(uint8_t channel)
{
    const int c = channel - 1;
    volatile uint8_t  * const frame = (volatile uint8_t  *)(0x20001b00 + c * 0x100);
    volatile uint16_t * const state = (volatile uint16_t *)(0x2000260c + c * 2);
    volatile uint16_t * const ack   = (volatile uint16_t *)0x20001e00;
    volatile uint8_t  * const cfg   = (volatile uint8_t  *)0x200004d0;

    if (frame[1] == 6) {
        uint16_t addr  = (uint16_t)((frame[2] << 8) | frame[3]);
        uint16_t value = (uint16_t)((frame[4] << 8) | frame[5]);

        if (addr == 0x80) {
            if ((*(volatile uint16_t *)0x200006a0 & 4) != 0) {   /* mode bit 2 */
                void * const hrtc = (void *)0x200006f0;
                FUN_08013be4();
                gpio_bit_write(0x48000400, 0x200, 0);            /* PB9 = 0 */
                *(volatile uint8_t *)0x20000412 = 0;
                fedl5236_command_write(9, 0);
                *(volatile uint8_t *)0x20000724 = 1;
                *(volatile uint8_t *)0x20000698 = (uint8_t)~*(volatile uint8_t *)0x20000724;
                rtc_backup_write(hrtc, 0, *(volatile uint8_t *)0x20000724);
                rtc_backup_write(hrtc, 1, *(volatile uint8_t *)0x20000698);
                mb_echo_frame(frame);
                if (channel == 1) {
                    FUN_080161b4();
                } else {
                    uart_flush();
                }
                FUN_08013b94();                                  /* reset */
            } else {
                (*ack)++;
            }
        } else if (addr == 9 || addr == 10 || addr == 0x95) {
            (*ack)++;
        } else if (addr == 1 && value == 0) {
            mb_echo_frame(frame);
            shipping_enter();
        } else if (addr == 0xf020) {
            if (value < 0x14 || value > 0xeb) {
                cfg[8] = (uint8_t)value;
                *(volatile uint8_t *)0x2000020e = cfg[8];
                fedl5236_record_save();
            }
            (*ack)++;
        } else if (addr == 0xf021) {
            if (value < 0x14 || value > 0xeb) {
                cfg[9] = (uint8_t)value;
                *(volatile uint8_t *)0x2000021b = cfg[9];
                fedl5236_record_save();
            }
            (*ack)++;
        } else if (addr == 0xf022) {
            if (value < 0x14 || value > 0xeb) {
                cfg[0xa] = (uint8_t)value;
                *(volatile uint8_t *)0x20000205 = cfg[0xa];
                fedl5236_record_save();
            }
            (*ack)++;
        }

        if (*ack != 0) {
            mb_echo_frame(frame);
        }
    } else {
        mb_echo_frame(frame);
    }

    *state = 0;
}
