/* bmsboot.h — VanMoof battery-module bootloader ("VanMoof BL")
 *
 * Prototypes, memory map, peripheral bases and protocol constants for the
 * STM32L072CZT6 loader, reconstructed from bmsboot_v007.bin.
 *
 * The loader owns the first 20 KB of flash. On reset the CPU enters the vector
 * table at flash base; boot_hw_init() relocates that table to SRAM and points
 * VTOR at it (the STM32L0 has VTOR, unlike the STM32F0 sibling powerbankboot).
 * The application (batteryware) lives at AP_BASE; a backup / OTA-staging copy
 * lives at SHADOW_BASE.
 */
#ifndef BMSBOOT_H
#define BMSBOOT_H

#include <stdint.h>

/* ===================================================================== */
/* Flash / EEPROM memory map (STM32L072CZT6: 192 KB flash, 6 KB EEPROM)  */
/* ===================================================================== */
#define BOOT_BASE        0x08000000u    /* loader (this image), 20 KB          */
#define APP_BASE         0x08005000u    /* application bank,    86 KB          */
#define SHADOW_BASE      0x0801A800u    /* backup / OTA staging, 86 KB         */
#define BANK_SIZE        0x00015800u    /* 86 KB per app bank                  */
#define SHADOW_OFFSET    0x00015800u    /* shadow = AP + this (copy source)    */
#define FLASH_END        0x08030000u    /* end of 192 KB main flash            */
#define FLASH_PAGE_SIZE  0x80u          /* 128-byte erase/program page (L0)    */

/* STM32L0 data EEPROM holds the persisted boot flag + reset cause. It is
 * byte-writable through the flash controller (flash_program()). */
#define EEPROM_BASE      0x08080000u
#define BOOT_FLAG_ADDR   0x08080000u    /* 1-byte persisted boot flag          */
#define RESET_CAUSE_ADDR 0x08080002u    /* 4-byte saved RCC_CSR reset flags    */

/* Persisted boot-flag values (BOOT_FLAG_ADDR). */
#define BOOT_FLAG_NORMAL 0x55u          /* normal: validate + boot AP          */
#define BOOT_FLAG_STAY   0xCCu          /* recover: install Shadow -> AP        */
#define BOOT_FLAG_ACK    0x33u          /* post-write: rewrite to 0x55          */
#define BOOT_FLAG_WIPE   0x5Au          /* wipe AP + Shadow, then download      */

/* VanMoof application image header (prefixes AP / Shadow banks). */
#define IMG_MAGIC        0xAA55AA55u    /* header[0]                           */
#define IMG_HDR_SIZE     0x28u          /* header bytes before the vectors     */
#define IMG_MAX_SIZE     0x00015801u    /* size must be < this (bank+1)        */

typedef struct {
    uint32_t magic;        /* +0x00  0xAA55AA55                               */
    uint32_t version;      /* +0x04  packed version                          */
    uint32_t crc32;        /* +0x08  CRC-32 over header(masked) + body       */
    uint32_t size;         /* +0x0C  total image size in bytes               */
    uint8_t  build[0x18];  /* +0x10  build date/time, padding                */
    /* +0x28: real Cortex-M0+ vector table (SP @ +0x28, reset @ +0x2C)       */
} image_header_t;

/* image_verify() return codes */
#define IMG_OK           0      /* magic + size + CRC all good                */
#define IMG_CRC_BAD      1      /* magic+size ok, CRC mismatch                */
#define IMG_MAGIC_BAD    2      /* magic wrong or size out of range           */

/* ===================================================================== */
/* Peripheral bases (STM32L0)                                            */
/* ===================================================================== */
#define IWDG_BASE        0x40003000u    /* independent watchdog               */
#define USART1_BASE      0x40013800u    /* comms / OTA bus (9600 8N1, PA9/10) */
#define PWR_BASE         0x40007000u
#define RCC_BASE         0x40021000u
#define FLASH_R_BASE     0x40022000u
#define CRC_BASE         0x40023000u
#define GPIOA_BASE       0x50000000u
#define GPIOB_BASE       0x50000400u
#define SCB_BASE         0xE000ED00u    /* VTOR @ +0x08, AIRCR @ +0x0C        */
#define VTOR_RAM         0x20000000u    /* relocated vector table             */

/* RCC register offsets used by the loader */
#define RCC_IOPENR       0x2Cu          /* GPIO port clock enable             */
#define RCC_AHBENR       0x30u
#define RCC_APB2ENR      0x34u          /* USART1EN = bit14                   */
#define RCC_APB1ENR      0x38u          /* PWREN    = bit28                   */
#define RCC_CSR          0x50u          /* reset flags; RMVF = bit23          */

