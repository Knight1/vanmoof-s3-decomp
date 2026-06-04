#include <stdint.h>

#include "flash.h"
#include "log.h"

/* STM32F4 FLASH status register (RM0430 §3.7.5). flash_write clears the error
 * flags (write-1-to-clear) before programming. The OEM literal is 0x40023C00
 * (FLASH base) + 0x0C. */
#define FLASH_SR (*(volatile uint32_t *)0x40023C0Cu)

/* --- HAL / runtime externs ---
 * flash_program        0x08027BE0  width-tagged program: op 0/1/2/3 =
 *                                   byte/half/word/dword (CubeF4-style, locked).
 * flash_erase_sector   0x080235B4  HAL sector erase (EraseInit, &sector_error).
 * flash_addr_to_sector 0x0803CE14  flash address -> sector index.
 * flash_erase_prep     0x0803CF1C  pre-erase housekeeping.
 * lock_acquire         0x08027B80  flash/resource lock (tick timeout).
 * watchdog_kick        0x080314D8  refresh the watchdog (flash ops are slow). */
extern int  flash_program(int op, uint32_t addr, uint32_t value, int unused);
extern int  flash_erase_sector(void *erase_init, uint32_t *sector_error);
extern int  flash_addr_to_sector(int addr);
extern void flash_erase_prep(void);
extern int  lock_acquire(int timeout_ticks);
extern void watchdog_kick(void);

/* CubeF4 FLASH_EraseInitTypeDef (sector mode). */
struct flash_erase_init {
    uint32_t type_erase;     /* +0x00  0 = FLASH_TYPEERASE_SECTORS */
    uint32_t banks;          /* +0x04  (ignored for sector erase) */
    uint32_t sector;         /* +0x08 */
    uint32_t nb_sectors;     /* +0x0C */
    uint32_t voltage_range;  /* +0x10  2 = 2.7-3.6 V (word programming) */
};

static inline void irq_disable(void) { __asm volatile ("cpsid i" ::: "memory"); }
static inline void irq_enable(void)  { __asm volatile ("cpsie i" ::: "memory"); }

int flash_erase(int addr, int len)
{
    flash_erase_prep();

    int first = flash_addr_to_sector(addr);
    int last  = flash_addr_to_sector(addr + len);

    for (uint32_t i = 0; i < (uint32_t)(last - first); i++) {
        struct flash_erase_init ei;
        uint32_t sector_error;
        int rc;

        watchdog_kick();
        ei.type_erase    = 0;
        ei.banks         = 0;   /* OEM leaves this field unset; harmless for sectors */
        ei.sector        = (uint32_t)(first + (int)i);
        ei.nb_sectors    = 1;
        ei.voltage_range = 2;

        rc = flash_erase_sector(&ei, &sector_error);
        if (rc != 0) {
            g_log_func("Flash erase error %d\r\n", sector_error);  /* OEM str 0x0803CF90 */
            return rc;
        }
    }
    return 0;
}

int flash_write(uint32_t addr, const uint32_t *data, int len)
{
    uint32_t end = addr + (uint32_t)len;

    irq_disable();
    lock_acquire(0xFFFF);
    FLASH_SR = 0xF3;   /* clear PGSERR/PGPERR/PGAERR/WRPERR/EOP */

    while (addr < end) {
        watchdog_kick();
        int rc = flash_program(2, addr, *data, 0);   /* program one 32-bit word */
        if (rc != 0) {
            /* Faithful OEM quirk: the error path returns with IRQs still
             * masked (no re-enable). */
            return rc;
        }
        addr += 4;
        data++;
    }

    irq_enable();
    return 0;
}
