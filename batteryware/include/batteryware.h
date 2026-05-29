#ifndef BATTERYWARE_H
#define BATTERYWARE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "cmsis_intrinsics.h"

/* Hex conversion helpers */
char     nibble_to_hex(uint8_t nibble);
uint8_t  hex_to_nibble(char c);
uint32_t atoi_hex_offset1(char *str, uint8_t digits);

/* Modbus CRC-16 */
uint16_t crc16_calc(uint8_t *data, int16_t len);

/* CRC-8 for flash verification */
uint8_t  crc8_calc(uint8_t *data, int8_t len);

/* CRC-8/SMBus PEC (poly 0x07, init 0x00) — `crc8_verify` is the
 * read-side helper; identical computation. OEM at FUN_080141E4 is
 * 1834 B table-driven; this is bitwise. */
uint32_t crc8_for_smbus(uint8_t *data, uint32_t len);
uint8_t  crc8_verify(uint8_t *data, uint32_t len);

/* GPIO bit write: atomic set (BSRR) or clear (BRR) */
void gpio_bit_write(uint32_t gpio_base, uint16_t pin_bit, uint8_t value);

/* GPIO input read: returns true if pin is high */
bool gpio_bit_read(uint32_t gpio_base, uint16_t pin_bit);

/* GPIO pin reset — reset multiple pins to input mode */
void gpio_pin_reset(uint32_t *gpio_base, uint32_t pin_mask);
void gpio_init_buttons(void);

/* Word-to-bytes unpack */
void word_to_bytes(uint32_t *src, uint16_t byte_count, int dst);

/* Bytes-to-word pack (reverse of word_to_bytes) */
uint32_t bytes_to_words(uint32_t **dst_pp, const uint8_t *src, uint32_t byte_count);

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
void system_reset_fault(void);
void system_init(void);

/* NVIC interrupt control — OEM uses raw cpsid/cpsie inline; no
 * separate helper symbols exist. Vector-table install is also inlined
 * (memset_byte_copy + SCB->VTOR write) in batteryware_main. */

/* Charge MOSFET control via GPIOB pin 9 */
void charge_mosfet_set(bool on);
void charge_mosfet_off(void);
void charge_mosfet_on(void);
void discharge_mosfet_set(bool on);

/* YMODEM protocol */
void ymodem_send_byte(uint8_t b);
void ymodem_receive(uint8_t data);

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
void modem_command_handler(uint8_t byte);

/* Block until all TX bytes are sent */
void uart_tx_flush(void);
void uart_check_parity_error(void);
void uart_check_overrun_error(void);
void uart_puthex_byte(uint8_t b);
void uart_puthex_16(uint16_t val);
void uart_puthex_32(uint32_t val);
void uart_puts(char *str);
void uart_putdec_64(uint32_t lo, uint32_t hi, uint8_t digits);
void uart_resp_handler(void);
void uart_protocol_handler(uint8_t byte);
void uart_printf(uint8_t *fmt);

/* Flash controller operations */
uint32_t flash_enable_prefetch(void);
uint32_t flash_unlock_opt(void);
uint32_t flash_lock_opt(void);
uint32_t flash_wait_ready(void *ctx);
uint32_t flash_timeout_check(uint32_t param);
bool peripheral_reset(void);
uint32_t flash_verify_header(void);

/* Flash page erase + interrupt priority setup */
bool flash_page_erase(uint32_t timeout_ticks);
bool flash_erase_page_wrapper(uint32_t timeout_ticks);
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
void flash_program_handler(uint8_t *frame, uint16_t frame_len);
void flash_program_init(void *ctx);
uint32_t flash_prescaler_setup(int *ctx);
uint8_t  usart1_dma_setup(int *ctx);
uint32_t flash_op_start(int *ctx);

/* Fuel gauge status */
bool fg_status_flag_get(void);
bool fg_status_flag2_get(void);
void fg_clear_status(void);
void fg_threshold_check(void);
uint16_t calculate_rsoc(void);

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
void fg_scan(void);
void fg_coulomb_update(void);
void cell_balance_update(void);
void coulomb_counter(uint32_t val);
void cell_voltage_scan(void);
void config_init(void);
void bms_init(void);

/* memcmp/verify helpers */
void memcmp_verify(char *actual, uint32_t len, char *expected);
void memcpy_oem(const uint8_t *src, uint16_t count, uint8_t *dst);

