/* image.c — application-image validation and bank-to-bank copy.
 *
 * Reconstructed from image_verify (0x08001750) and flash_copy_image (0x08001824).
 *
 * An image bank starts with the 40-byte VanMoof header (see image_header_t);
 * the stored CRC-32 covers the header (with its own crc/size words masked to
 * 0xFFFFFFFF) followed by the body words from +0x28 to +size. The CRC engine
 * is the STM32 hardware CRC unit driven through the HAL.
 */
#include "powerbankboot.h"

/* STM32 HAL CRC peripheral (vendor-stock). crc32_calc resets the unit and
 * folds in the first run of words; crc32_accumulate continues without reset. */
extern uint32_t crc32_calc(void *hcrc, const void *data, uint32_t nwords);
extern uint32_t crc32_accumulate(void *hcrc, const void *data, uint32_t nwords);
extern void    *g_hcrc;            /* CRC_HandleTypeDef (0x20000B64)           */

/* 2 KB scratch page used to ferry data between banks (OEM SRAM buffer). */
static uint8_t s_page_buf[FLASH_PAGE_SIZE];

int image_verify(const uint32_t *slot)
{
    const image_header_t *h = (const image_header_t *)slot;

    /* Log which bank is being checked: "-->CRC Verify 0x%4x%4x = " */
    dbg_printf(STR_CRC_VERIFY, (uint32_t)((uintptr_t)slot >> 16),
                              (uint32_t)((uintptr_t)slot & 0xFFFF));

    if (h->magic != IMG_MAGIC || h->size >= IMG_MAX_SIZE) {
        dbg_printf(STR_RC_MAGIC);          /* "2\n\r" */
        return IMG_MAGIC_BAD;
    }

    /* CRC the header with the variable words blanked, then the body. */
    uint32_t hdr[IMG_HDR_SIZE / 4];
    mem_copy((uint8_t *)hdr, (const uint8_t *)slot, IMG_HDR_SIZE);
    hdr[2] = 0xFFFFFFFFu;                   /* crc32 field excluded from itself */
    hdr[3] = 0xFFFFFFFFu;                   /* size field blanked, like the OEM */

    uint32_t crc = crc32_calc(g_hcrc, hdr, IMG_HDR_SIZE / 4);
    crc = crc32_accumulate(g_hcrc, slot + (IMG_HDR_SIZE / 4),
                           (h->size - IMG_HDR_SIZE) / 4);

    if (crc == h->crc32) {
        dbg_printf(STR_RC_OK);             /* "0\n\r" */
        return IMG_OK;
    }
    dbg_printf(STR_RC_CRC);                /* "1\n\r" */
    return IMG_CRC_BAD;
}

/* flash_copy_image() — mirror one 112 KB bank onto the other, 2 KB at a time,
 * erase+program with retry, then re-verify the whole destination and loop until
 * it passes. Direction is logged from the source address. */
void flash_copy_image(uint32_t dst, uint32_t src)
{
    dbg_printf(src == AP_BASE ? STR_COPY_AP2SH : STR_COPY_SH2AP);

    do {
        for (uint32_t off = 0; off < BANK_SIZE; off += FLASH_PAGE_SIZE) {
            flash_read((const uint32_t *)(src + off), FLASH_PAGE_SIZE, s_page_buf);
            int err;
            do {
                flash_erase_page(dst + off);
                err = flash_program((uint32_t *)(dst + off), FLASH_PAGE_SIZE, s_page_buf);
            } while (err != 0);
        }
    } while (image_verify((const uint32_t *)dst) != IMG_OK);

    dbg_printf(STR_DONE);                  /* "--> Done\n\r" */
}
