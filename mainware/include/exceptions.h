#ifndef MAINWARE_EXCEPTIONS_H
#define MAINWARE_EXCEPTIONS_H

#include <stdint.h>

/* Cortex-M4 system-exception vector handlers (flash 0x0803C974..0x0803CA1F,
 * each on a 20-byte boundary) plus the HardFault frame dumper. Mainware is
 * built from ST's CubeF4 startup template, so every exception has its own
 * named handler rather than the shared trap stub seen on the Cortex-M0 wares.
 *
 * Two behaviours among the simple handlers:
 *   - NMI / SVC / DebugMon / PendSV  — log the handler name and return.
 *   - MemManage / BusFault / UsageFault — log and spin (`b .`), since a
 *     fault escalated here is unrecoverable; the IWDG reboots the board.
 *   - HardFault — capture the active (MSP/PSP) exception frame and tail-call
 *     fault_dump(), which never returns.
 *   - SysTick — Muco tick: scheduler_tick() then systick_tick().
 */

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVC_Handler(void);
void DebugMon_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);

/* HardFault helper (OEM 0x0803CB6C). `frame` points at the 8-word hardware
 * exception stack frame {R0,R1,R2,R3,R12,LR,PC,xPSR}. Dumps the frame and the
 * SCB fault registers through g_log_func, then spins. */
_Noreturn void fault_dump(uint32_t *frame);

#endif
