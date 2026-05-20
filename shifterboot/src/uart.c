/* uart.c — VanMoof-custom USART1 send-byte / send-buffer wrappers
 *          on top of the MindMotion HAL.
 *
 * Two leaves, both 28 bytes, decomp'd from shifterboot.bin:
 *
 *   - `uart1_send_byte`  @ 0x080000C8 — write one byte to USART1, then
 *     spin on the TX-ready/complete flag (SR bit 0 on MM32) before
 *     returning. The OEM realises the spin via `USART_GetFlagStatus`
 *     with flag id 1.
 *
 *   - `uart1_send_buf`   @ 0x080000E4 — drive a 16-bit count of bytes
 *     out through `uart1_send_byte`. The OEM compiles this as a
 *     decrement-then-test loop with the count in a uxth-clamped
 *     register, so the parameter is `uint16_t`.
 *
 * Calls into the MindMotion HAL (`USART_SendData` @ 0x08001364,
 * `USART_GetFlagStatus` @ 0x08001372 — both vendor-stock per
 * `docs/progress.md`). The HAL functions are declared `extern` here;
 * their bodies will arrive when the MindMotion BSP is vendored in.
 */

#include "uart.h"

#include <stdint.h>

/* MM32F031 peripheral pointer for USART1. The OEM loads this from a
 * literal pool word at 0x080004C8. Declared as an opaque pointer
 * because we don't yet ship a CMSIS-style USART_TypeDef; the MindMotion
 * HAL primitives take a `USART_TypeDef *` (which on this part is just
 * the base address wrapped in a struct), and `void *` covers either. */
static void *const USART1_BASE = (void *)0x40013800u;

/* MindMotion HAL leaves (vendor-stock — not yet present in the build).
 * Signatures match MindMotion's `hal_uart.h`. */
extern void     USART_SendData(void *USARTx, uint16_t Data);
extern uint32_t USART_GetFlagStatus(void *USARTx, uint16_t USART_FLAG);

/* Additional HAL leaves used by `boot_init_usart1` below — all
 * vendor-stock per `docs/progress.md`. Signatures match MindMotion
 * `hal_rcc.h` / `hal_gpio.h` / `hal_uart.h` / `hal_misc.h`. */
extern void RCC_AHBPeriphClockCmd(uint32_t RCC_AHBPeriph, uint32_t NewState);
extern void RCC_APB2PeriphClockCmd(uint32_t RCC_APB2Periph, uint32_t NewState);
extern void GPIO_Init(void *GPIOx, void *GPIO_InitStruct);
extern void GPIO_PinAFConfig(void *GPIOx, uint16_t GPIO_PinSource, uint8_t GPIO_AF);
extern void USART_Init(void *USARTx, void *USART_InitStruct);
extern void USART_Cmd(void *USARTx, uint32_t NewState);
extern void USART_ITConfig(void *USARTx, uint16_t USART_IT, uint32_t NewState);
extern void NVIC_Init(void *NVIC_InitStruct);

/* MM32F031 GPIOB peripheral base (`reference/mm32f031/README.md`). */
static void *const GPIOB_BASE = (void *)0x48000400u;

/* RCC clock-enable bit masks materialised by the OEM as `(1u << N)`. */
#define RCC_APB2Periph_USART1   (1u << 14)   /* APB2ENR bit 14 */
#define RCC_AHBPeriph_GPIOB     (1u << 18)   /* AHBENR  bit 18 */

#define USART1_IRQn             27           /* CMSIS IRQn for USART1 */

/* MM32F031 GPIO mode constants the MindMotion HAL encodes into
 * `GPIO_InitTypeDef.GPIO_Mode`. The values land verbatim in the OEM
 * disassembly: `0x18` = AF push-pull, `0x04` = floating input. */
#define GPIO_Mode_AF_PP         (0x18u)
#define GPIO_Mode_IN_FLOATING   (0x04u)

#define GPIO_Speed_50MHz        (3u)

#define GPIO_Pin_6              (1u << 6)
#define GPIO_Pin_7              (1u << 7)

#define GPIO_PinSource6         (6u)
#define GPIO_PinSource7         (7u)
#define GPIO_AF_0               (0u)

/* MindMotion USART_InitTypeDef field constants. `0x30` is the HAL's
 * encoding for "8 data bits" on this part (not bit 12 of CR1 — the
 * HAL translates these magic numbers internally). `0x18` is the
 * combined `Mode_Rx | Mode_Tx` selector. */
#define USART_WordLength_8b              (0x30u)
#define USART_StopBits_1                 (0x00u)
#define USART_Parity_No                  (0x00u)
#define USART_Mode_Rx_Tx                 (0x18u)
#define USART_HardwareFlowControl_None   (0x00u)

#define USART_IT_RXNE                    (2u)
#define ENABLE                           (1u)
#define DISABLE                          (0u)

/* HAL init-struct layouts. Field offsets come from the OEM
 * disassembly (the OEM materialises strb/strh at each `&struct + N`
 * step). All three structs live in a single 24-byte stack region in
 * the OEM body — we let the compiler manage that. */
