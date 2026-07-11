#include <stddef.h>
#include <stdint.h>

#include "uart.h"
#include "util.h"
#include "panic.h"   /* Error_Handler */

/* ---------------------------------------------------------------------------
 * CubeF4 UART init/config/deinit (stm32f4xx_hal_uart.c). The eight
 * uartX_init/usartX_init wrappers populate a UART_HandleTypeDef and call
 * HAL_UART_Init; the BLE/modem/shifter reconfigure paths call HAL_UART_DeInit.
 * ------------------------------------------------------------------------- */

/* USART register bit masks (RM0430). */
#define USART_CR1_RE       0x0004u
#define USART_CR1_TE       0x0008u
#define USART_CR1_PS       0x0200u
#define USART_CR1_PCE      0x0400u
#define USART_CR1_M        0x1000u
#define USART_CR1_UE       0x2000u
#define USART_CR1_OVER8    0x8000u

#define USART_CR2_CLKEN    0x0800u
#define USART_CR2_STOP     0x3000u
#define USART_CR2_LINEN    0x4000u

#define USART_CR3_IREN     0x0002u
#define USART_CR3_HDSEL    0x0008u
#define USART_CR3_SCEN     0x0020u
#define USART_CR3_RTSE     0x0100u
#define USART_CR3_CTSE     0x0200u

/* APB2 USART/UART instances (clocked from PCLK2). On the STM32F413 these are
 * USART1, USART6, UART9, UART10 at 0x40011000 + n*0x400; every other serial
 * peripheral hangs off APB1 (PCLK1). */
#define USART1_BASE   0x40011000u
#define USART6_BASE   0x40011400u
#define UART9_BASE    0x40011800u
#define UART10_BASE   0x40011C00u

/* MSP hooks and APB clock queries live in other HAL modules. */
extern void     HAL_UART_MspInit(UART_HandleTypeDef *huart);    /* 0x080333C0 */
extern void     HAL_UART_MspDeInit(UART_HandleTypeDef *huart);  /* 0x08033740 */
extern uint32_t HAL_RCC_GetPCLK1Freq(void);                     /* 0x08027374 */
extern uint32_t HAL_RCC_GetPCLK2Freq(void);                     /* 0x08027394 */

/* Program BRR + the framing fields of CR1/CR2/CR3 from huart->Init.
 * OEM UART_SetConfig, 0x08026AF0. The OEM evaluates the BRR numerator
 * (pclk * 25) as a 64-bit dividend (__aeabi_uldivmod); the divisions below are
 * the behaviour-equivalent form of the CubeF4 UART_DIV_SAMPLING macros. */
static void UART_SetConfig(UART_HandleTypeDef *huart)
{
    USART_TypeDef *u = huart->Instance;
    uint32_t pclk;
    uint32_t usartdiv, mantissa, fraction;

    u->CR2 = (u->CR2 & ~USART_CR2_STOP) | huart->Init.StopBits;

    u->CR1 = (u->CR1 & ~(USART_CR1_M | USART_CR1_PCE | USART_CR1_PS |
                         USART_CR1_TE | USART_CR1_RE | USART_CR1_OVER8)) |
             huart->Init.OverSampling | huart->Init.WordLength |
             huart->Init.Parity | huart->Init.Mode;

    u->CR3 = (u->CR3 & ~(USART_CR3_RTSE | USART_CR3_CTSE)) | huart->Init.HwFlowCtl;

    if (u == (USART_TypeDef *)USART1_BASE || u == (USART_TypeDef *)USART6_BASE ||
        u == (USART_TypeDef *)UART9_BASE  || u == (USART_TypeDef *)UART10_BASE) {
        pclk = HAL_RCC_GetPCLK2Freq();
    } else {
        pclk = HAL_RCC_GetPCLK1Freq();
    }

    if (huart->Init.OverSampling == UART_OVERSAMPLING_8) {
        usartdiv = (uint32_t)(((uint64_t)pclk * 25u) / (2u * huart->Init.BaudRate));
        mantissa = usartdiv / 100u;
        fraction = ((usartdiv - mantissa * 100u) * 8u + 50u) / 100u;
        u->BRR = (mantissa << 4) | ((fraction & 0xF8u) << 1) | (fraction & 0x07u);
    } else {
        usartdiv = (uint32_t)(((uint64_t)pclk * 25u) / (4u * huart->Init.BaudRate));
        mantissa = usartdiv / 100u;
        fraction = ((usartdiv - mantissa * 100u) * 16u + 50u) / 100u;
        u->BRR = (mantissa << 4) | (fraction & 0xF0u) | (fraction & 0x0Fu);
    }
}

