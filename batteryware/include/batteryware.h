#ifndef BATTERYWARE_H
#define BATTERYWARE_H

#include <stdint.h>
#include <stdbool.h>

/* Hex conversion helpers */
char     nibble_to_hex(uint8_t nibble);
uint8_t  hex_to_nibble(char c);
uint32_t atoi_hex_offset1(char *str, uint8_t digits);

/* GPIO bit write: atomic set (BSRR) or clear (BRR) */
void gpio_bit_write(uint32_t gpio_base, uint16_t pin_bit, uint8_t value);

/* GPIO input read: returns true if pin is high */
bool gpio_bit_read(uint32_t gpio_base, uint16_t pin_bit);

/* Busy-wait millisecond delay */
void delay_ms(uint32_t ms);

/* Busy-wait microsecond delay */
void delay_us(uint32_t us);

/* LED flash routine — toggles a GPIO pin with fast/slow timing */
void led_flash(void);

/* System reset via SCB AIRCR register */
void nvic_system_reset(void);
void system_reset(void);
void system_reset_with_arg(uint32_t arg);

/* Charge MOSFET control via GPIOB pin 9 */
void charge_mosfet_set(bool on);
void discharge_mosfet_set(bool on);

/* YMODEM protocol — send response byte and reset state */
void ymodem_send_byte(uint8_t b);

/* UART TX ring buffer — write single byte */
void uart_putchar(uint8_t c);

/* UART TXE interrupt handler — drains TX ring buffer */
void uart_tx_isr(void);

/* USART2/modem configuration */
void modem_config(void);

/* Block until all TX bytes are sent */
void uart_tx_flush(void);
void uart_check_parity_error(void);
void uart_check_overrun_error(void);
void uart_puthex_byte(uint8_t b);
void uart_puts(char *str);

/* Flash controller operations */
uint32_t flash_enable_prefetch(void);
uint32_t flash_unlock_opt(void);
uint32_t flash_lock_opt(void);

/* Flash page erase + interrupt priority setup */
bool flash_page_erase(uint32_t timeout_ticks);
void interrupt_set_priority(uint8_t irqn, uint32_t priority);
void flash_opt_byte_op(uint8_t op, uint32_t val);

/* Fuel gauge status */
bool fg_status_flag_get(void);

/* Fault flags — central protection status register at 0x20002C44 */
extern volatile uint32_t * const g_fault_flags;
#define FAULT_UVP1  0x01
#define FAULT_UVP2  0x02
#define FAULT_OVP1  0x04
#define FAULT_OVP2  0x08
#define FAULT_DISCHARGE_OC  0x40
#define FAULT_CHARGE_OC     0x80
#define FAULT_TS            0x10

/* Under/over-voltage protection checks */
void fg_uvp1_check(void);
void fg_uvp2_check(void);
void fg_ovp1_check(void);
void fg_ovp2_check(void);
void fg_discharge_oc_check(void);
void fg_charge_oc_check(void);

/* Shipping mode */
void shipping_mode_check(void);
void rsoc_lookup(void);

/* System tick — read millisecond counter */
uint32_t get_tick_ms(uint32_t *out);

/* NVIC interrupt enable */
void nvic_enable_irq(uint8_t irqn);
void nvic_enable_irq_dsb(uint8_t irqn);
void nvic_enable_irq_s(int8_t irqn);
void nvic_enable_irq_s_dsb(int8_t irqn);

#endif /* BATTERYWARE_H */
