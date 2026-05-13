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
 * the shifter ever emits — TBD; ≥8 bytes per `modbus_reply_passthrough`). */
#define MODBUS_TX_BUF   ((uint8_t *)0x200001A9u)

/* Module-local RX scratch / inbound PDU buffer. The dispatcher in
 * FUN_08003c9a fills this from UART1 RX and then either reads fields
 * out of it (`report_image_status` cherry-picks specific offsets) or
 * forwards the first 6 bytes back as a passthrough reply
 * (`modbus_reply_passthrough`). */
#define MODBUS_RX_BUF   ((const volatile uint8_t *)0x200000C8u)

/* Inter-byte timeout counter, decremented once per system tick by
 * `modbus_tick`. The dispatcher uses it to detect end-of-frame per
 * Modbus RTU's 3.5-character idle rule. */
#define MODBUS_TICK_CTR (*(volatile uint32_t *)0x200000C4u)

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

/* OEM @ 0x080044DC (20 B). Decrement-if-nonzero on the inter-byte
 * timeout counter. Called from a periodic ISR (likely SysTick or TIM2)
 * once per tick. */
void modbus_tick(void)
{
    if (MODBUS_TICK_CTR != 0u) {
        MODBUS_TICK_CTR = MODBUS_TICK_CTR - 1u;
    }
}

/* OEM @ 0x080037CC (70 B). Forward the first 6 bytes of the inbound
 * Modbus PDU back as the reply payload: copy RX[0..5] → TX[0..5],
 * append CRC at TX[6..7], transmit 8 bytes. Used by the RX dispatcher
 * for echo-style responses where the same header bytes ride back. */
void modbus_reply_passthrough(void)
{
    uint8_t *const tx = MODBUS_TX_BUF;

    tx[0] = MODBUS_RX_BUF[0];
    tx[1] = MODBUS_RX_BUF[1];
    tx[2] = MODBUS_RX_BUF[2];
    tx[3] = MODBUS_RX_BUF[3];
    tx[4] = MODBUS_RX_BUF[4];
    tx[5] = MODBUS_RX_BUF[5];

    modbus_crc16_compute(tx, 6);

    tx[6] = MODBUS_CRC_LO;
    tx[7] = MODBUS_CRC_HI;

    modbus_tx_finalize(8u);
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
