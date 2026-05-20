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

/* Bring up USART1 + the matching GPIO pins for the Modbus RTU bus.
 *
 *   PB6 = USART1_TX (AF0, push-pull)
 *   PB7 = USART1_RX (AF0, floating input)
 *   USART1: 8-N-1, baud = `baud_rate`, RX+TX enabled, RXNE IRQ enabled
 *   NVIC: USART1 (IRQ 27) at priority 3, ENABLE
 *
 * Sole on-target caller is `main` at `0x08000214` with
 * `baud_rate = 9600` (materialised as `75 << 7`). After this returns,
 * `USART1_IRQHandler` starts firing on every received byte. */
void boot_init_usart1(uint32_t baud_rate);

#ifdef __cplusplus
}
#endif

#endif /* SHIFTERBOOT_UART_H */
