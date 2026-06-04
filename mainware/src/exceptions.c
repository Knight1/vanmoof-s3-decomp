#include <stdint.h>

#include "cortex_m_scb.h"
#include "exceptions.h"
#include "log.h"
#include "scheduler.h"
#include "systick.h"

/* Cortex-M4 system-exception handlers. See exceptions.h for the behaviour
 * summary. Each handler logs its own name through g_log_func; the spinning
 * ones replicate the OEM's `b .` tail. Faithful to mainware 1.07.06's
 * CubeF4-style startup (handlers at 0x0803C974+, 20 bytes apart). */

void NMI_Handler(void)
{
    g_log_func("NMI_Handler\r\n");
}

/* HardFault: select the stack the faulting context used (EXC_RETURN bit 2 in
 * LR — 0 = MSP, 1 = PSP), load it into r0, and tail-branch to fault_dump.
 * Naked so the prologue can't clobber LR before we test it — matches the OEM
 * five-instruction sequence byte-for-byte. */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile (
        "tst   lr, #4          \n"
        "ite   eq              \n"
        "mrseq r0, msp         \n"
        "mrsne r0, psp         \n"
        "b.w   fault_dump      \n"
    );
}

void MemManage_Handler(void)
{
    g_log_func("MemManage_Handler\r\n");
    for (;;) {
    }
}

void BusFault_Handler(void)
{
    g_log_func("BusFault_Handler\r\n");
    for (;;) {
    }
}

void UsageFault_Handler(void)
{
    g_log_func("UsageFault_Handler\r\n");
    for (;;) {
    }
}

void SVC_Handler(void)
{
    g_log_func("SVC_Handler\r\n");
}

void DebugMon_Handler(void)
{
    g_log_func("DebugMon_Handler\r\n");
}

void PendSV_Handler(void)
{
    g_log_func("PendSV_Handler\r\n");
}

/* SysTick: the Muco tick. scheduler_tick() services the one-shot timers,
 * then systick_tick() advances the free-running counter. */
void SysTick_Handler(void)
{
    scheduler_tick();
    systick_tick();
}

/* HardFault frame dumper (OEM 0x0803CB6C). `frame` is the hardware-stacked
 * exception frame; the labels and the "MMAR"/"PSR" spellings are the OEM's. */
_Noreturn void fault_dump(uint32_t *frame)
{
    g_log_func("[Hard fault handler]\r\n");
    g_log_func("R0 = %x\r\n",  frame[0]);
    g_log_func("R1 = %x\r\n",  frame[1]);
    g_log_func("R2 = %x\r\n",  frame[2]);
    g_log_func("R3 = %x\r\n",  frame[3]);
    g_log_func("R12 = %x\r\n", frame[4]);
    g_log_func("LR = %x\r\n",  frame[5]);
    g_log_func("PC = %x\r\n",  frame[6]);
    g_log_func("PSR = %x\r\n", frame[7]);
    g_log_func("MMAR = %x\r\n", SCB_MMFAR);
    g_log_func("BFAR = %x\r\n", SCB_BFAR);
    g_log_func("CFSR = %x\r\n", SCB_CFSR);
    g_log_func("HFSR = %x\r\n", SCB_HFSR);
    g_log_func("DFSR = %x\r\n", SCB_DFSR);
    g_log_func("AFSR = %x\r\n", SCB_AFSR);
    for (;;) {
    }
}