/* Shipping mode */
void shipping_mode_check(void);
void rsoc_lookup(void);
void rsoc_set(uint8_t percent);

/* Command dispatch table entry.
 *
 * OEM layout (per 47-byte slot at 0x08012b9c..0x08012fd5):
 *     byte 0     : idx (1..23)
 *     byte 1     : name_len
 *     byte 2..   : name (zero-padded to 47 bytes total)
 *
 * Our decomp uses a smaller pointer-based form since we can reference
 * the names directly from `strings.c`. */
typedef struct {
    uint8_t      idx;
    uint8_t      name_len;
    const char  *name;
} cmd_entry_t;

/* Modbus command response helpers */
void cmd_counter_inc(uint16_t frame_word);
void cmd_counter_inc_v2(uint16_t frame_word);
void cmd_counter_inc_v3(uint16_t frame_word);
void cmd_send_response(void);
void cmd_send_8byte(void);
void command_parser(uint32_t buf_addr, int buf_len, uint8_t cmd_byte);

/* BMS configuration */
void bms_configure(uint8_t cfg);
void bms_set_state(uint8_t state);

/* Main entry point */
void batteryware_main(void);
void peripheral_init(bool arg);
void main_clock_setup(void);
int  main(void);

/* State machine handlers */
void state_handler_01(void);
void state_handler_03_init(void);
void state_handler_17_19(void);
void state_flags_handler(uint8_t arg);
void state_flags_handler_timer(void);
void state_timer_0b(void);
void state_timer_0c(void);
void state_timer_12(void);
void state_timer_13(void);
void state_timer_0d(void);
void state_timer_0e(void);
void state_timer_14(void);
void state_timer_03(void);
void state_timer_05(void);
void state_timer_06(void);
void state_timer_07(void);
void state_timer_08(void);
void state_timer_09(void);
void state_timer_10(void);
void state_timer_charge_a(void);
void state_timer_charge_b(void);
void bms_state_machine(void);

/* DMA operations */
uint32_t dma_transfer_irq(volatile uint32_t *ctx, void *src, uint32_t count);
uint8_t  atomic_copy_16words(volatile uint32_t *dst, volatile uint32_t *src);

/* DMA initialization */
void dma_init(void);
void dma_channel_reset(uint32_t dma_base);
uint32_t dma_flash_start(void *ctx);
uint32_t dma_wait_for_ready(uint32_t timeout);
void dma_error_clear(void);
void dma_error_clear_v2(void);
uint32_t dma_get_status(uint32_t reg_val);

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
uint8_t  dma_irq_copy(uint32_t *dst1, uint32_t *src2, uint32_t *dst3, uint32_t *src4);

/* Coulomb counter deferred thunk — OEM "veneer" that resolves into
 * an SRAM-installed routine; semantics here are a no-op. */
uint32_t veneer_1556c(uint32_t arg);
uint32_t veneer_1557c(uint32_t numerator, uint32_t divisor);

/* 0x08011exx/0x08011fxx veneers — each is a trampoline that bx-jumps to a
 * function pointer held in flash whose value is an SRAM address (Thumb bit
 * set) installed at runtime; the routine bodies are not in this image, so
 * these are no-op stubs (see src/nops.c). Signatures match the call sites. */
void veneer_11ee8(void);
void veneer_11f08(int arg);
void veneer_11f18(void);
void veneer_11f58(uint32_t arg);
void veneer_11f68(void);
void veneer_11f88(void);

/* memcpy helpers */
uint32_t memcpy_halfword(volatile uint32_t *dst_ptr, uint32_t src_base, uint32_t count);
void memcpy_byte(void *dst, const void *src, uint32_t len);
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
bool button_entry_check(void);
void state_timer_15(void);
void state_timer_0a(void);
void can_transmit(void);   /* misnomer: actually the state-0x16 periodic timer */
void service_uart_init(void);   /* FUN_0800AB7C: USART1 service UART, PA10-gated */

/* System tick — read millisecond counter */
uint32_t get_tick_ms(uint32_t *out);
uint32_t tick_get(void);
uint32_t tick_counter_read(void);
int32_t  clock_prescaler_val(void);
uint32_t rcc_reconfigure(uint32_t *param);
int8_t   clock_config(uint32_t *param, uint32_t clk_src);
uint32_t tick_val_get(void);
uint32_t tick_ms_get(void);
uint32_t tick_timeout_get(void);
void     timer_scheduler(void);

