/* modbus.c — Modbus RTU primitives shared by every PDU the shifter
 * emits. CRC, byte-buffer transmit, and the OTA-aware finalize that
 * reboots after a successful image apply. */

#include "modbus.h"
#include "uart.h"
#include "compiler.h"
#include <stdint.h>

#define MODBUS_INIT     0xFFFFu
#define MODBUS_POLY     0xA001u

/* The OEM keeps the CRC's two bytes as separate addressable RAM
 * bytes (probably uint8_t crc_lo, crc_hi globals). Accessed by raw
 * address here to match the OEM bytes. */
#define MODBUS_CRC_LO   (*(volatile uint8_t *)0x200000E7u)
#define MODBUS_CRC_HI   (*(volatile uint8_t *)0x200000E8u)

/* Module-local TX buffer (max length determined by the longest PDU
 * the shifter ever emits — TBD; ≥7 bytes per `report_image_status`). */
#define MODBUS_TX_BUF   ((uint8_t *)0x200001A9u)

/* Latch set by `image_apply` when the staged image validates clean.
 * `modbus_tx_finalize` consumes it after a 7-byte transmit to trigger
 * the install-time reboot. */
#define G_IMG_OK_FLAG   (*(volatile uint8_t *)0x200000DAu)

/* Cortex-M0 SCB->AIRCR. VECTKEY (0x05FA) << 16 | SYSRESETREQ (bit 2). */
#define SCB_AIRCR       (*(volatile uint32_t *)0xE000ED0Cu)
#define AIRCR_SYSRESET  0x05FA0004u

/* OEM @ 0x0800378C (64 B). */
void modbus_crc16_compute(const uint8_t *buf, int len)
{
    uint16_t crc = (uint16_t)MODBUS_INIT;
    while (len != 0) {
        crc ^= (uint16_t)(*buf++);
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 1u) != 0u) {
                crc = (uint16_t)((crc >> 1) ^ MODBUS_POLY);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
        len--;
    }
    MODBUS_CRC_LO = (uint8_t)(crc & 0xFFu);
    MODBUS_CRC_HI = (uint8_t)(crc >> 8);
}

/* OEM @ 0x0800373A (28 B). */
void modbus_send_bytes(const uint8_t *buf, unsigned len)
{
    while (len != 0u) {
        uart1_send_byte(*buf++);
        len--;
    }
}

/* OEM @ 0x08003756 (54 B). */
void modbus_tx_finalize(unsigned len)
{
    modbus_send_bytes(MODBUS_TX_BUF, len);

    if (len == 7u && G_IMG_OK_FLAG == 1u) {
        G_IMG_OK_FLAG = 0u;
        __nop();
        __dsb();
        SCB_AIRCR = AIRCR_SYSRESET;
        __dsb();
        __nop();
        __nop();
        for (;;) { /* wait for the reset to actually fire */ }
    }
}
