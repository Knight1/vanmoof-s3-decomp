/* powerbankboot.h — VanMoof PowerBank / battery-module bootloader
 *
 * Prototypes, memory map, peripheral bases and protocol constants for the
 * STM32F091xC loader, reconstructed from powerbank_bootloader_1.00.bin.
 */
#ifndef POWERBANKBOOT_H
#define POWERBANKBOOT_H

#include <stdint.h>

/* ===================================================================== */
/* Flash memory map                                                      */
/* ===================================================================== */
#define BOOT_BASE        0x08000000u            /* loader (this image), 32 KB */
#define AP_BASE          0x08008000u            /* application bank,    112 KB */
#define SHADOW_BASE      0x08024000u            /* backup / OTA staging,112 KB */
#define BANK_SIZE        0x0001C000u            /* 112 KB per app bank         */
#define FLASH_PAGE_SIZE  0x800u                 /* 2 KB erase page             */

/* VanMoof application image header (prefixes AP / Shadow banks) */
#define IMG_MAGIC        0xAA55AA55u            /* header[0]                   */
#define IMG_HDR_SIZE     0x28u                  /* header bytes before vectors */
#define IMG_MAX_SIZE     0x0001C001u            /* size must be < this         */

typedef struct {
    uint32_t magic;        /* +0x00  0xAA55AA55                              */
    uint32_t version;      /* +0x04  e.g. 0x011105B2 (1.11.05, type 0xB2)    */
    uint32_t crc32;        /* +0x08  CRC-32 over words [+0x28 .. +size)      */
    uint32_t size;         /* +0x0C  total image size in bytes               */
    uint8_t  build_date[12];/* +0x10                                          */
    uint8_t  build_time[9]; /* +0x1C                                          */
    uint8_t  reserved[3];  /* +0x25                                           */
    /* +0x28: real Cortex-M0 vector table (SP @ +0x28, reset @ +0x2C)        */
} image_header_t;

/* image_verify() return codes */
#define IMG_OK           0      /* magic + size + CRC all good               */
#define IMG_CRC_BAD      1      /* magic+size ok, CRC mismatch               */
#define IMG_MAGIC_BAD    2      /* magic wrong or size out of range          */

/* ===================================================================== */
/* Peripheral bases (STM32F0)                                            */
/* ===================================================================== */
#define RTC_BASE         0x40002800u
#define IWDG_BASE        0x40003000u
#define USART2_BASE      0x40004400u            /* comms / OTA bus           */
#define USART1_BASE      0x40013800u            /* debug trace console       */
#define RCC_BASE         0x40021000u
#define FLASH_R_BASE     0x40022000u
#define GPIOA_BASE       0x48000000u

#define IWDG_KR_RELOAD   0x0000AAAAu            /* IWDG_KR refresh key        */

/* RTC backup-register indices used for the persisted boot flag */
#define BKP_UPGRADE      0      /* BKP0R = fBMS_Upgrade_Finish value          */
#define BKP_UPGRADE_INV  1      /* BKP1R = its bitwise complement (redundancy)*/

/* ===================================================================== */
/* Serial-download ("Who?") protocol                                     */
/* ===================================================================== */
#define OTA_ACK          0x79u                  /* 'y' — accepted             */
#define OTA_NAK          0x1Fu                  /* rejected                   */
#define OTA_CMD_ERASE    0x31u                  /* '1' — prepare/erase target */
#define OTA_CMD_VERIFY   0x21u                  /* '!' — verify/finalize      */

/* protocol receive states (s_ota.state) */
#define OTA_ST_IDLE      0      /* parsing the "Who?" / command header        */
#define OTA_ST_ARG       1      /* accumulating the 4-byte address + XOR      */
#define OTA_ST_DATA      2      /* streaming a data block                     */

/* ===================================================================== */
/* MMIO helpers                                                          */
/* ===================================================================== */
#define REG32(addr)      (*(volatile uint32_t *)(uintptr_t)(addr))
#define REG16(addr)      (*(volatile uint16_t *)(uintptr_t)(addr))