/* OEM HAL_UART_Init, 0x08026CFC. */
int HAL_UART_Init(UART_HandleTypeDef *huart)
{
    if (huart == NULL) {
        return HAL_ERROR;
    }

    if (huart->gState == HAL_UART_STATE_RESET) {
        huart->Lock = 0;                 /* HAL_UNLOCKED */
        HAL_UART_MspInit(huart);
    }

    huart->gState = HAL_UART_STATE_BUSY;

    huart->Instance->CR1 &= ~USART_CR1_UE;
    UART_SetConfig(huart);
    huart->Instance->CR2 &= ~(USART_CR2_LINEN | USART_CR2_CLKEN);
    huart->Instance->CR3 &= ~(USART_CR3_SCEN | USART_CR3_HDSEL | USART_CR3_IREN);
    huart->Instance->CR1 |= USART_CR1_UE;

    huart->ErrorCode = HAL_UART_ERROR_NONE;
    huart->gState  = HAL_UART_STATE_READY;
    huart->RxState = HAL_UART_STATE_READY;
    return HAL_OK;
}

/* OEM HAL_UART_DeInit (uart_handle_deinit), 0x08026D5A. */
int HAL_UART_DeInit(UART_HandleTypeDef *huart)
{
    if (huart == NULL) {
        return HAL_ERROR;
    }

    huart->gState = HAL_UART_STATE_BUSY;
    huart->Instance->CR1 &= ~USART_CR1_UE;

    HAL_UART_MspDeInit(huart);

    huart->ErrorCode = HAL_UART_ERROR_NONE;
    huart->gState  = HAL_UART_STATE_RESET;
    huart->RxState = HAL_UART_STATE_RESET;
    huart->Lock    = 0;                  /* HAL_UNLOCKED */
    return HAL_OK;
}

/* ── Per-port init wrappers ──────────────────────────────────────────────────
 * Eight near-identical CubeF4 MX_UARTx_Init routines (OEM 0x08033220..0x0803338C),
 * each populating a UART_HandleTypeDef (8-bit / 1 stop / no parity, Mode = TX|RX,
 * 16× oversampling) at its fixed SRAM handle, then calling HAL_UART_Init (which
 * drives HAL_UART_MspInit for GPIO-AF + RCC clock + NVIC). Kept inline in eight
 * standalone functions to mirror the OEM (no shared helper — each is a leaf that
 * writes the handle directly). Fatal Error_Handler on failure. */

void usart1_init(void)   /* 0x080332F0 — console / debug, 115200 */
{
    UART_HandleTypeDef *h = (UART_HandleTypeDef *)0x200098A4u;
    h->Instance = (USART_TypeDef *)0x40011000u;   /* USART1 */
    h->Init.BaudRate = 115200u;
    h->Init.WordLength = 0u; h->Init.StopBits = 0u; h->Init.Parity = 0u;
    h->Init.Mode = 0xcu; h->Init.HwFlowCtl = 0u; h->Init.OverSampling = 0u;
    if (HAL_UART_Init(h) != HAL_OK) { Error_Handler(); }
}

void usart2_init(void)   /* 0x08033324 — GSM modem, 115200 */
{
    UART_HandleTypeDef *h = (UART_HandleTypeDef *)0x200099A4u;
    h->Instance = (USART_TypeDef *)0x40004400u;   /* USART2 */
    h->Init.BaudRate = 115200u;
    h->Init.WordLength = 0u; h->Init.StopBits = 0u; h->Init.Parity = 0u;
    h->Init.Mode = 0xcu; h->Init.HwFlowCtl = 0u; h->Init.OverSampling = 0u;
    if (HAL_UART_Init(h) != HAL_OK) { Error_Handler(); }
}

