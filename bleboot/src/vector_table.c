/* CC2642R1F Cortex-M4F vector table — 54 entries (216 B) at flash
 * base 0x00056000. Layout matches the OEM bleboot_1.0.0.bin:
 *   [0]      Initial SP = 0x20014000 (top of 80 KB SRAM)
 *   [1]      Reset_Handler
 *   [2]      NMI_Handler
 *   [3]      HardFault_Handler
 *   [4..6]   MemManage / BusFault / UsageFault -> Default_Handler
 *   [7..10]  RESERVED (slots filled with 0)
 *   [11]     SVCall -> Default_Handler
 *   [12]     DebugMon -> Default_Handler
 *   [13]     RESERVED (0)
 *   [14]     PendSV -> Default_Handler
 *   [15]     SysTick -> Default_Handler
 *   [16..53] IRQ vectors -> Default_Handler (38 IRQs, CC2642R1F NVIC)
 *
 * The image ends the vector table at VT[53] (offset 0xD4) because the
 * BIM never enables any of the CC2642R1F IRQ sources — it's an
 * unattended boot image whose only path is reset -> trim -> dispatch
 * -> launch. The application image takes over the full NVIC after
 * bim_launch_image, so a short VT is sufficient. */

#include <stdint.h>

#include "bim.h"
#include "exception.h"

extern void Reset_Handler(void);

extern uint32_t _estack;   /* defined in linker_cc2642r1.ld */

typedef void (*vector_t)(void);

__attribute__((section(".isr_vector"), used))
const vector_t g_pfnVectors[54] = {
    /* Cortex-M loads the initial SP from VT[0] as a 32-bit value; cast
     * through uintptr_t to silence the C-pedantic data-to-fn-ptr
     * warning without changing the emitted bytes. */
    (vector_t)(uintptr_t)&_estack,  /*  0  Initial SP            */
    Reset_Handler,             /*  1  Reset                     */
    NMI_Handler,               /*  2  NMI                       */
    HardFault_Handler,         /*  3  HardFault                 */
    Default_Handler,           /*  4  MemManage                 */
    Default_Handler,           /*  5  BusFault                  */
    Default_Handler,           /*  6  UsageFault                */
    0, 0, 0, 0,                /*  7- 10  reserved              */
    Default_Handler,           /* 11  SVCall                    */
    Default_Handler,           /* 12  DebugMon                  */
    0,                         /* 13  reserved                  */
    Default_Handler,           /* 14  PendSV                    */
    Default_Handler,           /* 15  SysTick                   */
    Default_Handler,           /* 16  AON_GPIO_EDGE             */
    Default_Handler,           /* 17  I2C                       */
    Default_Handler,           /* 18  RFC_CPE1                  */
    Default_Handler,           /* 19  AON_SPIS                  */
    Default_Handler,           /* 20  AON_RTC                   */
    Default_Handler,           /* 21  UART0                     */
    Default_Handler,           /* 22  AUX_SWEV0                 */
    Default_Handler,           /* 23  SSI0                      */
    Default_Handler,           /* 24  SSI1                      */
    Default_Handler,           /* 25  RFC_CPE0                  */
    Default_Handler,           /* 26  RFC_HW                    */
    Default_Handler,           /* 27  RFC_CMD_ACK               */
    Default_Handler,           /* 28  I2S                       */
    Default_Handler,           /* 29  AUX_SWEV1                 */
    Default_Handler,           /* 30  WATCHDOG                  */
    Default_Handler,           /* 31  GPT0A                     */
    Default_Handler,           /* 32  GPT0B                     */
    Default_Handler,           /* 33  GPT1A                     */
    Default_Handler,           /* 34  GPT1B                     */
    Default_Handler,           /* 35  GPT2A                     */
    Default_Handler,           /* 36  GPT2B                     */
    Default_Handler,           /* 37  GPT3A                     */
    Default_Handler,           /* 38  GPT3B                     */
    Default_Handler,           /* 39  CRYPTO                    */
    Default_Handler,           /* 40  UDMA SW                   */
    Default_Handler,           /* 41  UDMA ERR                  */
    Default_Handler,           /* 42  FLASH                     */
    Default_Handler,           /* 43  SWEV0                     */
    Default_Handler,           /* 44  AUX_COMB                  */
    Default_Handler,           /* 45  AON_PROG                  */
    Default_Handler,           /* 46  DYNAMIC_PROG              */
    Default_Handler,           /* 47  AUX_COMPA                 */
    Default_Handler,           /* 48  AUX_ADC                   */
    Default_Handler,           /* 49  TRNG                      */
    Default_Handler,           /* 50  OSC_COMB                  */
    Default_Handler,           /* 51  AUX_TIMER2                */
    Default_Handler,           /* 52  UART1                     */
    Default_Handler,           /* 53  BATMON                    */
};