#define IWDG_KR_RELOAD   0x0000AAAAu    /* IWDG_KR refresh / reload key       */
#define AIRCR_SYSRESET   0x05FA0004u    /* SCB_AIRCR SYSRESETREQ + VECTKEY    */

/* STM32L0 flash unlock keys (FLASH_PEKEYR / FLASH_PRGKEYR) */
#define FLASH_PEKEY1     0x89ABCDEFu
#define FLASH_PEKEY2     0x02030405u
#define FLASH_PRGKEY1    0x8C9DAEBFu
#define FLASH_PRGKEY2    0x13141516u

/* ===================================================================== */
/* Serial-download ("Who?") protocol                                    */
/* ===================================================================== */
#define OTA_ACK          0x79u          /* 'y' — accepted                     */
#define OTA_NAK          0x1Fu          /* rejected                           */
#define OTA_CMD_A        0x11u          /* write address (variant A)          */
#define OTA_CMD_VERIFY   0x21u          /* '!' — verify / finalise            */
#define OTA_CMD_ERASE    0x31u          /* '1' — write address (erase first)  */
#define OTA_LO_BOUND     0x08004FFFu    /* target must be >  this (>= AP_BASE) */

/* protocol receive states (s_ota.state) */
#define OTA_ST_IDLE      0      /* parsing "WHO?" / command header            */
#define OTA_ST_ARG       1      /* accumulating the 4-byte address + XOR      */
#define OTA_ST_DATA      2      /* streaming a 128-byte data block            */

#define USART1_IRQn      27     /* STM32L0 position 27                        */

/* ===================================================================== */
/* MMIO helpers                                                          */
/* ===================================================================== */
#define REG32(addr)      (*(volatile uint32_t *)(uintptr_t)(addr))
#define REG16(addr)      (*(volatile uint16_t *)(uintptr_t)(addr))
#define REG8(addr)       (*(volatile uint8_t  *)(uintptr_t)(addr))

/* ===================================================================== */
/* Bootloader entry points (this decomp)                                 */
/* ===================================================================== */
/* startup_stm32l072.S */
void Reset_Handler(void);
void Default_Handler(void);
void SystemInit(void);

/* main.c */
int  main(void);

/* boot.c */
void goto_application(void);          /* deinit, load app SP/PC, jump          */

/* image.c */
int  image_verify(const uint32_t *slot);
void flash_copy_image(void);          /* install Shadow -> AP, then boot       */

/* flash.c */
void flash_erase_page(uint32_t addr);
void flash_program(uint8_t *dst, uint16_t nbytes, const uint8_t *src);
int  flash_program_verify(uint32_t addr, int16_t nbytes, const uint8_t *src);
void flash_read(const uint32_t *src, uint16_t nbytes, uint8_t *dst);
void mem_copy(uint8_t *dst, const uint8_t *src, int16_t n);
void mem_copy_bytes(uint8_t *dst, const uint8_t *src, int n);

/* ota.c */
void ota_process_byte(uint8_t b);
void ota_send_response(uint8_t code);

/* uart.c */
void comms_rx_state_init(void);
void download_uart_init(void);        /* USART1 @ 9600, PA9/PA10, IRQ27        */
void uart_tx_string(const char *s);
void uart_tx_byte(uint8_t b);
void uart_rx_drain(void);
void uart_tx_pump(void);
void uart_tx_flush(void);
void comms_deinit(void);
void USART1_IRQHandler(void);

/* system.c */
void boot_hw_init(void);
void clock_periph_init(void);
void gpio_init(void);
void led_init(void);
void download_pin_check(void);
void iwdg_init(void);
int  iwdg_config(uint32_t prescaler);
void iwdg_refresh(void);
void iwdg_hal_init(void);

/* handlers.c */
void HardFault_Handler(void);
void SysTick_Handler(void);
void system_reset(void);              /* NVIC_SystemReset (no return)          */
void failsafe(void);                  /* flash-failure trap -> system_reset    */

/* ---- shared bootloader state (OEM SRAM globals, see docs/hardware.md) ---- */
extern volatile uint8_t  g_boot_events;   /* SysTick -> super-loop (0x200008BC) */
extern volatile uint8_t  g_boot_countdown;/* boot delay countdown (0x2000088C)  */
extern volatile uint8_t  g_loop_flags;    /* download/finalise flags (0x20000850)*/

/* trace / banner strings (strings.c) */
extern const char STR_BANNER_V007[];      /* "\nI am VanMoof BL V007 ...\r"     */
extern const char STR_BANNER_V006[];      /* "\nI am VanMoof BL V006 \r"        */
extern const char STR_BANNER_WHO[];       /* WHO?-handshake reply banner        */

#endif /* BMSBOOT_H */