void usart3_init(void)   /* 0x08033358 — inter-module bus, 9600 */
{
    UART_HandleTypeDef *h = (UART_HandleTypeDef *)0x20009824u;
    h->Instance = (USART_TypeDef *)0x40004800u;   /* USART3 */
    h->Init.BaudRate = 9600u;
    h->Init.WordLength = 0u; h->Init.StopBits = 0u; h->Init.Parity = 0u;
    h->Init.Mode = 0xcu; h->Init.HwFlowCtl = 0u; h->Init.OverSampling = 0u;
    if (HAL_UART_Init(h) != HAL_OK) { Error_Handler(); }
}

void usart6_init(void)   /* 0x0803338C — 38400 */
{
    UART_HandleTypeDef *h = (UART_HandleTypeDef *)0x20009924u;
    h->Instance = (USART_TypeDef *)0x40011400u;   /* USART6 */
    h->Init.BaudRate = 38400u;
    h->Init.WordLength = 0u; h->Init.StopBits = 0u; h->Init.Parity = 0u;
    h->Init.Mode = 0xcu; h->Init.HwFlowCtl = 0u; h->Init.OverSampling = 0u;
    if (HAL_UART_Init(h) != HAL_OK) { Error_Handler(); }
}

void uart4_init(void)    /* 0x08033220 — 9600 */
{
    UART_HandleTypeDef *h = (UART_HandleTypeDef *)0x20009964u;
    h->Instance = (USART_TypeDef *)0x40004C00u;   /* UART4 */
    h->Init.BaudRate = 9600u;
    h->Init.WordLength = 0u; h->Init.StopBits = 0u; h->Init.Parity = 0u;
    h->Init.Mode = 0xcu; h->Init.HwFlowCtl = 0u; h->Init.OverSampling = 0u;
    if (HAL_UART_Init(h) != HAL_OK) { Error_Handler(); }
}

void uart5_init(void)    /* 0x08033254 — serial TX primitive (g_uart_dev_pp), 115200 */
{
    UART_HandleTypeDef *h = (UART_HandleTypeDef *)0x20009864u;
    h->Instance = (USART_TypeDef *)0x40005000u;   /* UART5 */
    h->Init.BaudRate = 115200u;
    h->Init.WordLength = 0u; h->Init.StopBits = 0u; h->Init.Parity = 0u;
    h->Init.Mode = 0xcu; h->Init.HwFlowCtl = 0u; h->Init.OverSampling = 0u;
    if (HAL_UART_Init(h) != HAL_OK) { Error_Handler(); }
}

void uart7_init(void)    /* 0x08033288 — 115200 */
{
    UART_HandleTypeDef *h = (UART_HandleTypeDef *)0x200097E4u;
    h->Instance = (USART_TypeDef *)0x40007800u;   /* UART7 */
    h->Init.BaudRate = 115200u;
    h->Init.WordLength = 0u; h->Init.StopBits = 0u; h->Init.Parity = 0u;
    h->Init.Mode = 0xcu; h->Init.HwFlowCtl = 0u; h->Init.OverSampling = 0u;
    if (HAL_UART_Init(h) != HAL_OK) { Error_Handler(); }
}

void uart8_init(void)    /* 0x080332BC — 115200 */
{
    UART_HandleTypeDef *h = (UART_HandleTypeDef *)0x200098E4u;
    h->Instance = (USART_TypeDef *)0x40007C00u;   /* UART8 */
    h->Init.BaudRate = 115200u;
    h->Init.WordLength = 0u; h->Init.StopBits = 0u; h->Init.Parity = 0u;
    h->Init.Mode = 0xcu; h->Init.HwFlowCtl = 0u; h->Init.OverSampling = 0u;
    if (HAL_UART_Init(h) != HAL_OK) { Error_Handler(); }
}

/* Per-port HAL_UART_DeInit veneers (OEM 0x08033894..0x08033910), run as one leg
 * of enter_stop_mode's pre-sleep teardown. Indexed 0..7 over the same handles the
 * init wrappers use, in USART1/2/3 / UART4/5 / USART6 / UART7/8 order. */
