#include "batteryware.h"

/* CRC-16 polynomial used by VanMoof (Modbus CRC-16) */
#define CRC16_POLY  0xA001

/*
 * Modbus CRC-16 calculation.
 *
 * Standard Modbus CRC-16: init = 0xFFFF, polynomial 0xA001.
 * Processes 'len' bytes from 'data' and returns the 16-bit CRC.
 * Used for command frame validation in the Modbus protocol handler.
 */
uint16_t crc16_calc(uint8_t *data, int16_t len)
{
    uint16_t crc = 0xFFFF;

    while (len-- != 0) {
        crc ^= *data++;

        for (uint8_t i = 0; i < 8; i++) {
            if ((crc & 1) != 0) {
                crc = (crc >> 1) ^ CRC16_POLY;
            } else {
                crc = crc >> 1;
            }
        }
    }

    return crc;
}

/* CRC-8 polynomial used for flash/firmware verification */
#define CRC8_POLY   0x07

/*
 * CRC-8 calculation.
 *
 * Polynomial 0x07, initial value 0xFF. Processes 'len' bytes.
 * Used for firmware page verification in the flash programming path.
 */
uint8_t crc8_calc(uint8_t *data, int8_t len)
{
    uint8_t crc = 0xFF;

    while (len-- != 0) {
        crc ^= *data++;

        for (uint8_t i = 0; i < 8; i++) {
            if (crc & 0x80) {
                crc = (uint8_t)(crc << 1) ^ CRC8_POLY;
            } else {
                crc = crc << 1;
            }
        }
    }

    return crc;
}

/*
 * CRC-8/SMBus PEC over `len` bytes.
 *
 * Polynomial 0x07, init 0x00 (matches SMBus packet-error-checking
 * spec — the FEDL5236 datasheet describes this byte appended to each
 * read response). Referenced from `spi.c::smbus_read` /
 * `smbus_write_reg` to validate and append the PEC byte.
 *
 * OEM has its own 1834-byte implementation at `FUN_080141e4`
 * (`crc8_for_smbus`) — table-driven, byte-divergent. This C version
 * computes the same value via bitwise iteration. `crc8_verify` is the
 * read-side helper (same computation; callers compare its return
 * against the appended PEC byte).
 */
uint32_t crc8_for_smbus(uint8_t *data, uint32_t len)
{
    uint8_t crc = 0x00;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];

        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ CRC8_POLY);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

uint8_t crc8_verify(uint8_t *data, uint32_t len)
{
    return (uint8_t)crc8_for_smbus(data, len);
}

/*
 * CRC peripheral handle layout (STM32L0 CRC, base 0x40023000).
 *
 * Offsets match what FUN_0800edf0 accesses; this is the standard
 * HAL CRC_HandleTypeDef layout under -Os:
 *   +0  Instance                  (CRC_TypeDef *)
 *   +4  Init.DefaultPolynomialUse (uint8_t, 0 = use 0x04C11DB7)
 *   +5  Init.DefaultInitValueUse  (uint8_t, 0 = use 0xFFFFFFFF)
 *   +6  Init.InputDataFormat      (uint8_t — not touched by Init)
 *   +8  Init.GeneratingPolynomial (uint32_t — only if custom)
 *   +12 Init.CRCLength            (uint32_t — POLYSIZE encoding 0/8/16/24)
 *   +16 Init.InitValue            (uint32_t — only if custom init)
 *   +20 Init.InputDataInversionMode  (uint32_t — CR bits 5..6)
 *   +24 Init.OutputDataInversionMode (uint32_t — CR bit 7)
 *   +28 Lock                      (uint8_t)
 *   +29 State                     (uint8_t)
 *
 * Instance register map (word indices used here):
 *   [2] = CR    (0x08)  — POLYSIZE [4:3], REV_IN [6:5], REV_OUT [7]
 *   [4] = INIT  (0x10)
 *   [5] = POL   (0x14)
 */

#define CRC_STATE_RESET   0u
#define CRC_STATE_READY   1u
#define CRC_STATE_BUSY    2u

#define CRC_LOCK_UNLOCKED 0u

#define CRC_DEFAULT_POLY  0x04C11DB7u  /* IEEE 802.3 / ANSI X3.66 */

#define CRC_CR_POLYSIZE_Msk  0x18u
#define CRC_CR_REV_IN_Msk    0x60u
#define CRC_CR_REV_OUT_Msk   0x80u