/* ===================================================================== */
/* Bootloader entry points (this decomp)                                 */
/* ===================================================================== */
/* startup_stm32f091.S */
void init_data_bss(void);

/* main.c */
int  main(void);

/* boot.c */
void boot_main(void);                 /* A/B orchestrator + download server   */
void goto_application(void);          /* pet IWDG, load app SP/PC, jump        */

/* image.c */
int  image_verify(const uint32_t *slot);
void flash_copy_image(uint32_t dst, uint32_t src);

/* flash.c */
void flash_erase_page(uint32_t addr);
int  flash_program(uint32_t *dst, uint16_t nbytes, const uint8_t *src);
void flash_read(const uint32_t *src, uint16_t nbytes, uint8_t *dst);
void mem_copy(uint8_t *dst, const uint8_t *src, int16_t n);

/* ota.c */
void ota_process_byte(uint8_t b);
void ota_send_response(uint8_t code);
void ota_reset(void);                 /* clear the download state machine     */

/* ---- shared bootloader state (OEM SRAM globals, see docs/hardware.md) ---- */
extern uint8_t          g_upgrade_finished;  /* fBMS_Upgrade_Finish (0x20000BB4) */
extern volatile uint8_t g_boot_events;       /* SysTick → server loop (0x20000BBC)*/
extern uint8_t          g_loop_mode;         /* download/finalize mode (0x20000B50)*/

/* uart.c */
void uart_tx_string(const char *s);
void uart_tx_byte(uint8_t b);
void uart_rx_drain(void);
void uart_tx_pump(void);
void uart_tx_flush(void);
void USART2_IRQHandler(void);

/* system.c */
void boot_hw_init(void);
void clock_periph_init(void);
void iwdg_init(void);
void iwdg_refresh(void);
void gpio_init(void);
void comms_rx_state_init(void);
void boot_read_persistent_flags(void);
void store_boot_flag(uint8_t v);
uint32_t rtc_bkp_read(void *hrtc, int idx);
void rtc_bkp_write(void *hrtc, int idx, uint32_t v);

/* handlers.c */
void NMI_Handler(void);
void HardFault_Handler(void);
void SVC_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);
void TIM6_DAC_IRQHandler(void);

/* ===================================================================== */
/* Vendor-stock leaves (X-CUBE-STL / HAL / tinyprintf) — extern for now  */
/* ===================================================================== */
void dbg_printf(const char *fmt, ...);   /* gated trace logger (USART1)       */
void stl_failsafe(void);                  /* IEC-60730 fail-safe (no return)   */

/* trace / banner strings (strings.c) */
extern const char STR_BANNER[];           /* "\nI am VM-BATT BL\r"             */
extern const char STR_IN_BL[];            /* "-->In BL \n\r"                   */
extern const char STR_UPGRADE_FIN[];      /* "-->fBMS_Upgrade_Finish = 1 \n\r" */
extern const char STR_CRC_VERIFY[];       /* "-->CRC Verify 0x%4x%4x = "       */
extern const char STR_RC_OK[];            /* "0\n\r"                           */
extern const char STR_RC_CRC[];           /* "1\n\r"                           */
extern const char STR_RC_MAGIC[];         /* "2\n\r"                           */
extern const char STR_COPY_AP2SH[];       /* "Copy AP to Shadow"               */
extern const char STR_COPY_SH2AP[];       /* "Copy Shadow to AP"               */
extern const char STR_DONE[];             /* "--> Done\n\r"                    */
extern const char STR_GOTO_APP[];         /* "-->Goto_Application()\n\r"       */
extern const char STR_WRITE_FLASH_NG[];   /* "Write Flash NG\n\r"             */
extern const char STR_HARDFAULT_NG[];     /* "HardFault NG\n\r"               */
extern const char STR_SVC_NG[];           /* "SVC NG\n\r"                     */
extern const char STR_NMI_NG1[];          /* "NMI NG1\n\r"                    */
extern const char STR_NMI_NG2[];          /* "NMI NG2\n\r"                    */

#endif /* POWERBANKBOOT_H */
