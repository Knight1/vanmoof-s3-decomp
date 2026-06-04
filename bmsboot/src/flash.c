/* flash.c — flash / EEPROM erase, program and read primitives.
 *
 * Reconstructed from flash_erase_page (0x080013D8), flash_program (0x080015CE),
 * flash_program_verify (0x08001468), flash_read (0x080014E0), mem_copy
 * (0x08000ED4) and mem_copy_bytes (0x080048B4).
 *
 * The wrappers drive the STM32L0 HAL flash driver. STM32L0 flash organises into
 * 128-byte pages (erase) and 64-byte half-pages (fast program); the data EEPROM
 * at 0x08080000 is byte-writable via the same controller (flash_program()).
 * flash_erase_page() retries up to 50 times before tripping the fail-safe
 * (which resets the chip).
 */
#include "bmsboot.h"

/* STM32L0 HAL flash driver (vendor-stock). Return 0 == HAL_OK. */
extern void     hal_flash_unlock(void);
extern uint32_t hal_flash_erase(void *erase_init, uint32_t *page_error);
extern uint8_t  hal_flash_program(int type, uint8_t *addr, uint32_t data);
extern uint32_t hal_flash_program_halfpage(uint32_t addr, const uint8_t *src);

#define ERASE_RETRY_LIMIT  0x32u   /* 50 attempts before fail-safe */

/* HAL FLASH_EraseInitTypeDef{ TypeErase, PageAddress, NbPages } (OEM SRAM). */
static struct { uint32_t type_erase, page_addr, nb_pages; } s_erase;
static uint32_t s_page_error;
static uint8_t  s_erase_tries;

/* flash_erase_page() — erase the 128-byte page at addr (retry, then trap). */
void flash_erase_page(uint32_t addr)
{
    hal_flash_unlock();
    s_erase.type_erase = 0;             /* FLASH_TYPEERASE_PAGES */
    s_erase.page_addr  = addr;
    s_erase.nb_pages   = 1;

    s_erase_tries = 0;
    do {
        s_page_error = 0;
        if (hal_flash_erase(&s_erase, &s_page_error) == 0) {
            s_erase_tries = ERASE_RETRY_LIMIT;          /* success — leave loop */
        } else if (++s_erase_tries > (ERASE_RETRY_LIMIT - 1)) {
            failsafe();                                 /* no return            */
        }
    } while (s_erase_tries < ERASE_RETRY_LIMIT);
}

/* flash_program() — byte-program nbytes from src into dst, read-back-verifying
 * each byte (re-programs until it reads back). Used for the EEPROM boot flag
 * and the saved reset-cause word. */
void flash_program(uint8_t *dst, uint16_t nbytes, const uint8_t *src)
{
    for (uint16_t i = 0; i < nbytes; i++) {
        do {
            hal_flash_program(0, dst, *src);   /* type 0 == byte program */
        } while (*src != *dst);
        dst++;
        src++;
    }
}

/* flash_program_verify() — program nbytes from src into flash at addr in
 * 64-byte half-pages (the STM32L0 fast-program unit). Returns 1 on any
 * half-page error, 0 on success. */
int flash_program_verify(uint32_t addr, int16_t nbytes, const uint8_t *src)
{
    do {
        if (hal_flash_program_halfpage(addr, src) != 0)
            return 1;
        addr   += 0x40;
        src    += 0x40;
        nbytes  = (int16_t)(nbytes - 0x40);
    } while (nbytes != 0);
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

/* mem_copy_bytes() — byte copy with an int count (used to relocate the vector
 * table from flash into SRAM in boot_hw_init). */
void mem_copy_bytes(uint8_t *dst, const uint8_t *src, int n)
{
    for (int i = 0; i != n; i++)
        dst[i] = src[i];
}

/* mem_set() — byte fill (OEM helper at 0x080048C6; the HAL config structs are
 * zeroed through it before their fields are filled). */
void mem_set(void *dst, int val, int n)
{
    uint8_t *p = (uint8_t *)dst;
    for (int i = 0; i < n; i++)
        p[i] = (uint8_t)val;
}
