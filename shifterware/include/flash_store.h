#ifndef SHIFTER_FLASH_STORE_H
#define SHIFTER_FLASH_STORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FLASH_PAGE_SIZE      1024u

/* OEM-confirmed (void where the OEM provides no return; success
 * checked by reading FLASH->SR otherwise). */
void flash_unlock(void);                          /* @ 0x0800471C */
void flash_lock(void);                            /* @ 0x08004728 */
void flash_clear_status(uint32_t bits);           /* @ 0x08004C4A */
void flash_erase_page(uint32_t page_addr);        /* @ 0x08003812 */
void flash_erase_pages(uint32_t base, int n_pages); /* @ 0x08003832 */

/* Lower-level helpers. Return codes: 1=busy, 2=PGERR, 3=WRPRTERR,
 * 4=ready, 5=timeout (wait helper only). */
int  flash_get_status(void);                      /* @ 0x08004736 */
int  flash_wait_status(int timeout);              /* @ 0x08004786 */
int  flash_do_page_erase(uint32_t page_addr);     /* @ 0x080047B2 */

/* Speculative. */
bool flash_program_halfword(uint32_t addr, uint16_t value);
bool flash_program_block(uint32_t addr, const void *data, size_t len_bytes);

#endif /* SHIFTER_FLASH_STORE_H */
