#include "powerbankware.h"

/*
 * Hardware CRC peripheral — HAL_CRC_Accumulate and the BMS-record wrapper.
 *
 *   bms_record_crc   = OEM FUN_080142e4  (wrapper over the fixed handle)
 *   crc_accumulate   = OEM FUN_08019e04  (HAL_CRC_Calculate: reset then feed)
 *   crc_continue     = OEM FUN_08019d5e  (HAL_CRC_Accumulate: feed, no reset)
 *   crc_feed_*       = OEM FUN_08019fc8 / FUN_08019eba (halfword / byte feed)
 *
 * The CRC unit's data register is fed per the handle's InputDataFormat
 * (handle+0x20: 1=bytes, 2=halfwords, 3=words); the result is read back from
 * the data register. The CRC handle is fixed at 0x200006c0.
 *
 * HAL_CRC_HandleTypeDef offsets used:
 *   +0x00 Instance  +0x1c Lock  +0x1d State  +0x20 InputDataFormat
 * CRC registers (Instance): +0x00 DR, +0x08 CR (bit 0 = RESET).
 */

#define CRC_HANDLE  ((void *)0x200006c0)

#define CRC_INSTANCE(h) (*(volatile uint32_t **)((uint8_t *)(h) + 0))
#define CRC_DR(h)       (CRC_INSTANCE(h)[0])     /* +0x00 */
#define CRC_CR(h)       (CRC_INSTANCE(h)[2])     /* +0x08 */
#define CRC_LOCK(h)     (*(volatile uint8_t *)((uint8_t *)(h) + 0x1c))
#define CRC_STATE(h)    (*(volatile uint8_t *)((uint8_t *)(h) + 0x1d))
#define CRC_FORMAT(h)   (*(volatile uint8_t *)((uint8_t *)(h) + 0x20))

/* FUN_08019fc8 — feed `count` halfwords (the OEM swaps the pair order). */
static uint32_t crc_feed_halfword(uint8_t *h, const uint16_t *data, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count >> 1; i++) {
        CRC_DR(h) = ((uint32_t)data[i * 2] << 16) | data[i * 2 + 1];
    }
    if ((count & 1) != 0) {
        *(volatile uint16_t *)&CRC_DR(h) = data[i * 2];
    }
    return CRC_DR(h);
}

/* FUN_08019eba — feed `count` bytes, big-endian into each 32-bit word. */
static uint32_t crc_feed_byte(uint8_t *h, const uint8_t *data, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count >> 2; i++) {
        CRC_DR(h) = ((uint32_t)data[i * 4] << 24) |
                    ((uint32_t)data[i * 4 + 1] << 16) |
                    ((uint32_t)data[i * 4 + 2] << 8) |
                    data[i * 4 + 3];
    }
    switch (count & 3) {
    case 1:
        *(volatile uint8_t *)&CRC_DR(h) = data[i * 4];
        break;
    case 2:
        *(volatile uint16_t *)&CRC_DR(h) =
            (uint16_t)(((uint16_t)data[i * 4] << 8) | data[i * 4 + 1]);
        break;
    case 3:
        *(volatile uint16_t *)&CRC_DR(h) =
            (uint16_t)(((uint16_t)data[i * 4] << 8) | data[i * 4 + 1]);
        *(volatile uint8_t *)&CRC_DR(h) = data[i * 4 + 2];
        break;
    default:
        break;
    }
    return CRC_DR(h);
}

/*
 * HAL_CRC_Accumulate (FUN_08019e04): reset the unit, feed `len` data items
 * (word/halfword/byte per InputDataFormat), return the accumulated CRC.
 * Returns 2 (HAL_BUSY) if the handle is locked.
 */
uint32_t crc_accumulate(void *handle, const void *buf, uint32_t len)
{
    uint8_t *h = handle;
    uint32_t result = 0;

    if (CRC_LOCK(h) == 1) {
        return 2;
    }
    CRC_LOCK(h)  = 1;
    CRC_STATE(h) = 2;
    CRC_CR(h) |= 1u;                 /* reset the CRC computation */

    switch (CRC_FORMAT(h)) {
    case 2:
        result = crc_feed_halfword(h, (const uint16_t *)buf, len);
        break;
    case 3: {
        const uint32_t *w = (const uint32_t *)buf;
        for (uint32_t i = 0; i < len; i++) {
            CRC_DR(h) = w[i];
        }
        result = CRC_DR(h);
        break;
    }
    case 1:
        result = crc_feed_byte(h, (const uint8_t *)buf, len);
        break;
    default:
        break;
    }

    CRC_STATE(h) = 1;
    CRC_LOCK(h)  = 0;
    return result;
}

/*
 * HAL_CRC_Accumulate (FUN_08019d5e): identical to crc_accumulate but does NOT
 * reset the unit first — it feeds `len` more data items into the running CRC,
 * continuing from the current data-register value. Used to CRC an image body on
 * top of its already-accumulated header.
 */
