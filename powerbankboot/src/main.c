/* main.c — firmware entry (OEM 0x0800070C).
 *
 * This is ST's X-CUBE-STL (IEC-60730 Class-B) `main` template with the VanMoof
 * bootloader spliced in: boot_main() is invoked right after the trace console
 * comes up. On a normal boot boot_main() never returns — it either jumps to the
 * validated application or stays in the download server loop — so the elaborate
 * self-test body below is effectively dormant (it is the unmodified STL scaffold
 * the OEM kept in place). It is reproduced here to document the structure; every
 * stl_* leaf is recognised X-CUBE-STL and stays vendor-stock.
 *
 * The `cfc` (control-flow counter) inc/dec around each step is the STL program-
 * flow signature; stl_checkpoint_verify() confirms it reached the expected value
 * (0x26 at checkpoint 1, 0x7C at checkpoint 2) with its complement intact. Each
 * test traces an "OK"/"NG" line and FailSafes on error. The full startup suite,
 * in order, is: CPU registers -> IWDG/reset-cause -> CRC unit -> flash CRC ->
 * [CP1] -> RAM march -> clock/oscillator -> [CP2].
 */
#include "powerbankboot.h"

/* X-CUBE-STL leaves (vendor-stock), named from their disassembly + trace strings. */
extern void stl_log_init(void);           /* 0x08005870  trace/printf bring-up      */
extern void stl_log_init2(void);          /* 0x080059FC                              */
extern void hal_init(void);               /* 0x08002B88  HAL_Init (prefetch+tick+msp)*/
extern void stl_startup(void);            /* 0x080058F8                              */
extern void stl_clock_uart_startup(void); /* 0x08000A80  SystemClock + trace UART    */
extern int  stl_cpu_test(void);           /* 0x080028B4  CPU regs + PSP/MSP test     */
extern void stl_iwdg_test(void);          /* 0x08000D10  reset-cause + IWDG test     */
extern void stl_crc_init(void);           /* 0x08000988  HAL_CRC_Init                */
extern void stl_flash_crc_test(void);     /* 0x08000E38  flash CRC vs golden         */
extern unsigned char stl_checkpoint_verify(unsigned expected); /* 0x08000940         */
extern int  stl_ram_march_test(void *start, void *end, int pattern); /* 0x08002AB0   */
extern void stl_uart_handle_reset(void);  /* 0x08000BCC  restore UART handle         */
extern void stl_iwdg_config(int test_mode);/* 0x08000C98  HAL_IWDG_Init              */
extern void stl_clock_test(void);         /* 0x08000E84  oscillator/PLL test         */

/* STL trace strings (vendor rodata @ 0x08006920+). */
extern const char STL_INIT[], CPU_OK[], CPU_NG[], CP1_OK[], CP1_NG[],
                  RAM_OK[], RAM_NG[], CP2_OK[], CP2_NG[], STL_DONE[];

static uint32_t cfc;        /* control-flow counter         (OEM 0x20000000) */
static uint32_t cfc_inv;    /* its running complement       (OEM 0x20000024) */
static uint32_t s_march_pattern[4];  /* RAM-test pattern buffer (OEM 0x200000E8) */

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
    stl_clock_uart_startup();

    cfc = 0; cfc_inv = cfc;
    dbg_printf(STL_INIT);                                       /* "--> STL Init"   */

    cfc = 0; cfc_inv = ~cfc; cfc += 2;
    if (stl_cpu_test() == 1) { cfc_inv -= 2; dbg_printf(CPU_OK); }   /* "CPU Test OK"*/
    else                     { dbg_printf(CPU_NG); stl_failsafe(); } /* "CPU Test NG"*/

    cfc += 0x05; stl_iwdg_test();      cfc_inv -= 0x05;   /* reset-cause + IWDG test */
    cfc += 0x0B; stl_crc_init();       cfc_inv -= 0x0B;   /* HAL_CRC_Init            */
    cfc += 0x11; stl_flash_crc_test(); cfc_inv -= 0x11;   /* flash CRC -> "Flash OK/NG" */

    if (stl_checkpoint_verify(0x26) == 0) { dbg_printf(CP1_NG); stl_failsafe(); }
    else                                  dbg_printf(CP1_OK);        /* "CP 1 OK/NG" */

    irq_disable();
    if (stl_ram_march_test((void *)0x20000000, (void *)0x20007FFF, 0) != 1) {
        irq_enable(); init_data_bss(); stl_uart_handle_reset();
        dbg_printf(RAM_NG); stl_failsafe();                          /* "RAM NG"     */
    }
    irq_enable();
    init_data_bss();                      /* the march test is destructive — reload */
    stl_uart_handle_reset();
    stl_iwdg_config(0);
    dbg_printf(RAM_OK);                                              /* "RAM OK"     */

    cfc = 0; cfc_inv = ~cfc;
    cfc += 0x13; cfc_inv -= 0x13;
    cfc += 0x17; stl_clock_test(); cfc_inv -= 0x17;  /* -> "Clock/LSI/LSE/HSE/PLL NG"*/

    cfc += 0x35;
    s_march_pattern[0] = 0xEEEEEEEEu;
    s_march_pattern[1] = 0xCCCCCCCCu;
    s_march_pattern[2] = 0xBBBBBBBBu;
    s_march_pattern[3] = 0xDDDDDDDDu;
    cfc_inv -= 0x35;

    if (stl_checkpoint_verify(0x7C) == 0) { dbg_printf(CP2_NG); stl_failsafe(); }
    dbg_printf(CP2_OK);                                             /* "CP 2 OK"     */
    dbg_printf(STL_DONE);                                           /* "--> STL Done"*/

    /* trailing bootloader entry (mirror of the splice above) */
    init_data_bss();
    stl_log_init();
    stl_log_init2();
    boot_main();
    return 0;
}
