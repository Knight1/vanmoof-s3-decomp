#ifndef SHIFTER_UART_H
#define SHIFTER_UART_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define UART_RX_BUFFER_SIZE   64u

void   uart1_init(uint32_t baud);
void   uart1_send_byte(uint8_t b);
size_t uart1_send(const uint8_t *data, size_t len);
bool   uart1_rx_available(void);
uint8_t uart1_rx_byte(void);
size_t uart1_rx_drain(uint8_t *dst, size_t max_len);

void   USART1_IRQHandler(void);

#endif /* SHIFTER_UART_H */