typedef struct {
    uint8_t  NVIC_IRQChannel;
    uint8_t  NVIC_IRQChannelPriority;
    uint8_t  NVIC_IRQChannelCmd;
} NVIC_InitTypeDef;

typedef struct {
    uint32_t USART_BaudRate;
    uint16_t USART_WordLength;
    uint16_t USART_StopBits;
    uint16_t USART_Parity;
    uint16_t USART_Mode;
    uint16_t USART_HardwareFlowControl;
} USART_InitTypeDef;

typedef struct {
    uint16_t GPIO_Pin;
    uint8_t  GPIO_Speed;
    uint8_t  GPIO_Mode;
} GPIO_InitTypeDef;

/* OEM @ 0x08001578 (150 B). Sole caller is `main` at `0x08000214`
 * with the baud rate materialised as `75 << 7 = 9600` — the
 * canonical low-speed Modbus RTU rate for the S3 inter-module bus.
 *
 * Sets up USART1 end-to-end:
 *   1. Enable USART1 + GPIOB clocks
 *   2. NVIC IRQ 27 (USART1) at priority 3
 *   3. PB6 / PB7 to alternate-function 0 (USART1_TX / USART1_RX)
 *   4. USART_Init with caller-supplied baud rate, 8-N-1, RX+TX
 *   5. Enable the RXNE interrupt (drives `USART1_IRQHandler` →
 *      Modbus accumulator)
 *   6. Enable USART1
 *   7. Configure PB6 as AF push-pull (TX), PB7 as floating input (RX)
 *
 * The post-Init GPIO_Init pair is the only part of the sequence
 * that's mode-distinguishable from a generic UART setup — VanMoof's
 * choice of `AF_PP` for TX + `IN_FLOATING` for RX is the standard
 * STM32-derived USART pin recipe. */
void boot_init_usart1(uint32_t baud_rate)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_GPIOB, ENABLE);

    NVIC_InitTypeDef nvic = {
        .NVIC_IRQChannel         = USART1_IRQn,
        .NVIC_IRQChannelPriority = 3u,
        .NVIC_IRQChannelCmd      = ENABLE,
    };
    NVIC_Init(&nvic);

    GPIO_PinAFConfig(GPIOB_BASE, GPIO_PinSource6, GPIO_AF_0);
    GPIO_PinAFConfig(GPIOB_BASE, GPIO_PinSource7, GPIO_AF_0);

    USART_InitTypeDef usart = {
        .USART_BaudRate            = baud_rate,
        .USART_WordLength          = USART_WordLength_8b,
        .USART_StopBits            = USART_StopBits_1,
        .USART_Parity              = USART_Parity_No,
        .USART_Mode                = USART_Mode_Rx_Tx,
        .USART_HardwareFlowControl = USART_HardwareFlowControl_None,
    };
    USART_Init(USART1_BASE, &usart);

    USART_ITConfig(USART1_BASE, USART_IT_RXNE, ENABLE);
    USART_Cmd(USART1_BASE, ENABLE);

    GPIO_InitTypeDef gpio = {
        .GPIO_Pin   = GPIO_Pin_6,
        .GPIO_Speed = GPIO_Speed_50MHz,
        .GPIO_Mode  = GPIO_Mode_AF_PP,
    };
    GPIO_Init(GPIOB_BASE, &gpio);

    gpio.GPIO_Pin  = GPIO_Pin_7;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOB_BASE, &gpio);
}

/* MM32 USART status flag selector consumed by `USART_GetFlagStatus`.
 * The MindMotion HAL's flag enum encodes the SR bit position; for the
 * MM32F031 silicon the "TX-ready / TX-complete" bit is SR bit 0, which
 * the HAL exposes as flag id 1. Same wire-level meaning as the
 * shifterware-side `USART_SR_TX_READY_Msk` (`1u << 0`). */
#define USART_FLAG_TX_READY  (1u)

/* OEM @ 0x080000C8 (28 B). The compiler emits a redundant
 *     `mov r4, r0 ; mov r1, r4`
 * pair before the first HAL call, and a `nop` between the two HAL
 * calls — both are -O0 artefacts in the MindMotion BSP build. We
 * don't attempt to reproduce them; control flow and side effects
 * match. */
void uart1_send_byte(uint8_t b)
{
    USART_SendData(USART1_BASE, (uint16_t)b);
    while (USART_GetFlagStatus(USART1_BASE, USART_FLAG_TX_READY) == 0u) {
        /* spin until TX-ready */
    }
}

/* OEM @ 0x080000E4 (28 B). Decrement-then-test loop with a
 * pre-test jump (`b 0x080000F4`) skipping the body on the first
 * iteration so `len == 0` is a no-op. The count register holds a
 * `uxth`-clamped value, fixing `len`'s width at 16 bits. */
void uart1_send_buf(const uint8_t *buf, uint16_t len)
{
    while (len != 0u) {
        uart1_send_byte(*buf);
        buf++;
        len = (uint16_t)(len - 1u);
    }
}
