#include <stdint.h>

#include "bim.h"

/* CC13x2/CC26x2 GPIO controller — `GPIO_BASE = 0x40022000`. The
 * `DOUTSET31_0` register at offset 0x90 is a set-only alias for the
 * 32 DIO output bits: writing a 1 in bit N drives DIOn high without
 * disturbing the other pins (the cleared bits are no-ops). The
 * VanMoof BLE PCB wires DIO2 to a status LED that the BIM lights
 * when the boot-decision state machine has nowhere safe to jump.
 *
 * Identified by base+offset only — the exact pin assignment isn't
 * yet cross-referenced against the CC2642R1F TRM's pad mux. The
 * write is unconditional and stateless, so we don't bother probing
 * IOC's pin-config registers first; the OEM doesn't either. */
#define GPIO_DOUTSET31_0  (*(volatile uint32_t *)0x40022090u)
#define DIO_PANIC_LED     (1u << 2)

/* CC13x2/CC26x2 ROM dispatch slot. The literal `0x100001B8` lives
 * in the boot ROM (which CC2642R1F maps at `0x10000000..0x1003FFFF`)
 * and resolves to a pointer-to-pointer-to-table: the slot itself
 * holds the address of one of TI's ROM API jump tables, and that
 * table is an array of function pointers indexed 0..N. The
 * argument values used here (`4` and `0x500`) and the index choices
 * (5, 13, 7) don't cleanly match any of the documented
 * `ROM_API_*_TABLE` layouts in the SimpleLink CC13x2/CC26x2 SDK
 * 3.40.00.02 `rom.h` — likely a TI-internal "AON" / "low-level"
 * vector or a SDK-3.40-specific ordering. Decoded mechanically;
 * the exact API names go in once the table identity is pinned. */
#define BIM_ROM_DISPATCH  (*(const uintptr_t *const *)0x100001B8u)

/* CC26x2 PRCM register at `PRCM_BASE + 0x28` — the GPIO peripheral
 * clock-gate register `PRCM_GPIOCLKGR` (bit 0 = clock enable in
 * RUN mode). The OEM writes through two aliases of the same word:
 * `0x60082028` for the (write-through, unbuffered) update and
 * `0x40082028` for the (buffered, readable) status poll. Bit 1
 * of the readable alias is the AHB-side acknowledge that the
 * clock-gate change has propagated — TI's `PRCMLoadSet` /
 * `PRCMLoadGet` pair does the same handshake. The OEM inlines it
 * here rather than calling driverlib. */
#define PRCM_GPIOCLKGR_SET  (*(volatile uint32_t *)0x60082028u)
#define PRCM_GPIOCLKGR_GET  (*(volatile uint32_t *)0x40082028u)
#define PRCM_LOAD_DONE_Msk  0x2u

/* Bit-band alias of bit 2 of `GPIO_DOE31_0` (`GPIO_BASE + 0xD0` =
 * `0x400220D0`). Writing 1 here enables DIO2 as an output without
 * a read-modify-write on the rest of the 32-bit word. Paired with
 * the `DOUTSET31_0` write below to light the panic LED on DIO2.
 *
 * The bit-band math: `0x42000000 + (0x220D0 * 32) + (2 * 4) =
 * 0x42441A08`. */
#define GPIO_DOE_DIO2_BB    (*(volatile uint32_t *)0x42441A08u)

void bim_panic_prep(void)
{
    /* Step 1 — quiesce a peripheral via ROM dispatch[5] with arg 4. */
    ((void (*)(uint32_t))BIM_ROM_DISPATCH[5])(4u);

    /* Step 2 — poll ROM dispatch[13] with arg 4 until it returns 1.
     * The same peripheral as step 1; this is the "ready" handshake. */
    while (((uint32_t (*)(uint32_t))BIM_ROM_DISPATCH[13])(4u) != 1u) {
        /* spin */
    }

    /* Step 3 — one more ROM call, dispatch[7] with arg 0x500. The
     * argument and ignored return suggest a fire-and-forget set or
     * delay primitive. */
    ((void (*)(uint32_t))BIM_ROM_DISPATCH[7])(0x500u);

    /* Step 4 — open the GPIO clock gate and wait for the load to
     * complete (LOAD_DONE = PRCM_GPIOCLKGR bit 1 in the buffered
     * alias). After this point GPIO MMIO is safe to touch. */
    PRCM_GPIOCLKGR_SET = 1u;
    while ((PRCM_GPIOCLKGR_GET & PRCM_LOAD_DONE_Msk) == 0u) {
        /* spin */
    }

    /* Step 5 — configure DIO2 as an output (it's an input by reset
     * default). The companion `bim_panic_indicate` call from
     * bim_dispatch drives it high. */
    GPIO_DOE_DIO2_BB = 1u;
}

void bim_panic_indicate(void)
{
    GPIO_DOUTSET31_0 = DIO_PANIC_LED;
}
