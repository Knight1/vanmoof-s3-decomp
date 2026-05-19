#ifndef SHIFTERBOOT_UART_H
#define SHIFTERBOOT_UART_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* VanMoof-custom USART1 wrappers built on the MindMotion HAL.
 *
 * Both functions live in the early part of flash and are called from
 * the bootloader's `main` (and indirectly from the OTA helpers around
 * `FUN_08001534`). The HAL leaves `USART_SendData` / `USART_GetFlagStatus`
 * are byte-identical to MindMotion's stock `hal_uart.c` and are
 * declared as `extern` here; the real bodies arrive when the
 * MindMotion BSP is vendored in. */

void uart1_send_byte(uint8_t b);
void uart1_send_buf(const uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* SHIFTERBOOT_UART_H */
