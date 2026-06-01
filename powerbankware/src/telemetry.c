#include "powerbankware.h"

/*
 * status_frame_emit — OEM FUN_0800F02C.
 *
 * Assemble the periodic status frame from the BMS record (0x200004D0) and the
 * serial-number block (0x200006E8) into the TX scratch buffer at 0x2000048C,
 * packing four sub-blocks (length-prefixed, big-endian fields) through the
 * frame packer, then log the human-readable summary. Field layout mirrors the
 * OEM byte offsets exactly.
 */

#define FR   ((volatile uint8_t  *)0x2000048C)   /* frame buffer  */
#define REC  ((volatile uint8_t  *)0x200004D0)   /* BMS record    */
#define SN   ((volatile uint8_t  *)0x200006E8)   /* serial number */
#define FLEN (*(volatile uint8_t *)0x20000480)   /* working length */

extern const char s_send_sn[];   /* "\nSend SN = %x %x ... State = %x\r" 0x0801E000 */

/*
 * crc16_append — OEM FUN_0800EF70. Modbus CRC-16 (init 0xFFFF, reflected poly
 * 0xA001, no final XOR) over buf[0..len-1], with the two checksum bytes stored
 * little-endian at buf[len]/buf[len+1]. Same algorithm as modbus_crc16
 * (FUN_08019094) but computed-and-appended in place; a distinct OEM routine,
 * kept separate to preserve the emitted call structure.
 */
void crc16_append(volatile uint8_t *buf, int len)
{
    uint16_t crc = 0xffff;
    volatile uint8_t *p = buf;

    for (int n = len; n != 0; n--) {
        crc ^= *p++;
        for (uint8_t bit = 0; bit < 8; bit++) {
            if ((crc & 1) == 0) {
                crc >>= 1;
            } else {
                crc = (uint16_t)((crc >> 1) ^ 0xa001u);
            }
        }
    }
    p[0] = (uint8_t)crc;
    p[1] = (uint8_t)(crc >> 8);
}

void status_frame_emit(void)
{
    FLEN = 8;
    FR[0] = 0;
    FR[1] = REC[0x67];
    FR[2] = REC[0x66];
    FR[3] = REC[0x69];
    FR[4] = REC[0x68];
    FR[5] = REC[0x6b];
    crc16_append(FR, FLEN - 2);

    FLEN = 6;
    FR[8]  = 1;
    FR[9]  = SN[3];
    FR[10] = SN[2];
    FR[0xb] = SN[1];
    crc16_append(FR + 8, FLEN - 2);              /* sub-block at 0x20000494 */

    FLEN = 4;
    FR[0xe] = 2;
    FR[0xf] = REC[0x5a];                         /* SOC */
    crc16_append(FR + 0xe, FLEN - 2);            /* sub-block at 0x2000049A */

    FLEN = 7;
    FR[0x12] = 3;
    FR[0x13] = REC[0x5b];                        /* SOH */
    {
        uint16_t cyc = *(volatile uint16_t *)(0x200004D0 + 0x50);
        FR[0x14] = (uint8_t)cyc;
        FR[0x15] = (uint8_t)(cyc >> 8);
    }
    FR[0x16] = *(volatile uint8_t *)0x200004B4;  /* state */
    crc16_append(FR + 0x12, FLEN - 2);           /* sub-block at 0x2000049E */

    FLEN = 8;
    log_print(2, s_send_sn,
              FR[1], FR[2], FR[3], FR[4], FR[5],
              *(volatile uint16_t *)(0x200006E8 + 2),  /* version */
              REC[0x5a],                               /* SOC */
              REC[0x5b],                               /* SOH */
              *(volatile uint16_t *)(0x200004D0 + 0x50),/* cycle count */
              *(volatile uint8_t *)0x200004B4);        /* state */

    *(volatile uint8_t *)0x2000048A = 0;
}
