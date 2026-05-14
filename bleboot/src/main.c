#include <stdint.h>

#include "main.h"

/* The BIM dispatcher — undecoded yet. Defined elsewhere (currently
 * inside the OEM image as FUN_00056F2A) and called from main()
 * after the hardware-id stash below. */
extern void bim_dispatch(void);

/* MMIO config word; low 4 bits are a hardware-revision / package
 * code that the BIM dispatcher consults to decide which flash
 * region holds the application slot. Address verified from the
 * literal pool of FUN_00057000 at file offset 0x1018 of
 * bleboot_1.0.0.bin; the exact register is not yet identified in
 * the CC2642R1F TRM (sits in the 0x40030000..0x40034000 band,
 * between the FLASH controller and VIMS). */
#define BIM_HW_ID_REG   (*(volatile uint32_t *)0x40032430u)

/* SRAM global that caches the (hw_id_low_4 << 10) value so the
 * BIM dispatcher and downstream helpers don't have to re-read the
 * MMIO every time. The OEM places this at SRAM 0x20000400 — we
 * leave the exact placement to the linker for now; a future SRAM
 * layout pass will pin it to match. */
volatile uint32_t g_hw_id_cached;

int main(void)
{
    g_hw_id_cached = (BIM_HW_ID_REG & 0xFu) << 10;
    bim_dispatch();
    return 0;
}
