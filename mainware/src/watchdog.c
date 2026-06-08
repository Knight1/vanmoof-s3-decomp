#include <stdint.h>

#include "panic.h"
#include "watchdog.h"

/* Descriptor-driven 32-bit register write (OEM wdg_reg_write_from_desc,
 * 0x08026DB4): store desc[3] (word index 3 = byte +0xC) to the MMIO address in
 * desc[0]. The runtime descriptor @ 0x20009728 (built by watchdog_init) makes the
 * sole use a WWDG counter reload: desc[0] = WWDG_CR (0x40002C00), desc[3] = 0x7F. */
uint32_t wdg_reg_write_from_desc(uint32_t *desc)
{
    *(volatile uint32_t *)desc[0] = desc[3];
    return 0;
}

/* Refresh the window watchdog (OEM watchdog_kick, 0x080314D8): a thunk applying
 * the descriptor at 0x20009728 — reloads WWDG_CR with 0x7F. The OEM tail-calls
 * the worker; callers ignore the (always-0) return. */
void watchdog_kick(void)
{
    (void)wdg_reg_write_from_desc((uint32_t *)0x20009728u);
}

/* Enable the WWDG peripheral clock if the descriptor targets WWDG (OEM
 * wwdg_clk_enable, 0x08031474 — the HAL_WWDG_MspInit equivalent): set WWDGEN
 * (bit 11) in RCC_APB1ENR (RCC 0x40023800 + 0x40). */
void wwdg_clk_enable(uint32_t *desc)
{
    if (desc[0] != 0x40002C00u) {        /* only for the WWDG instance */
        return;
    }
    *(volatile uint32_t *)(0x40023800u + 0x40) |= 0x800u;   /* RCC_APB1ENR.WWDGEN */
}

/* Program the WWDG hardware from the descriptor (OEM wwdg_hw_init, 0x08026D8A —
 * HAL_WWDG_Init-like): enable the clock, then write WWDG_CR = T | WDGA(0x80) and
 * WWDG_CFR = window | prescaler. desc[0]=WWDG_CR addr, +4 = WWDG_CFR. Returns 0
 * on success, 1 if desc is NULL. */
int wwdg_hw_init(uint32_t *desc)
{
    if (desc == 0) {
        return 1;
    }
    wwdg_clk_enable(desc);
    *(volatile uint32_t *)(desc[0])       = desc[3] | 0x80u;            /* WWDG_CR  = T | WDGA */
    *(volatile uint32_t *)(desc[0] + 4)   = desc[4] | desc[1] | desc[2]; /* WWDG_CFR = W | WDGTB */
    return 0;
}

/* Build the WWDG descriptor and bring up the window watchdog (OEM watchdog_init,
 * 0x08031444). Descriptor @ 0x20009728 = { WWDG_CR (0x40002C00), 0x180, 0x7F,
 * 0x7F, 0 }; then program the hardware — on failure, the fatal Error_Handler. */
void watchdog_init(void)
{
    uint32_t *desc = (uint32_t *)0x20009728u;

    desc[0] = 0x40002C00u;   /* WWDG_CR address (the reg watchdog_kick reloads) */
    desc[1] = 0x180u;        /* WWDG_CFR WDGTB prescaler bits */
    desc[2] = 0x7Fu;         /* WWDG_CFR window value W */
    desc[3] = 0x7Fu;         /* WWDG_CR counter T (reload value, 0x7F) */
    desc[4] = 0u;

    if (wwdg_hw_init(desc) != 0) {
        Error_Handler();
    }
}

/* Disable the WWDG APB1 peripheral clock (OEM wwdg_apb_clk_disable, 0x080314A4):
 * clear RCC_APB1ENR.WWDGEN (bit 11). A bare read-modify-write with no read-back
 * (unlike wwdg_clk_enable). Used by the shipping-mode / stop-mode powerdown paths. */
void wwdg_apb_clk_disable(void)
{
    *(volatile uint32_t *)(0x40023800u + 0x40u) &= ~0x800u;   /* RCC_APB1ENR.WWDGEN */
}
