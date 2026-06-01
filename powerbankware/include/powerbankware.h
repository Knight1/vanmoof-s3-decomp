#ifndef POWERBANKWARE_H
#define POWERBANKWARE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * powerbankware — VanMoof PowerBank application (STM32F091xC, Cortex-M0).
 *
 * Seed header for an in-progress decomp. Prototypes are added per function
 * as src/ files land (mirroring batteryware's batteryware.h). The firmware
 * is a BMS sibling of batteryware (shared FEDL5236 core) plus a power-path
 * output stage (bypass FET, DAC-regulated Vout, charger/load detection).
 *
 * Startup chain (verified): Reset_Handler -> SystemInit -> __libc_init_array
 * -> main. main() runs the mode-gated state-machine super-loop.
 */

void Reset_Handler(void);
void SystemInit(void);
void __libc_init_array_lite(void);
int  main(void);

/* ---- GPIO (src/gpio.c) ------------------------------------------------- *
 * STM32F091 GPIO ports live on the AHB at 0x48000000 (GPIOA), 0x400-stride.
 * The register layout matches every Cortex-M0 STM32 part:
 *   +0x00 MODER  +0x04 OTYPER  +0x08 OSPEEDR  +0x0C PUPDR
 *   +0x10 IDR    +0x18 BSRR    +0x20/24 AFRL/AFRH  +0x28 BRR
 */

/* GPIO input read via IDR (+0x10): true if the pin is high. */
bool gpio_bit_read(uint32_t gpio_base, uint16_t pin_bit);

/* GPIO atomic bit write: value!=0 -> BSRR (set, +0x18); value==0 -> BRR
 * (reset, +0x28). Both write-only/atomic. */
void gpio_bit_write(uint32_t gpio_base, uint16_t pin_bit, uint8_t value);

/* gpio_pin_cfg_t — 5x u32 buffer passed to gpio_pin_config (= HAL_GPIO_Init).
 * Field order matches the OEM stack buffer {0,4,8,12,16}. Callers reuse one
 * instance across calls; unset fields inherit the previous value. */
typedef struct {
    uint32_t pin_mask;   /* cfg[0] — which port pins to configure        */
    uint32_t mode;       /* cfg[1] — mode bitfield (see GPIO_MODE_* etc.) */
    uint32_t pupd;       /* cfg[2] — PUPDR value                          */
    uint32_t speed;      /* cfg[3] — OSPEEDR value                        */
    uint32_t af;         /* cfg[4] — alternate-function number 0..15      */
} gpio_pin_cfg_t;

/* mode bitfield (bits combine — an EXTI falling-edge input is
 * INPUT | EXTI_ENABLE | EXTI_IMR | EXTI_FTSR):
 *   bits[1:0]  MODER   bit 4 OTYPER(OD)
 *   bit 16 IMR  bit 17 EMR  bit 20 RTSR  bit 21 FTSR  bit 28 EXTI-enable */
#define GPIO_MODE_INPUT     0x00000000U
#define GPIO_MODE_OUTPUT    0x00000001U
#define GPIO_MODE_AF        0x00000002U
#define GPIO_MODE_ANALOG    0x00000003U
#define GPIO_OTYPE_OD       0x00000010U
#define GPIO_EXTI_IMR       0x00010000U
#define GPIO_EXTI_EMR       0x00020000U
#define GPIO_EXTI_RTSR      0x00100000U
#define GPIO_EXTI_FTSR      0x00200000U
#define GPIO_EXTI_ENABLE    0x10000000U

#define GPIO_PUPD_NONE      0U
#define GPIO_PUPD_UP        1U
#define GPIO_PUPD_DOWN      2U

#define GPIO_SPEED_LOW      0U
#define GPIO_SPEED_MED      1U
#define GPIO_SPEED_HIGH     2U
#define GPIO_SPEED_VHIGH    3U

void gpio_pin_config(uint32_t *gpio_base, gpio_pin_cfg_t *cfg);

/* ---- Timing (src/delay.c) ---------------------------------------------- */
/* Busy-wait `ms` software ticks (1 ms each) off the SysTick flag byte at
 * 0x2000077c; kicks the IWDG on the periodic-flag (bit 2) iterations. */
void delay_ms(int ms);
/* As delay_ms, but services the UART TX/RX each spin (for slow EEPROM waits). */
void delay_ms_service(int ms);

/* ---- Memory helpers (src/mem.c) ---------------------------------------- */
void mem_zero(void *dst, int len);
bool mem_compare(const void *a, const void *b, uint16_t len);

/* ---- Hardware CRC (src/crc.c) ------------------------------------------ */
uint32_t crc_accumulate(void *handle, const void *buf, uint32_t len);
uint32_t bms_record_crc(const void *buf, uint16_t len);

/* ---- BMS-record persistence (src/fedl5236.c) --------------------------- */
/* Stamp the record CRC and write+verify the 0x80-byte BMS record to the I2C
 * EEPROM, retrying until the read-back matches. */
void fedl5236_record_save(void);

/* ---- FEDL5236 AFE SPI driver (src/fedl5236.c) -------------------------- *
 * The FEDL5236 battery-AFE hangs off hardware SPI with CS on GPIOA PA15
 * (0x8000). Unlike batteryware (bit-banged SMBus), powerbankware talks to
 * the same chip over SPI, so the framing is image-specific.
 *   command_write: TX [ (reg<<1)|0x80, val, crc8 ]            (3 bytes)
 *   read_data:     TX [ (reg<<1)|0x81, count, 0xff ]; reads count+3 back,
 *                  verifies the trailing CRC-8. Returns 1 on success, 0 on
 *                  busy/retry failure.
 */
void fedl5236_command_write(uint8_t reg, uint8_t val);
int  fedl5236_read_data(uint8_t reg, uint8_t count);
/* Full AFE bring-up: default register block, zero-current offset, cell/total
 * voltage scan, charger voltage, and the TS watch (returns on a clean temp
 * reading; powers down + halts on a persistent TS fault). */
void fedl5236_initialize(void);
/* AFE power-down handshake: wait for the charger to fall below ~10 V
 * (Check_Charger_Voltage), wait the reg-0x0C bit-7 handshake (Check_PUPIN),
 * then issue POWER_DOWN. Retries up to 6×; halts on failure. When
 * `enable_record` is set, persists the BMS record on success. */
void fedl5236_powerdown(int enable_record);

/* ---- SPI HAL (src/spi.c) ----------------------------------------------- */
/* HAL handle State byte: 1 == READY. */
uint8_t spi_get_state(void *handle);
/* HAL_SPI_TransmitReceive_IT: arms a full-duplex IT transfer. 0=OK,1=ERR,2=BUSY. */
int     spi_transmit_receive(void *handle, volatile uint8_t *tx,
                             volatile uint8_t *rx, uint32_t count);
/* SPI fatal-error path — requests a system reset and hangs. */
void    spi_error_reset(void);
/* Free-running millisecond tick counter (0x20002614). */
uint32_t tick_get(void);

/* ---- I2C EEPROM driver (src/i2c.c) ------------------------------------- */
/* STM32F0 HAL_I2C_Mem_Write/Read (polling). dev = 8-bit address, addr =
 * memory address, addrsz = 1 or 2 bytes. Returns 0=OK, 1=NACK/AF, 2=BUSY,
 * 3=timeout/error. */
int eeprom_mem_write(void *h, uint8_t dev, uint16_t addr, uint16_t addrsz,
                     void *buf, uint16_t len, uint32_t timeout);
int eeprom_mem_read(void *h, uint8_t dev, uint16_t addr, uint16_t addrsz,
                    void *buf, uint16_t len, uint32_t timeout);

/* ---- UART TX ring + log formatter (src/uart.c) ------------------------- */
void uart_putchar(uint8_t c);
void uart_tx_isr(void);
void uart_flush(void);
void uart_puts(const char *str);
char nibble_to_hex(uint8_t nibble);
/* printf-like log over the TX ring. Specifiers: %d(u8) %i(u16) %l(u32) decimal,
 * %x(u8) %w(u16) hex, %s string. `channel` is vestigial (OEM keeps it). */
void log_print(uint8_t channel, const char *fmt, ...);

/* ---- Extend_IO LED-bar driver (src/extend_io.c) ------------------------ */
/* Map BMS state + SOC to an LED-bar level and push it to the I/O expander on
 * the FEDL5236 SPI bus (CS = PA8). Re-sends only on change. */
void extend_io_update(void);

/* ---- Thermistor (src/temp.c) ------------------------------------------- */
/* Convert the latest FEDL5236 TS ADC sample (RX buffer) to a temperature
 * index via the rational curve + descending LUT. */
uint8_t ntc_temp_read(void);

#endif /* POWERBANKWARE_H */