uint32_t crc_continue(void *handle, const void *buf, uint32_t len)
{
    uint8_t *h = handle;
    uint32_t result = 0;

    if (CRC_LOCK(h) == 1) {
        return 2;
    }
    CRC_LOCK(h)  = 1;
    CRC_STATE(h) = 2;

    switch (CRC_FORMAT(h)) {
    case 2:
        result = crc_feed_halfword(h, (const uint16_t *)buf, len);
        break;
    case 3: {
        const uint32_t *w = (const uint32_t *)buf;
        for (uint32_t i = 0; i < len; i++) {
            CRC_DR(h) = w[i];
        }
        result = CRC_DR(h);
        break;
    }
    case 1:
        result = crc_feed_byte(h, (const uint8_t *)buf, len);
        break;
    default:
        break;
    }

    CRC_STATE(h) = 1;
    CRC_LOCK(h)  = 0;
    return result;
}

/* FUN_080142e4 — record CRC over the fixed CRC handle. */
uint32_t bms_record_crc(const void *buf, uint16_t len)
{
    return crc_accumulate(CRC_HANDLE, buf, len);
}

/* hal_crc_msp_init — OEM FUN_08019d4e (HAL_CRC_MspInit): empty weak callback
 * (the CRC clock is enabled directly in crc_init). */
void hal_crc_msp_init(void *hcrc)
{
    (void)hcrc;
}
extern int  FUN_0801a038(void *hcrc);   /* CRC default-polynomial program (own pass) */

/*
 * hal_crc_init — OEM FUN_08019cb0 (HAL_CRC_Init).
 * MSP init, program the polynomial engine, load the init value (0xFFFFFFFF when
 * DefaultInitValueUse, byte +0x05, == 0; else the handle InitValue at +0x10)
 * into CRC->INIT (+0x10), then fold InputDataInversionMode (+0x14 -> CR bits
 * 6:5) and OutputDataInversionMode (+0x18 -> CR bit 7) into CRC->CR (+0x08).
 * State +0x1d (2 = BUSY, 1 = READY). Widths/offsets disasm-confirmed.
 */
int hal_crc_init(void *handle)
{
    if (handle == NULL) {
        return 1;
    }
    uint8_t *h = (uint8_t *)handle;

    if (h[0x1d] == 0) {                             /* State == RESET */
        h[0x1c] = 0;                                /* Lock = UNLOCKED */
        hal_crc_msp_init(handle);                   /* HAL_CRC_MspInit */
    }
    h[0x1d] = 2;                                    /* State = BUSY */

    if (FUN_0801a038(handle) != 0) {
        return 1;
    }

    volatile uint32_t *inst = *(volatile uint32_t **)h;   /* Instance */
    if (h[5] == 0) {                                /* DefaultInitValueUse (default) */
        inst[4] = 0xffffffffu;                      /* INIT (+0x10) = default */
    } else {
        inst[4] = *(uint32_t *)(h + 0x10);          /* INIT = handle InitValue */
    }

    inst[2] = (inst[2] & 0xffffff9fu) | *(uint32_t *)(h + 0x14);   /* CR InputDataInv (6:5) */
    inst[2] = (inst[2] & 0xffffff7fu) | *(uint32_t *)(h + 0x18);   /* CR OutputDataInv (7) */

    h[0x1d] = 1;                                    /* State = READY */
    return 0;
}

/*
 * crc_init — OEM FUN_08012188. One of board_init's peripheral sub-inits.
 * Fill the CRC HAL handle (Instance = CRC 0x40023000, polynomial/init defaults
 * disabled, no in/out data inversion, InputDataFormat = 3 = words), enable the
 * CRC clock (AHBENR bit6), then run HAL_CRC_Init. Field widths (byte at +0x04/
 * +0x05, word at +0x14/+0x18/+0x20) disasm-confirmed against the OEM image.
 */
void crc_init(void)
{
    uint8_t * const h = (uint8_t *)CRC_HANDLE;                 /* 0x200006c0 */
    volatile uint32_t * const rcc_ahbenr = (volatile uint32_t *)(0x40021000 + 0x14);

    *(volatile uint32_t **)(h + 0x00) = (volatile uint32_t *)0x40023000u; /* Instance */
    *(volatile uint8_t  *)(h + 0x04) = 0;   /* DefaultPolynomialUse    */
    *(volatile uint8_t  *)(h + 0x05) = 0;   /* DefaultInitValueUse     */
    *(volatile uint32_t *)(h + 0x14) = 0;   /* InputDataInversionMode  */
    *(volatile uint32_t *)(h + 0x18) = 0;   /* OutputDataInversionMode */
    *(volatile uint32_t *)(h + 0x20) = 3;   /* InputDataFormat = WORDS */

    *rcc_ahbenr |= 0x40u;  (void)(*rcc_ahbenr & 0x40u);        /* CRCEN + read-back */

    if (hal_crc_init(CRC_HANDLE) != 0) {
        spi_error_reset();
    }
}
