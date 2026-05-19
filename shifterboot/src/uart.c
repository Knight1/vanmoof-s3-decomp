/* uart.c — VanMoof-custom USART1 send-byte / send-buffer wrappers
 *          on top of the MindMotion HAL.
 *
 * Two leaves, both 28 bytes, decomp'd from shifterboot.bin:
 *
 *   - `uart1_send_byte`  @ 0x080000C8 — write one byte to USART1, then
 *     spin on the TX-ready/complete flag (SR bit 0 on MM32) before
 *     returning. The OEM realises the spin via `USART_GetFlagStatus`
 *     with flag id 1.
 *
 *   - `uart1_send_buf`   @ 0x080000E4 — drive a 16-bit count of bytes
 *     out through `uart1_send_byte`. The OEM compiles this as a
 *     decrement-then-test loop with the count in a uxth-clamped
 *     register, so the parameter is `uint16_t`.
 *
 * Calls into the MindMotion HAL (`USART_SendData` @ 0x08001364,
 * `USART_GetFlagStatus` @ 0x08001372 — both vendor-stock per
 * `docs/progress.md`). The HAL functions are declared `extern` here;
 * their bodies will arrive when the MindMotion BSP is vendored in.
 */

#include "uart.h"

#include <stdint.h>

/* MM32F031 peripheral pointer for USART1. The OEM loads this from a
 * literal pool word at 0x080004C8. Declared as an opaque pointer
 * because we don't yet ship a CMSIS-style USART_TypeDef; the MindMotion
 * HAL primitives take a `USART_TypeDef *` (which on this part is just
 * the base address wrapped in a struct), and `void *` covers either. */
static void *const USART1_BASE = (void *)0x40013800u;

/* MindMotion HAL leaves (vendor-stock — not yet present in the build).
 * Signatures match MindMotion's `hal_uart.h`. */
extern void     USART_SendData(void *USARTx, uint16_t Data);
extern uint32_t USART_GetFlagStatus(void *USARTx, uint16_t USART_FLAG);

/* MM32 USART status flag selector consumed by `USART_GetFlagStatus`.
 * The MindMotion HAL's flag enum encodes the SR bit position; for the
 * MM32F031 silicon the "TX-ready / TX-complete" bit is SR bit 0, which
 * the HAL exposes as flag id 1. Same wire-level meaning as the
 * shifterware-side `USART_SR_TX_READY_Msk` (`1u << 0`). */
#define USART_FLAG_TX_READY  (1u)

/* OEM @ 0x080000C8 (28 B). The compiler emits a redundant
 *     `mov r4, r0 ; mov r1, r4`
 * pair before the first HAL call, and a `nop` between the two HAL
 * calls — both are -O0 artefacts in the MindMotion BSP build. We
 * don't attempt to reproduce them; control flow and side effects
 * match. */
void uart1_send_byte(uint8_t b)
{
    USART_SendData(USART1_BASE, (uint16_t)b);
    while (USART_GetFlagStatus(USART1_BASE, USART_FLAG_TX_READY) == 0u) {
        /* spin until TX-ready */
    }
}

/* OEM @ 0x080000E4 (28 B). Decrement-then-test loop with a
 * pre-test jump (`b 0x080000F4`) skipping the body on the first
 * iteration so `len == 0` is a no-op. The count register holds a
 * `uxth`-clamped value, fixing `len`'s width at 16 bits. */
void uart1_send_buf(const uint8_t *buf, uint16_t len)
{
    while (len != 0u) {
        uart1_send_byte(*buf);
        buf++;
        len = (uint16_t)(len - 1u);
    }
}
