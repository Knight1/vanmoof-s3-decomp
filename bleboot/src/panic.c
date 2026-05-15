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

void bim_panic_indicate(void)
{
    GPIO_DOUTSET31_0 = DIO_PANIC_LED;
}