void uart_handle_deinit_0(void) { HAL_UART_DeInit((UART_HandleTypeDef *)0x200098A4u); }  /* USART1 */
void uart_handle_deinit_1(void) { HAL_UART_DeInit((UART_HandleTypeDef *)0x200099A4u); }  /* USART2 */
void uart_handle_deinit_2(void) { HAL_UART_DeInit((UART_HandleTypeDef *)0x20009824u); }  /* USART3 */
void uart_handle_deinit_3(void) { HAL_UART_DeInit((UART_HandleTypeDef *)0x20009964u); }  /* UART4  */
void uart_handle_deinit_4(void) { HAL_UART_DeInit((UART_HandleTypeDef *)0x20009864u); }  /* UART5  */
void uart_handle_deinit_5(void) { HAL_UART_DeInit((UART_HandleTypeDef *)0x20009924u); }  /* USART6 */
void uart_handle_deinit_6(void) { HAL_UART_DeInit((UART_HandleTypeDef *)0x200097E4u); }  /* UART7  */
void uart_handle_deinit_7(void) { HAL_UART_DeInit((UART_HandleTypeDef *)0x200098E4u); }  /* UART8  */

/* UART/serial TX primitive (OEM uart_send_byte, 0x080364F0).
 *
 * The device handle is reached through a pointer-to-pointer at 0x20009864
 * (*g_uart_dev_pp -> the peripheral register block). The control register at
 * dev+0xC has its TX-interrupt-enable in bit 7; the OEM masks it (with a
 * DSB/ISB pair so the disable lands) before touching the TX ring buffer, then
 * re-enables it afterwards.
 *
 * ABI quirk preserved: the OEM never recomputes a return value — it leaves the
 * ring-push status that ringbuf_push_byte left in r0 untouched through the
 * trailing register re-enable, so the function implicitly returns that status.
 * slip_send_frame relies on this (it maps a 0 here to a TX-full error). */

extern volatile uint32_t **g_uart_dev_pp;   /* 0x20009864: -> device reg block */
extern uint8_t             g_uart_ctx[];     /* 0x20001A44: +0xB3C = TX ring ptr */

int uart_send_byte(uint8_t b)
{
    volatile uint32_t *dev = *g_uart_dev_pp;

    dev[0xC / 4] &= ~0x80u;                       /* mask TX interrupt */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    uint32_t rc = ringbuf_push_byte(*(ringbuf_t **)(g_uart_ctx + 0xB3C), b);

    dev = *g_uart_dev_pp;                          /* OEM re-loads the handle */
    dev[0xC / 4] |= 0x80u;                         /* re-enable TX interrupt */
    return (int)rc;
}

/* Clear a latched USART error flag the RM0430 way: if `flag` (PE 0x1 / FE 0x2 /
 * NE 0x4 / ORE 0x8) is set in SR, read SR then DR — that read sequence clears the
 * sticky error bit (and discards the offending byte). The OEM inlines this CubeF4
 * __HAL_UART_CLEAR_*FLAG idiom once per error bit. */
static void usart_clear_error_flag(volatile uint32_t *d, uint32_t flag)
{
    volatile uint32_t tmp;
    if ((d[0] & flag) != 0) {
        tmp = 0;
        tmp = d[0];   /* SR */
        tmp = d[1];   /* DR */
        (void)tmp;
    }
}

/* UART5 RX/TX byte-pump ISR (OEM 0x08036560), the BLE-coprocessor link, invoked
 * via a thin vector trampoline. On RXNE with no parity/framing/noise/overrun
 * error (SR bits 0-3 clear) and RXNEIE set, push the data byte into the BLE RX
 * ring (g_uart_ctx+0xB40, drained by ssp_rx_byte); then clear any latched
 * PE/FE/NE/ORE error (read SR+DR); on TXE with TXEIE set, pop the next TX byte
 * (g_uart_ctx+0xB3C, fed by uart_send_byte) and write it to DR, disabling TXEIE
 * when the ring drains. The RX/TX gates use SR/CR1 sampled once; the error-clear
 * block and TX-path register writes re-load the handle. */
