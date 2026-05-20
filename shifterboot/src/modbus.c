/* modbus.c — Modbus RTU primitives for shifterboot's OTA server.
 *
 * Shifterboot speaks the same RTU framing as shifterware (3.5-char
 * idle gap, 16-bit CRC suffix, big-endian within byte but the CRC
 * itself transmitted low-byte-first), but exposes only the OTA-flow
 * subset of function codes (cmd 0x81 = apply, cmd 0x82 = stream a
 * 32 B image chunk, etc.). The dispatcher lives in `main`; this file
 * holds the framing primitives those handlers share. */

#include "modbus.h"

#include <stdint.h>

/* RTU CRC parameters (per the Modbus over Serial Line specification). */
#define MODBUS_INIT  0xFFFFu
#define MODBUS_POLY  0xA001u

/* OEM @ 0x08000100 (56 B). Per-byte XOR + 8-bit shift-and-XOR with
 * polynomial 0xA001. Direct return — the result lands in r0, no RAM
 * side effect (the shifterware twin, by contrast, writes its two
 * bytes to globals at 0x200000E7 / 0x200000E8).
 *
 * Implementation notes vs the OEM bytes:
 *   - The OEM emits `asrs` (arithmetic shift right) for the post-XOR
 *     shift instead of `lsrs`. Since the live value never sets bit 31
 *     (CRC is 16-bit, initialised to 0x0000FFFF, only the lower bits
 *     are ever XOR'd), `asrs` is identical to `lsrs` here — a -O0
 *     codegen quirk, not an algorithm choice.
 *   - The outer-loop control is the snapshot-then-decrement idiom:
 *     `tmp = count; count--; if (tmp != 0) iterate`. That is exactly
 *     what gcc emits for `while (count != 0) { ... ; count--; }`
 *     when the test is placed at the loop header. */
uint16_t modbus_crc16(const uint8_t *data, uint16_t count)
{
    uint16_t crc = (uint16_t)MODBUS_INIT;

    while (count != 0u) {
        crc ^= (uint16_t)(*data++);
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 1u) != 0u) {
                crc = (uint16_t)((crc >> 1) ^ MODBUS_POLY);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
        count--;
    }

    return crc;
}

/* MindMotion HAL leaves used by the RX path (vendor-stock — bodies
 * arrive once the BSP is vendored). Signatures match MindMotion's
 * `hal_uart.h`. */
extern uint32_t USART_GetITStatus(void *USARTx, uint16_t USART_IT);
extern void     USART_ClearITPendingBit(void *USARTx, uint16_t USART_IT);
extern uint16_t USART_ReceiveData(void *USARTx);

/* Same pointer the TX-side uses in `uart.c`. Kept module-local so each
 * compilation unit references the literal pool independently — matches
 * the OEM, which materialises `0x40013800` from a fresh pool word in
 * every TU that touches USART1. */
static void *const USART1_BASE = (void *)0x40013800u;

/* MM32F031 USART HAL interrupt-selector constant for RXNE (data-register
 * not empty). The MindMotion HAL exposes interrupt sources as small
 * integer IDs that the same HAL helpers translate into the relevant SR
 * / IER bit positions; on this part the RXNE selector is `2`. */
#define USART_IT_RXNE  (2u)

/* Inbound Modbus frame buffer + write index.
 *
 *   `MODBUS_RX_BUF`   = SRAM 0x200000C4, 45 bytes wide (the OEM caps the
 *                       index at `0x2D = 45` before writing).
 *   `MODBUS_RX_IDX`   = SRAM 0x20000014, halfword — current write
 *                       position; bytes past index 44 are silently
 *                       dropped on the floor.
 *
 * The 45-byte ceiling is the maximum Modbus RTU frame shifterboot's
 * dispatcher consumes — wide enough for the longest OTA-streaming PDU
 * (cmd-0x82 carries 32 B of image payload plus an 11 B header plus the
 * 2 B trailing CRC). Anything wider gets dropped at this stage rather
 * than walking off the end of the buffer. */
static uint8_t  *const MODBUS_RX_BUF = (uint8_t *)0x200000C4u;
static volatile uint16_t *const MODBUS_RX_IDX = (volatile uint16_t *)0x20000014u;

#define MODBUS_RX_BUF_SIZE  45u

/* OEM @ 0x0800160E (58 B). Vector slot 43 (USART1).
 *
 * One byte per RXNE: clear the flag, read the byte, append to the RX
 * buffer if there's room. The frame-completion detection (Modbus RTU's
 * 3.5-character idle gap) lives elsewhere — this handler does no
 * framing, it just accumulates raw bytes. The dispatcher in `main`
 * polls `MODBUS_RX_IDX` to see when a frame has arrived.
 *
 * Note that the OEM does NOT re-check the IT status after reading the
 * data register (which would clear RXNE as a side effect on most STM32
 * variants). On MM32F031 the read-clears-RXNE side effect appears to
 * be sufficient, but the OEM explicitly calls `USART_ClearITPendingBit`
 * first — defensively, before the data read. We preserve that order. */
void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1_BASE, USART_IT_RXNE) == 0u) {
        return;
    }

    USART_ClearITPendingBit(USART1_BASE, USART_IT_RXNE);
    uint8_t b = (uint8_t)USART_ReceiveData(USART1_BASE);

    uint16_t idx = *MODBUS_RX_IDX;
    if (idx >= MODBUS_RX_BUF_SIZE) {
        return;
    }

    MODBUS_RX_BUF[idx] = b;
    *MODBUS_RX_IDX = (uint16_t)(idx + 1u);
}
