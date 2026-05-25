#ifndef BATTERYWARE_H
#define BATTERYWARE_H

#include <stdint.h>
#include <stdbool.h>

/* GPIO bit write: atomic set (BSRR) or clear (BRR) */
void gpio_bit_write(uint32_t gpio_base, uint16_t pin_bit, uint8_t value);

/* GPIO input read: returns true if pin is high */
bool gpio_bit_read(uint32_t gpio_base, uint16_t pin_bit);

/* Busy-wait millisecond delay */
void delay_ms(uint32_t ms);

/* LED flash routine — toggles a GPIO pin with fast/slow timing */
void led_flash(void);

/* System reset via SCB AIRCR register */
void nvic_system_reset(void);
void system_reset(void);
void system_reset_with_arg(uint32_t arg);

/* Charge MOSFET control via GPIOB pin 9 */
void charge_mosfet_set(bool on);

/* YMODEM protocol — send response byte and reset state */
void ymodem_send_byte(uint8_t b);

/* UART TX ring buffer — write single byte */
void uart_putchar(uint8_t c);

/* UART TXE interrupt handler — drains TX ring buffer */
void uart_tx_isr(void);

/* Block until all TX bytes are sent */
void uart_tx_flush(void);
void uart_check_parity_error(void);

/* System tick — read millisecond counter */
uint32_t get_tick_ms(uint32_t *out);

/* NVIC interrupt enable */
void nvic_enable_irq(uint8_t irqn);
void nvic_enable_irq_dsb(uint8_t irqn);
void nvic_enable_irq_s(int8_t irqn);
void nvic_enable_irq_s_dsb(int8_t irqn);

#endif /* BATTERYWARE_H */