void uart5_irq_handler(void)
{
    volatile uint32_t *dev = *g_uart_dev_pp;
    uint32_t sr  = dev[0];
    uint32_t cr1 = dev[0xC / 4];

    if ((sr & 0xFu) == 0 && (sr & 0x20u) != 0 && (cr1 & 0x20u) != 0) {
        ringbuf_push_byte(*(ringbuf_t **)(g_uart_ctx + 0xB40u), (uint8_t)dev[1]);
    }

    volatile uint32_t *d = *g_uart_dev_pp;
    usart_clear_error_flag(d, 0x1u);
    usart_clear_error_flag(d, 0x2u);
    usart_clear_error_flag(d, 0x4u);
    usart_clear_error_flag(d, 0x8u);

    if ((sr & 0x80u) != 0 && (cr1 & 0x80u) != 0) {
        uint8_t b;
        if (ringbuf_get_byte(*(ringbuf_t **)(g_uart_ctx + 0xB3Cu), &b) == 0) {
            (*g_uart_dev_pp)[0xC / 4] &= ~0x80u;
        } else {
            (*g_uart_dev_pp)[1] = b;
        }
    }
}

/* ---------------------------------------------------------------------------
 * UART8 — the BLE-coprocessor *debug* link (distinct from UART5's data link
 * above). It is reached only through the command-mode passthrough bridge: the
 * `bledebug` console command raises the bridge's UART8 flag, after which the
 * bridge shuttles bytes between the debug console and this UART. The handle is a
 * pointer-to-pointer at 0x200098E4; its TX ring handle sits at ctx+0x970 and the
 * RX ring at +0x974 (the same ctx block UART7 uses, at a different offset pair).
 * Same locked TX/RX primitives + RX/TX byte-pump ISR as UART5. The ring-push /
 * ring-get status that survives in r0 is the implicit return value (1 = byte
 * moved, 0 = ring full/empty); the bridge tests both.
 * ------------------------------------------------------------------------- */

#define UART8_HANDLE   (*(volatile uint32_t * volatile *)0x200098E4u)
#define UART8_CTX      0x20003C34u
#define UART8_TX_RING  (*(ringbuf_t * volatile *)(UART8_CTX + 0x970u))
#define UART8_RX_RING  (*(ringbuf_t * volatile *)(UART8_CTX + 0x974u))

int uart8_tx_byte(uint8_t b)
{
    volatile uint32_t *dev = UART8_HANDLE;

    dev[0xC / 4] &= ~0x80u;                        /* mask TXEIE */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    uint32_t rc = ringbuf_push_byte(UART8_TX_RING, b);

    dev = UART8_HANDLE;                            /* OEM re-loads the handle */
    dev[0xC / 4] |= 0x80u;                         /* re-enable TXEIE */
    return (int)rc;
}

int uart8_rx_byte(uint8_t *out)
{
    volatile uint32_t *dev = UART8_HANDLE;

    dev[0xC / 4] &= ~0x20u;                        /* mask RXNEIE */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    uint32_t rc = ringbuf_get_byte(UART8_RX_RING, out);

    dev = UART8_HANDLE;                            /* OEM re-loads the handle */
    dev[0xC / 4] |= 0x20u;                         /* re-enable RXNEIE */
    return (int)rc;
}

void uart8_irq_handler(void)
{
    volatile uint32_t *dev = UART8_HANDLE;
    uint32_t sr  = dev[0];
    uint32_t cr1 = dev[0xC / 4];

    if ((sr & 0xFu) == 0 && (sr & 0x20u) != 0 && (cr1 & 0x20u) != 0) {
        ringbuf_push_byte(UART8_RX_RING, (uint8_t)dev[1]);
    }

    volatile uint32_t *d = UART8_HANDLE;
    usart_clear_error_flag(d, 0x1u);
    usart_clear_error_flag(d, 0x2u);
    usart_clear_error_flag(d, 0x4u);
    usart_clear_error_flag(d, 0x8u);

    if ((sr & 0x80u) != 0 && (cr1 & 0x80u) != 0) {
        uint8_t b;
        if (ringbuf_get_byte(UART8_TX_RING, &b) == 0) {
            UART8_HANDLE[0xC / 4] &= ~0x80u;
        } else {
            UART8_HANDLE[1] = b;
        }
    }
}
