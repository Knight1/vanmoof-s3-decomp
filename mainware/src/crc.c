#include <stdint.h>

#include "crc.h"
#include "panic.h"   /* Error_Handler */

/* Modbus RTU CRC-16, reflected poly 0xA001 (the project-wide inter-module-bus
 * CRC). The OEM leaves the running CRC in r0 for both routines; Ghidra typed
 * them void but the callers consume the return. */

uint16_t crc16_modbus_update(uint16_t crc, uint8_t data)
{
    uint32_t v = (uint32_t)crc ^ (uint32_t)data;   /* XOR affects the low byte */

    for (int i = 0; i < 8; i++) {
        if (v & 1u) {
            v = (v >> 1) ^ 0xA001u;
        } else {
            v = v >> 1;
        }
    }
    return (uint16_t)v;
}

uint16_t crc16(const uint8_t *buf, int len, uint16_t crc)
{
    while (len != 0) {
        crc = crc16_modbus_update(crc, *buf);
        len--;
        buf++;
    }
    return crc;
}

/* STM32 hardware CRC-32 word-feed (OEM crc32_hw_feed, 0x0802320E). Marks the
 * driver busy, streams each word into CRC->DR (the OEM re-reads dev->dr every
 * iteration; `dr` is volatile-pointed so each store is a real register write),
 * reads the accumulated CRC back, marks idle. */
uint32_t crc32_hw_feed(crc_dev_t *dev, const uint32_t *src, uint32_t word_count)
{
    dev->state = 2;
    for (uint32_t i = 0; i < word_count; i++) {
        *dev->dr = src[i];
    }
    uint32_t result = *dev->dr;
    dev->state = 1;
    return result;
}

/* ── CRC peripheral clock + a device-ID hash ───────────────────────────────── */

#define CRC_INSTANCE   ((volatile uint32_t *)0x40023000u)   /* CRC base (== &CRC->DR) */
#define RCC_AHB1ENR    (*(volatile uint32_t *)0x40023830u)  /* RCC base 0x40023800 + 0x30 */
#define RCC_AHB1ENR_CRCEN  (1u << 12)

/* HAL_CRC_MspInit (OEM 0x08040288). Enable the CRC peripheral clock; the OEM
 * keeps the CubeF4 read-back-delay idiom (read the bit straight back into a
 * scratch slot after the set). */
void HAL_CRC_MspInit(crc_dev_t *hcrc)
{
    volatile uint32_t tmp;

    if (hcrc->dr != CRC_INSTANCE) {
        return;
    }
    RCC_AHB1ENR |= RCC_AHB1ENR_CRCEN;
    tmp = RCC_AHB1ENR & RCC_AHB1ENR_CRCEN;
    (void)tmp;
}

/* HAL_CRC_MspDeInit (OEM 0x080402B8). Disable the CRC peripheral clock. */
void HAL_CRC_MspDeInit(crc_dev_t *hcrc)
{
    if (hcrc->dr != CRC_INSTANCE) {
        return;
    }
    RCC_AHB1ENR &= ~RCC_AHB1ENR_CRCEN;
}

/* crc_accumulate_device_uid (OEM 0x080402E8). Copy the 96-bit device unique ID
 * to a stack buffer and run it through HAL_CRC_Accumulate; returns the CRC (the
 * OEM tail-passes HAL_CRC_Accumulate's r0 result through). */
extern uint32_t HAL_CRC_Accumulate(crc_dev_t *hcrc, uint32_t *buf, uint32_t len); /* 0x08023234 */

#define DEVICE_UID   ((volatile uint32_t *)0x1FFF7A10u)
#define CRC_HANDLE   ((crc_dev_t *)0x20009D90u)

uint32_t crc_accumulate_device_uid(void)
{
    uint32_t uid[3];

    uid[0] = DEVICE_UID[0];
    uid[1] = DEVICE_UID[1];
    uid[2] = DEVICE_UID[2];
    return HAL_CRC_Accumulate(CRC_HANDLE, uid, 3);
}

/* crc_init (OEM 0x08040268 — the HAL_CRC_Init wrapper): point the handle at the
 * CRC peripheral (Instance = &CRC->DR) and initialise it; HAL_CRC_Init enables
 * the peripheral clock via HAL_CRC_MspInit. Fatal Error_Handler on failure. */
extern int HAL_CRC_Init(crc_dev_t *hcrc);   /* 0x080231BA */

void crc_init(void)
{
    CRC_HANDLE->dr = CRC_INSTANCE;
    if (HAL_CRC_Init(CRC_HANDLE) != 0) {
        Error_Handler();
    }
}

/* ahb1_periph_handle_deinit (OEM 0x080402D8) — NOTE: an OEM misnomer. It de-inits
 * the CRC peripheral handle (HAL_CRC_DeInit gates the CRC AHB1 clock off via
 * HAL_CRC_MspDeInit), one leg of enter_stop_mode's pre-sleep teardown. */
extern int HAL_CRC_DeInit(crc_dev_t *hcrc);   /* 0x080231D8 */

void ahb1_periph_handle_deinit(void)
{
    HAL_CRC_DeInit(CRC_HANDLE);
}