/* NVIC interrupt enable */
void nvic_enable_irq(uint8_t irqn);
void nvic_enable_irq_dsb(uint8_t irqn);
void nvic_enable_irq_s(int8_t irqn);
void nvic_enable_irq_s_dsb(int8_t irqn);
uint32_t nvic_reconfigure(int *ctx, uint32_t *param);

/* Oscillator config — HAL_RCC_OscConfig (FUN_0800FDAC). Defined in
 * rcc.c. cfg points to a 14×u32 rcc_osc_init_t (OscillatorType plus
 * per-oscillator State / Calibration / Range fields, then the 4-word
 * PLL sub-struct). */
uint32_t rcc_osc_config(void *cfg);

/* Phase 2 init — USART/DMA/GPIO initialization */
void phase2_init(void);

/* Main clock setup */
void main_clock_setup(void);

/* IRQ wait handler — alias for state_timer_10 */
void irq_wait_handler(void);

/* Fuel gauge init — alias for bms_init */
void fg_init(void);

/* Flash unlock — alias for flash_unlock_both */
void flash_unlock(void);

/* CRC peripheral — HAL_CRC_Init / HAL_CRCEx_Polynomial_Set / MspInit.
 * Defined in crc.c. The crc_handle_t struct is module-local; callers
 * pass an opaque `void *` matching the HAL layout. */
uint32_t crc_init(void *hcrc);
uint8_t  crcex_polynomial_set(void *hcrc, uint32_t poly, uint32_t length);
void     crc_msp_init(void *hcrc);

/* Clock-tree config — HAL_RCC_ClockConfig (FUN_08010554).
 * Defined in rcc.c. cfg is a 5×u32 rcc_clk_init_t struct
 * (ClockType, SYSCLKSource, AHB, APB1, APB2 dividers).
 * flatency is the target FLASH_ACR.LATENCY (0 or 1). */
uint32_t rcc_configure(void *cfg, uint32_t flatency);
uint32_t rcc_get_sysclock_freq(void);
uint8_t  hal_init_tick(uint32_t priority);

/* GPIO pin configuration struct passed to gpio_pin_config.
 *
 * Layout matches the OEM 5×u32 buffer at offsets {0, 4, 8, 12, 16}.
 * Callers may reuse a single instance across multiple calls (OEM
 * does this — see gpio_init_buttons); fields that aren't explicitly
 * re-assigned inherit their value from the previous call. */
typedef struct {
    uint32_t pin_mask;   /* which pins of the GPIO port to configure  */
    uint32_t mode;       /* mode word — bitfield (see GPIO_MODE_* etc.) */
    uint32_t pupd;       /* PUPDR value: GPIO_PUPD_NONE / _UP / _DOWN  */
    uint32_t speed;      /* OSPEEDR value: GPIO_SPEED_LOW .. _VHIGH    */
    uint32_t af;         /* alternate-function number 0..15            */
} gpio_pin_cfg_t;

/* gpio_pin_cfg_t.mode bit layout (note: bits combine — e.g. an EXTI
 * falling-edge input is `INPUT | EXTI_ENABLE | EXTI_IMR | EXTI_FTSR`).
 *   bits[1:0]  MODER  (input/output/AF/analog)
 *   bit  4     OTYPER (push-pull/open-drain — only honoured for output and AF modes)
 *   bits[16,17,20,21] EXTI line enables (only honoured when EXTI_ENABLE is set)
 *   bit  28    EXTI block enable — gates the entire SYSCFG/EXTI path
 */
#define GPIO_MODE_INPUT     0x00000000U
#define GPIO_MODE_OUTPUT    0x00000001U
#define GPIO_MODE_AF        0x00000002U
#define GPIO_MODE_ANALOG    0x00000003U
#define GPIO_OTYPE_OD       0x00000010U  /* bit 4 — open-drain (default push-pull) */
#define GPIO_EXTI_IMR       0x00010000U  /* bit 16 — unmask interrupt           */
#define GPIO_EXTI_EMR       0x00020000U  /* bit 17 — unmask event               */
#define GPIO_EXTI_RTSR      0x00100000U  /* bit 20 — rising-edge trigger        */
#define GPIO_EXTI_FTSR      0x00200000U  /* bit 21 — falling-edge trigger       */
#define GPIO_EXTI_ENABLE    0x10000000U  /* bit 28 — enable the EXTI block      */

