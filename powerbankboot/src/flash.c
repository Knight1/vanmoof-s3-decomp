/* flash.c — flash erase / program / read primitives.
 *
 * Reconstructed from flash_erase_page (0x08001E94), flash_program (0x08001F1C),
 * flash_read (0x08002008) and mem_copy (0x08001A28).
 *
 * The erase/program wrappers drive the STM32F0 HAL flash driver: the OEM builds
 * a FLASH_EraseInitTypeDef{ TYPEERASE_PAGES, PageAddress=addr, NbPages=1 } and
 * calls HAL_FLASHEx_Erase, retrying up to 50 times before tripping the STL
 * fail-safe. Programming is word-at-a-time with an immediate read-back verify.
 */
#include "powerbankboot.h"

/* STM32F0 HAL flash driver (vendor-stock). Return 0 == HAL_OK. */
extern void     flash_hal_unlock(void);
extern void     flash_hal_clear_errors(void);
extern uint32_t flash_hal_erase_page(uint32_t page_addr, uint32_t *page_error);
extern uint32_t flash_hal_program_word(uint32_t addr, uint32_t data);

#define ERASE_RETRY_LIMIT  0x32u   /* 50 attempts before fail-safe */

/* flash_erase_page() — erase the 2 KB page containing addr (retry, then trap). */
void flash_erase_page(uint32_t addr)
{
    uint8_t tries = 0;
    uint32_t page_error;

    flash_hal_unlock();
    do {
        page_error = 0;
        if (flash_hal_erase_page(addr, &page_error) == 0) {
            tries = ERASE_RETRY_LIMIT;          /* success — leave the loop     */
        } else if (++tries > (ERASE_RETRY_LIMIT - 1)) {
            dbg_printf(STR_WRITE_FLASH_NG);
            stl_failsafe();                     /* no return                    */
        }
    } while (tries < ERASE_RETRY_LIMIT);
}

/* flash_program() — program nbytes (word-aligned) from src into dst flash,
 * verifying each word by read-back. Returns 0 on success, 1 on any error. */
int flash_program(uint32_t *dst, uint16_t nbytes, const uint8_t *src)
{
    uint16_t i = 0;
    uint32_t *p = dst;

    while (i < nbytes) {
        uint32_t w = (uint32_t)src[i]
                   | (uint32_t)src[(uint16_t)(i + 1)] << 8
                   | (uint32_t)src[(uint16_t)(i + 2)] << 16
                   | (uint32_t)src[(uint16_t)(i + 3)] << 24;
        i = (uint16_t)(i + 4);

        flash_hal_clear_errors();
        if (flash_hal_program_word((uint32_t)(uintptr_t)p, w) != 0)
            return 1;
        if (*p != w)                            /* read-back verify             */
            return 1;
        p++;
    }
    return 0;
}

/* flash_read() — copy nbytes out of flash (word reads) into a byte buffer. */
void flash_read(const uint32_t *src, uint16_t nbytes, uint8_t *dst)
{
    uint16_t i = 0;
    const uint32_t *p = src;

    while (i < nbytes) {
        uint32_t w = *p++;
        if (i < nbytes) dst[i++] = (uint8_t)(w);
        if (i < nbytes) dst[i++] = (uint8_t)(w >> 8);
        if (i < nbytes) dst[i++] = (uint8_t)(w >> 16);
        if (i < nbytes) dst[i++] = (uint8_t)(w >> 24);
    }
}

/* mem_copy() — byte copy with a signed 16-bit count (OEM helper). */
void mem_copy(uint8_t *dst, const uint8_t *src, int16_t n)
{
    for (int16_t i = n; i != 0; i--)
        *dst++ = *src++;
}