typedef struct {
    volatile uint32_t *Instance;         /* +0  */
    uint8_t  DefaultPolynomialUse;       /* +4  */
    uint8_t  DefaultInitValueUse;        /* +5  */
    uint8_t  InputDataFormat;            /* +6  */
    uint8_t  _pad7;                      /* +7  */
    uint32_t GeneratingPolynomial;       /* +8  */
    uint32_t CRCLength;                  /* +12 */
    uint32_t InitValue;                  /* +16 */
    uint32_t InputDataInversionMode;     /* +20 */
    uint32_t OutputDataInversionMode;    /* +24 */
    uint8_t  Lock;                       /* +28 */
    uint8_t  State;                      /* +29 */
} crc_handle_t;

/*
 * HAL_CRC_MspInit equivalent — clock enable for the CRC peripheral
 * (FUN_0800eebc). Empty thunk in this firmware: the OEM enables
 * RCC_AHBENR.CRCEN in a sibling init path, so the MSP callback is
 * the standard weak-empty body. Kept as its own symbol so the call
 * site in `crc_init` matches the OEM byte sequence.
 */
void crc_msp_init(void *hcrc)
{
    (void)hcrc;
}

/*
 * HAL_CRCEx_Polynomial_Set equivalent (FUN_0800f188).
 *
 * Validates that `poly` fits in the bit length implied by `length`
 * (POLYSIZE encoding: 24=7-bit, 16=8-bit, 8=16-bit, 0=32-bit), then
 * programs CRC->POL and CRC->CR.POLYSIZE. Returns 1 on validation
 * failure, 0 on success.
 */
uint8_t crcex_polynomial_set(void *hcrc_v, uint32_t poly, uint32_t length)
{
    crc_handle_t *hcrc = (crc_handle_t *)hcrc_v;
    uint8_t err = 0;
    int32_t msb = 31;

    while (msb-- != 0) {
        if (((poly >> (msb & 0x1F)) & 1u) != 0) {
            break;
        }
    }

    if (length == 24) {
        if (msb > 6) err = 1;
    } else if (length > 24) {
        err = 1;
    } else if (length == 16) {
        if (msb > 7) err = 1;
    } else if (length > 16) {
        err = 1;
    } else if (length == 0) {
        /* 32-bit poly: any msb is valid */
    } else if (length == 8) {
        if (msb > 15) err = 1;
    } else {
        err = 1;
    }

    if (err == 0) {
        hcrc->Instance[5] = poly;
        hcrc->Instance[2] =
            (hcrc->Instance[2] & ~CRC_CR_POLYSIZE_Msk) | length;
    }
    return err;
}

/*
 * HAL_CRC_Init equivalent (FUN_0800edf0).
 *
 * Initialises the CRC peripheral from `hcrc->Init`. On the first call
 * (State == RESET) it unlocks the handle and runs the MSP init hook.
 * Programs polynomial (default 0x04C11DB7 / POLYSIZE_32B, or custom
 * via crcex_polynomial_set), initial value (default 0xFFFFFFFF or
 * custom), and the REV_IN / REV_OUT data-inversion bits in CR.
 *
 * Returns 0 on success, 1 on null handle or polynomial validation
 * failure.
 *
 * Previously decompiled as `dma_channel_init` in dma.c — the literal
 * pool entry at flash 0x0800eeb8 (the 0x04C11DB7 constant) was being
 * read back as the address rather than the value, and the Lock byte
 * was being cleared as a 32-bit store instead of `strb`. Both fixed
 * here so the C source matches the OEM byte sequence.
 */
uint32_t crc_init(void *hcrc_v)
{
    crc_handle_t *hcrc = (crc_handle_t *)hcrc_v;

    if (hcrc == NULL) {
        return 1;
    }

    if (hcrc->State == CRC_STATE_RESET) {
        hcrc->Lock = CRC_LOCK_UNLOCKED;
        crc_msp_init(hcrc);
    }

    hcrc->State = CRC_STATE_BUSY;

    if (hcrc->DefaultPolynomialUse == 0) {
        hcrc->Instance[5] = CRC_DEFAULT_POLY;
        hcrc->Instance[2] &= ~CRC_CR_POLYSIZE_Msk;
    } else {
        if (crcex_polynomial_set(hcrc,
                                 hcrc->GeneratingPolynomial,
                                 hcrc->CRCLength) != 0) {
            return 1;
        }
    }

    if (hcrc->DefaultInitValueUse == 0) {
        hcrc->Instance[4] = 0xFFFFFFFFu;
    } else {
        hcrc->Instance[4] = hcrc->InitValue;
    }

    hcrc->Instance[2] =
        (hcrc->Instance[2] & ~CRC_CR_REV_IN_Msk) | hcrc->InputDataInversionMode;
    hcrc->Instance[2] =
        (hcrc->Instance[2] & ~CRC_CR_REV_OUT_Msk) | hcrc->OutputDataInversionMode;

    hcrc->State = CRC_STATE_READY;
    return 0;
}
