/* hal_stubs.S — weak fallbacks for the MindMotion HAL / CMSIS leaves
 *
 * The shifterboot decomp identifies these as vendor-stock (see
 * `docs/progress.md`'s vendor-stock table). Each lives at a specific
 * OEM address in the original binary; in our build they're declared
 * `extern` from the C sources that depend on them, with the
 * expectation that we eventually link against the real MindMotion BSP.
 *
 * Until that happens, this file provides minimal `__attribute__((weak))`
 * fallbacks so the build is link-complete. Each stub is marked weak
 * so the real BSP body overrides it transparently; until then, the
 * stubs implement the most conservative behaviour we can while still
 * allowing the build to link:
 *
 *   - "is-an-event-pending?" predicates return 0  (no event)
 *   - "is-ready?" predicates return 1             (always ready)
 *   - configure/enable/init helpers are no-ops
 *   - NVIC_SystemReset is implemented inline (SCB->AIRCR write) since
 *     it's small and a no-op fallback would be observably wrong
 *
 * If any of these fires at runtime, the chip's behaviour will diverge
 * from the OEM. Vendor the MindMotion BSP and remove this file once
 * the real bodies are available.
 */

    .syntax unified
    .cpu    cortex-m0
    .thumb

/* ===== Predicates: return 0 (no event pending) ===================== */

    .section .text.USART_GetITStatus, "ax", %progbits
    .weak    USART_GetITStatus
    .type    USART_GetITStatus, %function
    .thumb_func
USART_GetITStatus:
    movs    r0, #0
    bx      lr
    .size   USART_GetITStatus, .-USART_GetITStatus

/* ===== Predicates: return 1 (always ready) ========================= */

    .section .text.USART_GetFlagStatus, "ax", %progbits
    .weak    USART_GetFlagStatus
    .type    USART_GetFlagStatus, %function
    .thumb_func
USART_GetFlagStatus:
    movs    r0, #1
    bx      lr
    .size   USART_GetFlagStatus, .-USART_GetFlagStatus

/* ===== Data accessors: USART_ReceiveData returns 0 ================= */

    .section .text.USART_ReceiveData, "ax", %progbits
    .weak    USART_ReceiveData
    .type    USART_ReceiveData, %function
    .thumb_func
USART_ReceiveData:
    movs    r0, #0
    bx      lr
    .size   USART_ReceiveData, .-USART_ReceiveData

/* ===== Void HAL helpers: no-op (just `bx lr`) ====================== */

#define WEAK_NOOP(name) \
    .section .text.name, "ax", %progbits ; \
    .weak    name ; \
    .type    name, %function ; \
    .thumb_func ; \
name: ; \
    bx      lr ; \
    .size   name, .-name

    WEAK_NOOP(USART_SendData)
    WEAK_NOOP(USART_ClearITPendingBit)
    WEAK_NOOP(USART_Init)
    WEAK_NOOP(USART_Cmd)
    WEAK_NOOP(USART_ITConfig)
    WEAK_NOOP(NVIC_Init)
    WEAK_NOOP(NVIC_SetPriority)
    WEAK_NOOP(RCC_AHBPeriphClockCmd)
    WEAK_NOOP(RCC_APB2PeriphClockCmd)
    WEAK_NOOP(GPIO_Init)
    WEAK_NOOP(GPIO_PinAFConfig)
    WEAK_NOOP(CRC_ResetDR)

/* ===== NVIC_SystemReset: implement inline ==========================
 *
 * Standard CMSIS Cortex-M sequence — write VECTKEY (0x05FA) plus
 * SYSRESETREQ (bit 2) to SCB->AIRCR (0xE000ED0C), then spin. The OEM
 * has the same body at `0x0800052E` (already classified as vendor-
 * stock CMSIS-Core M0 inline). We provide it as `.weak` so a vendored
 * MindMotion / CMSIS build can override.
 */
    .section .text.NVIC_SystemReset, "ax", %progbits
    .weak    NVIC_SystemReset
    .type    NVIC_SystemReset, %function
    .thumb_func
NVIC_SystemReset:
    dsb     sy
    ldr     r0, =0x05FA0004         /* VECTKEY | SYSRESETREQ */
    ldr     r1, =0xE000ED0C         /* SCB->AIRCR */
    str     r0, [r1]
    dsb     sy
1:  b       1b
    .size   NVIC_SystemReset, .-NVIC_SystemReset
