#ifndef BATTERYWARE_H
#define BATTERYWARE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Hex conversion helpers */
char     nibble_to_hex(uint8_t nibble);
uint8_t  hex_to_nibble(char c);
uint32_t atoi_hex_offset1(char *str, uint8_t digits);

/* Modbus CRC-16 */
uint16_t crc16_calc(uint8_t *data, int16_t len);

/* CRC-8 for flash verification */
uint8_t  crc8_calc(uint8_t *data, int8_t len);

/* GPIO bit write: atomic set (BSRR) or clear (BRR) */
void gpio_bit_write(uint32_t gpio_base, uint16_t pin_bit, uint8_t value);

/* GPIO input read: returns true if pin is high */
bool gpio_bit_read(uint32_t gpio_base, uint16_t pin_bit);

/* GPIO pin reset — reset multiple pins to input mode */
void gpio_pin_reset(uint32_t *gpio_base, uint32_t pin_mask);

/* Word-to-bytes unpack */
void word_to_bytes(uint32_t *src, uint16_t byte_count, int dst);

/* Busy-wait millisecond delay */
void delay_ms(uint32_t ms);

/* Busy-wait microsecond delay */
void delay_us(uint32_t us);

/* LED flash routine — toggles a GPIO pin with fast/slow timing */
void led_flash(void);
void fault_led_trigger(void);

/* System reset via SCB AIRCR register */
void nvic_system_reset(void);
void system_reset(void);
void system_reset_with_arg(uint32_t arg);
void system_init(void);

/* Charge MOSFET control via GPIOB pin 9 */
void charge_mosfet_set(bool on);
void charge_mosfet_off(void);
void charge_mosfet_on(void);
void discharge_mosfet_set(bool on);

/* YMODEM protocol — send response byte and reset state */
void ymodem_send_byte(uint8_t b);

/* UART TX ring buffer — write single byte */
void uart_putchar(uint8_t c);

/* UART TXE interrupt handler — drains TX ring buffer */
void uart_tx_isr(void);

/* USART2/modem configuration */
void modem_config(void);
void modem_reinit(void);
bool modem_deinit(void *ctx);
void modem_init(void);
void modem_send_2bytes(uint8_t b1, uint8_t b2);
void temp_offset_send(uint8_t raw_temp);
uint8_t bus_ready_check(int ctx);
uint8_t smbus_transmit(int *ctx, int tx_buf, int rx_buf, int16_t count);

/* Block until all TX bytes are sent */
void uart_tx_flush(void);
void uart_check_parity_error(void);
void uart_check_overrun_error(void);
void uart_puthex_byte(uint8_t b);
void uart_puthex_16(uint16_t val);
void uart_puts(char *str);

/* Flash controller operations */
uint32_t flash_enable_prefetch(void);
uint32_t flash_unlock_opt(void);
uint32_t flash_lock_opt(void);
uint32_t flash_wait_ready(void *ctx);
uint32_t flash_timeout_check(uint32_t param);
bool peripheral_reset(void);

/* Flash page erase + interrupt priority setup */
bool flash_page_erase(uint32_t timeout_ticks);
void interrupt_set_priority(uint8_t irqn, uint32_t priority);
void flash_opt_byte_op(uint8_t op, uint32_t val);
uint32_t flash_program_start(void *ctx);
uint32_t flash_erase_start(void *ctx);
uint8_t  flash_word_write(uint32_t type, volatile uint32_t *dst, uint32_t val);
uint32_t flash_unlock_both(void);
void flash_op_cleanup(void *ctx);
uint32_t flash_page_program(int *ctx);
void flash_dma_start(uint32_t dst_addr);
uint32_t flash_write_verify(volatile uint32_t *dst, uint16_t count, int src_base);
void flash_program_init(void *ctx);
uint32_t flash_prescaler_setup(int *ctx);

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
void fg_alert_monitor(void);
uint8_t fg_charge_status(void);
void config_resend_all(void);
void fg_watchdog_kick(void);
void fg_cell_balance(uint8_t cell_idx);
void capacity_decrement(uint32_t amount);
uint32_t fg_read_field_8(void);
uint32_t fg_read_field_11(void);
void fg_read_loop(void *ctx);

/* Shipping mode */
void shipping_mode_check(void);
void rsoc_lookup(void);
void rsoc_set(uint8_t percent);

/* Modbus command response helpers */
void cmd_counter_inc(uint16_t frame_word);
void cmd_counter_inc_v2(uint16_t frame_word);
void cmd_counter_inc_v3(uint16_t frame_word);
void cmd_send_response(void);
void cmd_send_8byte(void);

/* BMS configuration */
void bms_configure(uint8_t cfg);
void bms_set_state(uint8_t state);

/* Main entry point */
void batteryware_main(void);
void peripheral_init(bool arg);

/* State machine handlers */
void state_handler_01(void);
void state_handler_03_init(void);
void state_handler_17_19(void);
void state_flags_handler(uint8_t arg);
void state_flags_handler_timer(void);

/* DMA operations */
uint32_t dma_transfer_irq(volatile uint32_t *ctx, void *src, uint32_t count);
uint8_t  atomic_copy_16words(volatile uint32_t *dst, volatile uint32_t *src);

/* DMA initialization */
void dma_init(void);
void dma_channel_reset(uint32_t dma_base);
uint32_t dma_flash_start(void *ctx);

/* DMA transfer handlers */
void dma_byte_handler(int *ctx);
void dma_byte_handler_v2(int *ctx);
void dma_halfword_handler(int *ctx);
void dma_halfword_handler_v2(int *ctx);
uint32_t dma_timeout_copy(int *ctx, uint32_t param2, uint32_t param3);
uint32_t timeout_poll(int *ctx, uint32_t mask, uint8_t expected, int deadline, uint32_t max_time);
void dma_transfer_done(int *ctx);
void dma_channel_config(int *ctx);
uint32_t dma_completion_handler(uint32_t *ctx);
uint32_t dma_wait_done(int timeout);
uint32_t dma_usart_init(int *ctx);

/* memcpy helpers */
uint32_t memcpy_halfword(volatile uint32_t *dst_ptr, uint32_t src_base, uint32_t count);
void memset_byte_copy(int dst, int src, int count);
void memset_byte_fill(uint8_t *dst, uint8_t val, int count);

/* DMA byte transfer done */
void dma_byte_done(int *ctx);

/* Timeout poll variant */
uint32_t timeout_poll_v2(int *ctx, uint32_t mask, uint8_t param_3, int param_4, uint32_t param_5);

/* SPI register write (FEDL5236 communication) */
uint8_t spi_register_write(uint8_t type, volatile void *reg, uint32_t val);
void smbus_write_reg(uint8_t reg, uint8_t val, uint8_t mask);
uint32_t smbus_read(uint8_t addr, uint8_t count);
void smbus_read_nack(uint8_t addr, uint8_t val);

/* System tick — read millisecond counter */
uint32_t get_tick_ms(uint32_t *out);
uint32_t tick_get(void);
uint32_t tick_counter_read(void);
int32_t  clock_prescaler_val(void);
uint32_t rcc_reconfigure(uint32_t *param);

/* NVIC interrupt enable */
void nvic_enable_irq(uint8_t irqn);
void nvic_enable_irq_dsb(uint8_t irqn);
void nvic_enable_irq_s(int8_t irqn);
void nvic_enable_irq_s_dsb(int8_t irqn);

#endif /* BATTERYWARE_H */