/* PUPDR values */
#define GPIO_PUPD_NONE      0U
#define GPIO_PUPD_UP        1U
#define GPIO_PUPD_DOWN      2U

/* OSPEEDR values */
#define GPIO_SPEED_LOW      0U
#define GPIO_SPEED_MED      1U
#define GPIO_SPEED_HIGH     2U
#define GPIO_SPEED_VHIGH    3U

void gpio_pin_config(uint32_t *gpio_base, gpio_pin_cfg_t *cfg);
void nop_e774(void);
void nop_e784(void);
void nop_a6e0(void);
void nop_2ba6(void);
void nop_2bac(void);
void nop_4764(void);
void nop_537c(void);
void epilogue(void);
void veneer_a6aa(void);
void veneer_a6ba(void);
void veneer_a6be(void);
/* nop_eebc removed — was FUN_0800eebc, now crc_msp_init in crc.c. */
void cmd_send_response_stub(void);
void cmd_send_response_stub2(void);
void protocol_reset(void);
void epilogue_e1ca(void);
void thunk_e1bc(void);
void thunk_e1c4(void);
void nop_e1c8(void);
void thunk_e1b4(void);
void null_trap(void);
void nop_716c(void);
void nop_721c(void);
uint64_t __aeabi_ldiv0(uint64_t dividend, uint64_t divisor);

/* OEM rodata strings (src/strings.c). See that file for layout +
 * the per-string OEM address. */
extern const char s_chg_cal_ok[];
extern const char s_dsg_cal_ok[];
extern const char s_fedl5236_init[];
extern const char s_chargeon_off_pb9_0[];
extern const char s_pdown_set_command[];
extern const char s_pdown_error[];
extern const char s_pdown_finish[];
extern const char s_i_am_vanmoof_ap_lead[];
extern const char s_mos_failure_mode[];
extern const char s_dp_mode[];
extern const char s_vanmoof_mode[];
extern const char s_power_on_uvp2_mode[];
extern const char s_power_on_uvp1_mode[];
extern const char s_power_on_ovp2_mode[];
extern const char s_power_on_ovp1_mode[];
extern const char s_pwron_detect_uvp2[];
extern const char s_pwron_detect_uvp1[];
extern const char s_pwron_detect_ovp2[];
extern const char s_pwron_detect_ovp1[];
extern const char s_predischg_lock_ctr[];
extern const char s_ui_timeout[];
extern const char s_shipping_mode[];
extern const char s_pdown_start[];
extern const char s_max_cell_voltage[];
extern const char s_min_cell_voltage[];
extern const char s_total_voltage[];
extern const char s_real_rsoc[];
extern const char s_record_soc[];
extern const char s_rsoc_adjust[];
extern const char s_i_am_vanmoof_ap[];
extern const char s_chg_cal_eq_pct_i[];
extern const char s_dsg_cal_eq_pct_i[];
extern const char s_fmt_bl_banner[];
extern const char s_fmt_ap_banner[];
extern const char s_chg_cal_eq_i_compact[];
extern const char s_dsg_cal_eq_i_compact[];
extern const char s_wait_log_clear[];
extern const char s_ts0_offset[];
extern const char s_ts1_offset[];
extern const char s_ts2_offset[];
extern const char s_log_clear_ok[];
extern const char s_temp_log_a[];
extern const char s_fmt_hex8_a[];
extern const char s_new_bootloader_crc[];
extern const char s_flash_write_count[];
extern const char s_temp_log_b[];
extern const char s_fmt_hex8_b[];
extern const char s_arrow_ok[];

extern const char cmd_who[];
extern const char cmd_now[];
extern const char cmd_pf[];
extern const char cmd_reset_bms[];
extern const char cmd_df[];
extern const char cmd_upgrade_ap[];
extern const char cmd_upgrade_bl[];
extern const char cmd_into_bootloader[];
extern const char cmd_chg_cal_set[];
extern const char cmd_chg_cal_get[];
extern const char cmd_dsg_cal_set[];
extern const char cmd_dsg_cal_get[];
extern const char cmd_reset_esn[];
extern const char cmd_log_clear[];
extern const char cmd_ts0_set[];
extern const char cmd_ts1_set[];
extern const char cmd_ts2_set[];
extern const char cmd_ts0_get[];
extern const char cmd_ts1_get[];
extern const char cmd_ts2_get[];
extern const char cmd_ts_reset[];
extern const char cmd_fcc[];
extern const char cmd_soc[];

#endif /* BATTERYWARE_H */
