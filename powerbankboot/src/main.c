/* main.c — firmware entry (OEM 0x0800070C).
 *
 * This is ST's X-CUBE-STL (IEC-60730 Class-B) `main` template with the VanMoof
 * bootloader spliced in: boot_main() is invoked right after the trace console
 * comes up. On a normal boot boot_main() never returns — it either jumps to the
 * validated application or stays in the download server loop — so the elaborate
 * self-test body below is effectively dormant (it is the unmodified STL scaffold
 * the OEM kept in place). It is reproduced here only to document the structure;
 * every FUN_-named leaf is recognised X-CUBE-STL and stays vendor-stock.
 *
 * The `cfc` (control-flow counter) inc/dec around each test is the STL signature
 * technique: a value + complement pair that must net to zero, else the flow has
 * been corrupted and FailSafe trips.
 */
#include "powerbankboot.h"

/* X-CUBE-STL leaves (vendor-stock). */
extern void stl_log_init(void);        /* FUN_08005870 */
extern void stl_log_init2(void);       /* FUN_080059FC */
extern void stl_startup(void);         /* FUN_080058F8 */
extern void stl_cpu_test_pre(void);    /* FUN_08000A80 */
extern int  stl_cpu_test(void);        /* FUN_080028B4 */
extern int  stl_ram_test(uint32_t base, uint32_t pattern, int mode); /* FUN_08002AB0 */
extern void stl_test_step_d10(void);   /* FUN_08000D10 */
extern void stl_test_step_9d0(void);   /* FUN_080009D0 */
extern void stl_test_step_988(void);   /* FUN_08000988 */
extern void stl_test_step_e38(void);   /* FUN_08000E38 */
extern void stl_test_step_e84(void);   /* FUN_08000E84 */
extern int  stl_crc_test(int sel);     /* FUN_08000940 (0x26 startup, 0x7C full) */
extern void stl_ram_test_post(void);   /* FUN_08000BCC */
extern void stl_ram_test_run(int mode);/* FUN_08000C98 */
extern void hal_init(void);            /* FUN_08002B88 = HAL_Init (prefetch+tick+msp) */

/* trace strings printed by the STL phases (rodata in the OEM). */
extern const char STL_INIT[], CPU_OK[], CPU_NG[], CLK_OK[], CLK_NG[],
                  FLASH_OK[], FLASH_NG[], STL_DONE[], RAM_NG[];

static uint32_t cfc;        /* control-flow counter (0x20000???)        */
static uint32_t cfc_inv;    /* its running complement                   */

static inline void irq_disable(void) { __asm volatile ("cpsid i" ::: "memory"); }
static inline void irq_enable(void)  { __asm volatile ("cpsie i" ::: "memory"); }

int main(void)
{
    init_data_bss();
    stl_log_init();
    stl_log_init2();

    /* ---- VanMoof bootloader: runs first, normally never returns ---- */
    boot_main();

    /* ================= X-CUBE-STL template (dormant) ================= */
    hal_init();
    stl_startup();
    stl_cpu_test_pre();

    cfc = 0; cfc_inv = cfc;
    dbg_printf(STL_INIT);

    cfc = 0; cfc_inv = ~cfc; cfc += 2;
    if (stl_cpu_test() == 1) { cfc_inv -= 2; dbg_printf(CPU_OK); }
    else                     { dbg_printf(CPU_NG); stl_failsafe(); }

    cfc += 5;  stl_test_step_d10(); cfc_inv -= 5;
    cfc += 11; stl_test_step_988(); cfc_inv -= 11;
    cfc += 17; stl_test_step_e38(); cfc_inv -= 17;
    if (stl_crc_test(0x26) == 0)  { dbg_printf(CLK_NG); stl_failsafe(); }
    else                          dbg_printf(CLK_OK);

    irq_disable();
    if (stl_ram_test(0x20000000, 0x20000800u, 0) != 1) {
        irq_enable(); init_data_bss(); stl_ram_test_post();
        dbg_printf(RAM_NG); stl_failsafe();
    }
    irq_enable();
    init_data_bss(); stl_ram_test_post(); stl_ram_test_run(0);
    dbg_printf(STL_DONE);

    cfc = 0; cfc_inv = ~cfc;
    cfc += 0x17; stl_test_step_e84(); cfc_inv -= 0x17;
    cfc += 0x35; stl_test_step_9d0(); cfc_inv -= 0x35;
    if (stl_crc_test(0x7C) == 0) { dbg_printf(FLASH_NG); stl_failsafe(); }
    dbg_printf(FLASH_OK);

    /* trailing bootloader entry (mirror of the splice above) */
    init_data_bss();
    stl_log_init();
    stl_log_init2();
    boot_main();
    return 0;
}
