/* motorware ↔ mainware serial link (S.0.00.22).
 *
 * Reconstructed from the IDA C28x disassembly (build/ida/motorware.lst); each
 * function cites its flash address. The link is **SLIP-framed packets with a
 * CRC-16/Modbus trailer, over SCI-A** — point-to-point, no Modbus slave
 * address. (Verified: see docs/protocol.md.)
 *
 * Frame on the wire (SLIP, RFC 1055):
 *     C0 | <payload, byte-stuffed> | <crc16_lo, crc16_hi byte-stuffed> | C0
 *   stuffing: C0 -> DB DC,  DB -> DB DD
 *   crc16 = Modbus CRC (poly 0xA001, init 0xFFFF) over the *unstuffed* payload.
 *
 * C28x notes: char/int are 16-bit; these run on the F28054F. The RAM globals
 * below live in L3 DPSARAM (0x9000+) at the cited word addresses; without TI's
 * cl2000 this file documents the reconstruction (it is not yet linked).
 */
#include <stdint.h>

/* --- comm state in L3 RAM (addresses from the disassembly) --------------- */
typedef struct sci_regs sci_regs_t;             /* SciaRegs @0x7050 (opaque) */

typedef struct {                                /* the HAL/comm object */
    /* ... offsets 0x00..0xBF ... */
    sci_regs_t *sci;                            /* +0xC0: SCI-A handle (SciaRegs) */
} comm_obj_t;

extern comm_obj_t *g_comm;                      /* pointer @0x903E */

static uint16_t s_tx_head;                       /* @0x94BB  ring write index */
static uint16_t s_tx_count;                      /* @0x94BD  bytes queued (0..64) */
static uint16_t s_tx_buf[64];                    /* @0x94C0  software TX ring */

void sci_tx_enable(sci_regs_t *sci);             /* sub_3F3F6D — kick SCI-A TX */

/* --- Modbus CRC-16 (poly 0xA001) ---------------------------------------- */

/* 0x3F4B20 — fold one byte into the running CRC (8 shifts). */
uint16_t modbus_crc16_byte(uint16_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 8; i; i--)
        crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    return crc;
}

/* 0x3F4B2C — CRC over a buffer; `crc` seeds the run (callers pass 0xFFFF). */
uint16_t modbus_crc16(const uint8_t *buf, uint16_t len, uint16_t crc)
{
    while (len--)
        crc = modbus_crc16_byte(crc, *buf++);
    return crc;
}

/* --- SCI-A transmit ----------------------------------------------------- */

/* 0x3F4294 — queue one byte into the 64-deep software TX ring and kick the
   transmitter. Returns 1 if queued, 0 if the ring is full. */
int sci_tx_byte(uint8_t byte)
{
    comm_obj_t *c = g_comm;

    if (s_tx_count >= 64)
        return 0;
    s_tx_buf[s_tx_head] = byte;
    s_tx_count++;
    if (++s_tx_head == 64)
        s_tx_head = 0;
    sci_tx_enable(c->sci);                       /* SciaRegs, comm_obj +0xC0 */
    return 1;
}

/* helper: emit one payload byte with SLIP byte-stuffing. */
static void slip_put(uint8_t b)
{
    if (b == 0xC0) {                             /* END  -> DB DC */
        sci_tx_byte(0xDB);
        sci_tx_byte(0xDC);
    } else if (b == 0xDB) {                      /* ESC  -> DB DD */
        sci_tx_byte(0xDB);
        sci_tx_byte(0xDD);
    } else {
        sci_tx_byte(b);
    }
}

/* 0x3F3310 — SLIP-encode `buf[len]`, append the CRC-16 (lo then hi), and frame
   it with END (0xC0) delimiters. */
void slip_tx_frame(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = modbus_crc16(buf, len, 0xFFFF);

    sci_tx_byte(0xC0);                           /* opening END */
    for (uint16_t i = 0; i < len; i++)
        slip_put(buf[i]);
    slip_put(crc & 0xFF);                        /* CRC low  byte */
    slip_put((crc >> 8) & 0xFF);                 /* CRC high byte */
    sci_tx_byte(0xC0);                           /* closing END */
}

/* RX path: 0x3F33E6 is the SLIP receive state machine (state @0x95B8:
 * 0=idle, 1=in-frame, 2=after-ESC), de-escaping incoming bytes into a frame
 * ring buffer and signalling a complete frame on END. Reconstruction of its
 * full body + the command dispatch is tracked in docs/progress.md. */
