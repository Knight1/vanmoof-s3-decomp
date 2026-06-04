/* image.c — application-image validation and Shadow->AP install.
 *
 * Reconstructed from image_verify (0x08000C68) and flash_copy_image (0x08000D18).
 *
 * An image bank starts with the 40-byte VanMoof header (see image_header_t);
 * the stored CRC-32 covers the header (with its own crc/size words masked to
 * 0xFFFFFFFF) followed by the body words from +0x28 to +size. The CRC engine
 * is the STM32 hardware CRC unit driven through the HAL.
 */
#include "bmsboot.h"

/* STM32 HAL CRC peripheral (vendor-stock) — MPEG-2 CRC32 over little-endian
 * 32-bit words; accumulate() folds nwords into the running value. */
extern uint32_t crc32_accumulate(void *hcrc, const void *data, uint32_t nwords);
extern void    *g_hcrc;            /* CRC_HandleTypeDef (0x20000864)           */
extern void     hal_gpio_write(uint32_t port, uint16_t pins, int state);

static const uint32_t *s_img_slot;            /* 0x200007FC  bank under check  */
static uint32_t        s_hdr[IMG_HDR_SIZE / 4]; /* 0x20000804  masked header    */
static uint8_t         s_page_buf[FLASH_PAGE_SIZE]; /* 0x200006F0  copy scratch */

int image_verify(const uint32_t *slot)
{
    const image_header_t *h = (const image_header_t *)slot;
    s_img_slot = slot;

    if (h->magic != IMG_MAGIC || h->size >= IMG_MAX_SIZE)
        return IMG_MAGIC_BAD;

    /* MPEG-2 CRC32 over the whole image, but with the crc32 and imageSize header
     * words — bytes [0x08:0x10) — blanked to 0xFFFFFFFF first. imageSize bounds
     * the CRC length but is itself excluded from the CRC content. */
    mem_copy((uint8_t *)s_hdr, (const uint8_t *)slot, IMG_HDR_SIZE);
    s_hdr[2] = 0xFFFFFFFFu;                   /* +0x08 crc32 (excluded from itself) */
    s_hdr[3] = 0xFFFFFFFFu;                   /* +0x0C imageSize                    */

    REG32(CRC_BASE + 0x08) |= 1u;             /* CRC->CR.RESET — start a fresh sum  */
    crc32_accumulate(g_hcrc, s_hdr, IMG_HDR_SIZE / 4);          /* 10 header words */
    uint32_t crc = crc32_accumulate(g_hcrc, slot + (IMG_HDR_SIZE / 4),
                                    (h->size - IMG_HDR_SIZE) / 4);   /* body words */

    return (crc == h->crc32) ? IMG_OK : IMG_CRC_BAD;
}

/* flash_copy_image() — install the Shadow bank onto AP, 128 bytes at a time,
 * erase+verify with retry, then re-verify the whole AP bank and loop until it
 * passes. Clears the persisted boot flag and jumps to the application; never
 * returns. The source page is the destination page + SHADOW_OFFSET. */
void flash_copy_image(void)
{
    uint32_t dst = APP_BASE;

    hal_gpio_write(GPIOB_BASE, 0x0001u, 1);   /* status LED on (PB0)            */
    comms_deinit();                            /* drop USART1 before the long op */

    do {
        for (dst = APP_BASE; dst <= (APP_BASE + BANK_SIZE - FLASH_PAGE_SIZE); dst += FLASH_PAGE_SIZE) {
            flash_read((const uint32_t *)(dst + SHADOW_OFFSET), FLASH_PAGE_SIZE, s_page_buf);
            int err;
            do {
                flash_erase_page(dst);
                err = flash_program_verify(dst, FLASH_PAGE_SIZE, s_page_buf);
            } while (err != 0);
            REG32(IWDG_BASE) = IWDG_KR_RELOAD;     /* kick the watchdog         */
        }
    } while (image_verify((const uint32_t *)APP_BASE) != IMG_OK);

    uint8_t flag = 0;
    flash_program((uint8_t *)BOOT_FLAG_ADDR, 1, &flag);   /* clear EEPROM flag  */
    goto_application();                                    /* no return         */
}
