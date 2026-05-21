/* cc2642r1.h — TI CC2642R1F peripheral register addresses.
 *
 * Minimal: only the registers the decomp has actually observed in use.
 * Extend as more functions land.
 */

#ifndef BLEWARE_CC2642R1_H
#define BLEWARE_CC2642R1_H

#include <stdint.h>

/* SCB (Cortex-M4 System Control Block). */
#define SCB_CPACR           (*(volatile uint32_t *)0xE000ED88u)
#define SCB_AIRCR           (*(volatile uint32_t *)0xE000ED0Cu)

/* FCFG1 — Factory Configuration. */
#define FCFG1_FCFG1_REVISION  (*(volatile uint32_t *)0x5000131Cu)
#define FCFG1_MISC_TRIM       (*(volatile uint32_t *)0x5000140Cu)

/* FLASH controller bit-banded acks (FLASH+0x24, alias base 0x42600000). */
#define FLASH_BB_FSM_ACK    (*(volatile uint32_t *)0x42600484u) /* bit 1 */
#define FLASH_BB_BIT5_494   (*(volatile uint32_t *)0x42600494u) /* bit 5 */

/* ROM HAPI sub-table — `*(0x100001F0)` is a pointer-to-table-of-pointers.
 * Slot [18] is the cold-reset hook `SetupTrimDevice` invokes. */
#define ROM_HAPI_TABLE_PTR  (*(void *const *volatile)0x100001F0u)

/* Bit-banded AON / system gates whose role isn't yet pinned to a TRM name. */
#define BB_AON_GATE_43280180  (*(volatile uint32_t *)0x43280180u)
#define BB_AON_READ_43200580  (*(volatile uint32_t *)0x43200580u)

/* AON_PMCTL + FLASH config registers. */
#define AON_REG_4008218C    (*(volatile uint32_t *)0x4008218Cu)
#define FLASH_REG_40032048  (*(volatile uint32_t *)0x40032048u)
#define AON_REG_40090028    (*(volatile uint32_t *)0x40090028u)

/* VIMS_STAT bit 3 (mode-change in progress), bit-banded. */
#define VIMS_BB_MODE_CHANGING  (*(volatile uint32_t *)0x4268000Cu)

#endif /* BLEWARE_CC2642R1_H */
