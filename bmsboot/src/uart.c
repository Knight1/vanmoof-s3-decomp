/* uart.c — interrupt-driven USART1 ring-buffer comms (the OTA bus).
 *
 * Reconstructed from download_uart_init (0x08001A90), comms_rx_state_init
 * (0x080017C8), uart_tx_string (0x08001C7C), uart_tx_byte (0x08001CE0),
 * uart_rx_drain (0x08001D38), uart_tx_pump (0x08001DA0), uart_tx_flush
 * (0x08001E3C), comms_deinit (0x08001C1C) and USART1_IRQHandler (0x080017E8).
 *
 * A hand-rolled HAL-style UART ISR over two ring buffers. The super-loop drains
 * the RX ring into ota_process_byte() and primes the TX ring; the ISR moves
 * bytes between the rings and the USART1 data registers. The driver state bytes
 * mirror the HAL UART handle gState/RxState (HAL_UART_STATE_READY = 0x20).
 *
 * The comms port is only brought up when download_pin_check() asserts it, so
 * the tx/rx helpers no-op (s_uart_enabled == 0) on a normal boot.
 *
 * USART1 register layout (STM32L0): CR1 @ +0x00, CR3 @ +0x08, ISR @ +0x1C,
 * ICR @ +0x20, RDR @ +0x24, TDR @ +0x28.
 */
#include "bmsboot.h"

#define U1_CR1   REG32(USART1_BASE + 0x00)
#define U1_CR3   REG32(USART1_BASE + 0x08)
#define U1_ISR   REG32(USART1_BASE + 0x1C)
#define U1_ICR   REG32(USART1_BASE + 0x20)
#define U1_RDR   REG32(USART1_BASE + 0x24)
#define U1_TDR   REG16(USART1_BASE + 0x28)

/* CR1 / CR3 / ISR bit masks */
#define CR1_RXNEIE  0x20u
#define CR1_TCIE    0x40u
#define CR1_TXEIE   0x80u
#define CR1_PEIE    0x100u
#define CR3_EIE     0x01u
#define CR3_DMAR    0x40u
#define ISR_ERRORS  0x0Fu   /* PE | FE | NE | ORE                              */
#define ISR_RXNE    0x20u
#define ISR_TC      0x40u
#define ISR_TXE     0x80u
#define RX_ERR_OFF  0xFFFFFEDFu  /* clear RXNEIE|PEIE on overrun shutdown      */

/* HAL_UART_StateTypeDef subset */
#define UART_RESET    0x00u
#define UART_READY    0x20u   /* ' ' */
#define UART_BUSY_TX  0x21u   /* '!' */

/* Ring sizes pinned from the OEM literal-pool wrap limits ("limit < index+1 ->
 * wrap", so the buffer holds limit+1 bytes):
 *   TX ring @ 0x200008C0, wrap limit 0xFFF -> 4096 bytes
 *   RX ring @ 0x200018C0, wrap limit 0x3FF -> 1024 bytes (abuts the UART handle
 *                                              at 0x20001CC0) */
#define RX_SIZE  1024
#define TX_SIZE  4096

static volatile uint8_t  s_rx_buf[RX_SIZE];
static volatile uint16_t s_rx_head, s_rx_tail;
static volatile uint8_t  s_tx_buf[TX_SIZE];
static volatile uint16_t s_tx_ridx, s_tx_widx;
static volatile uint8_t  s_tx_state = UART_READY;   /* handle gState (+0x78)   */
static volatile uint8_t  s_rx_state = UART_READY;   /* handle RxState (+0x7C)  */
static uint8_t           s_uart_enabled;            /* 0x20001D45              */

/* STM32L0 HAL UART/GPIO/NVIC driver (vendor-stock). */
extern void hal_gpio_init(uint32_t port, void *gpio);
extern void hal_gpio_deinit(uint32_t port, uint16_t pins);
extern uint32_t hal_uart_init(void *huart);
extern void hal_uart_deinit(void *huart);
extern void nvic_set_priority(int irqn, uint32_t pre, uint32_t sub);
extern void nvic_enable_irq(int irqn);
extern void nvic_disable_irq(int irqn);
extern uint8_t g_huart1[0x84];                      /* USART1 UART_HandleTypeDef */

#define HAL_INIT_RETRY_LIMIT  0x32u   /* 50 attempts before fail-safe */

/* comms_rx_state_init() — clear the RX ring indices (called from boot_hw_init). */
void comms_rx_state_init(void)
{
    s_rx_head = 0;
    s_rx_tail = 0;
}

/* download_uart_init() — USART1 @ 9600 8N1 on PA9/PA10 (AF4), RX-interrupt +
 * NVIC. Invoked from download_pin_check() only when the host requests the
 * serial-download server. */
void download_uart_init(void)
{
    /* clocks: USART1 (APB2ENR bit14) + GPIOA (IOPENR bit0) */
    REG32(RCC_BASE + RCC_APB2ENR) |= 0x4000u;
    REG32(RCC_BASE + RCC_IOPENR)  |= 0x0001u;
    s_uart_enabled = 1;

    /* PA9/PA10 -> USART1_TX/RX, alternate function AF4 (HAL GPIO_InitTypeDef) */
    gpio_cfg_t gpio = { 0x0600u, 2u, 0u, 3u, 4u };   /* pins PA9|PA10, AF mode, AF4 */
    hal_gpio_init(GPIOA_BASE, &gpio);

    /* USART1 @ 9600 8N1 (handle->Init.BaudRate = 0x2580; HAL fills BRR) */
    *(uint32_t *)g_huart1 = USART1_BASE;             /* handle->Instance */
    *(uint32_t *)(g_huart1 + 4) = 0x2580u;           /* handle->Init.BaudRate */

    uint8_t tries = 0;
    do {
        if (hal_uart_init(g_huart1) == 0) {
            tries = HAL_INIT_RETRY_LIMIT;
        } else if (++tries > (HAL_INIT_RETRY_LIMIT - 1)) {
            failsafe();                              /* no return */
        }
    } while (tries < HAL_INIT_RETRY_LIMIT);

    s_tx_state = UART_READY;
    s_rx_state = UART_READY;
    s_tx_ridx = s_tx_widx = 0;
    s_rx_head = s_rx_tail = 0;

    U1_CR1 |= CR1_RXNEIE;                             /* enable RXNE interrupt */
    nvic_set_priority(USART1_IRQn, 0, 0);
    nvic_enable_irq(USART1_IRQn);
}

/* uart_tx_string() — enqueue a NUL-terminated string into the TX ring. */
void uart_tx_string(const char *s)
{
    if (s_uart_enabled != 1)
        return;
    for (; *s != '\0'; s++) {
        s_tx_buf[s_tx_widx++] = (uint8_t)*s;
        if (s_tx_widx > (TX_SIZE - 1))
            s_tx_widx = 0;
    }
}

/* uart_tx_byte() — enqueue a single byte into the TX ring. */
void uart_tx_byte(uint8_t b)
{
    if (s_uart_enabled != 1)
        return;
    s_tx_buf[s_tx_widx++] = b;
    if (s_tx_widx > (TX_SIZE - 1))
        s_tx_widx = 0;
}

/* uart_rx_drain() — feed every queued RX byte to the OTA protocol machine. */
void uart_rx_drain(void)
{
    while (s_rx_head != s_rx_tail) {
        uint8_t b = s_rx_buf[s_rx_tail++];
        if (s_rx_tail > (RX_SIZE - 1))
            s_rx_tail = 0;
        ota_process_byte(b);
    }
}

/* uart_tx_pump() — if the line is idle and the ring has data, kick off an
 * interrupt-driven transmit (load TDR, enable TXEIE; the ISR finishes it). */
void uart_tx_pump(void)
{
    if (s_tx_state == UART_READY) {
        if (s_tx_ridx != s_tx_widx) {
            uint8_t b = s_tx_buf[s_tx_ridx++];
            s_tx_state = UART_BUSY_TX;
            if (s_tx_ridx > (TX_SIZE - 1))
                s_tx_ridx = 0;
            U1_TDR = b;
            U1_CR1 |= CR1_TXEIE;
        }
    } else if (s_tx_state == UART_RESET) {
        s_tx_widx = 0;
        s_tx_ridx = 0;
    }
}

/* uart_tx_flush() — kick the TX engine and block until the ring has drained
 * (gState back to READY). */
void uart_tx_flush(void)
{
    if (s_tx_state != UART_RESET) {
        uart_tx_pump();
        while (s_tx_state != UART_READY)
            ;
    }
}

/* comms_deinit() — tear down USART1 (NVIC, HAL, clock, PA9/PA10) before the
 * loader hands control to the application. */
void comms_deinit(void)
{
    if (s_uart_enabled == 1) {
        nvic_disable_irq(USART1_IRQn);
        hal_uart_deinit(g_huart1);
        REG32(RCC_BASE + RCC_APB2ENR) &= ~0x4000u;    /* USART1 clock off */
        hal_gpio_deinit(GPIOA_BASE, 0x0600u);          /* release PA9/PA10 */
        s_tx_state = UART_RESET;
        s_rx_state = UART_RESET;
    }
    s_uart_enabled = 0;
}

/* comms_disable() — used by download_pin_check() when the host did not request
 * the download server: hold the comms port down. */
void comms_disable(void)
{
    s_uart_enabled = 0;
    s_tx_state = UART_RESET;
    s_rx_state = UART_RESET;
}

/* USART1_IRQHandler() — RXNE -> RX ring; error flags -> ICR + recovery;
 * TXE -> TX ring; TC -> mark idle. Mirrors the OEM hand-rolled HAL UART IRQ
 * (it keeps ring buffers rather than the stock xfer counters). */
void USART1_IRQHandler(void)
{
    uint32_t isr = U1_ISR;
    uint32_t cr1 = U1_CR1;
    uint32_t cr3 = U1_CR3;
    uint32_t errcode = 0;

    /* ---- receive ---- */
    if ((isr & ISR_ERRORS) == 0) {
        if ((isr & ISR_RXNE) && (cr1 & CR1_RXNEIE)) {
            s_rx_buf[s_rx_head++] = (uint8_t)U1_RDR;
            if (s_rx_head > (RX_SIZE - 1))
                s_rx_head = 0;
        }
    } else if ((cr3 & CR3_EIE) || (cr1 & (CR1_RXNEIE | CR1_PEIE))) {
        /* clear the latched error flags (PE/FE/NE/ORE) via ICR */
        if ((isr & 0x1u) && (cr1 & CR1_PEIE)) { U1_ICR = 0x1u; errcode |= 0x1u; }
        if ((isr & 0x2u) && (cr3 & CR3_EIE))  { U1_ICR = 0x2u; errcode |= 0x4u; }
        if ((isr & 0x4u) && (cr3 & CR3_EIE))  { U1_ICR = 0x4u; errcode |= 0x2u; }
        if ((isr & 0x8u) && ((cr1 & CR1_RXNEIE) || (cr3 & CR3_EIE))) {
            U1_ICR = 0x8u; errcode |= 0x8u;
        }
        if (errcode != 0) {
            if ((isr & ISR_RXNE) && (cr1 & CR1_RXNEIE)) {
                s_rx_buf[s_rx_head++] = (uint8_t)U1_RDR;
                if (s_rx_head > (RX_SIZE - 1))
                    s_rx_head = 0;
            }
            if ((errcode & 0x8u) == 0 && (cr3 & CR3_DMAR) != CR3_DMAR) {
                errcode = 0;
            } else {
                /* overrun / DMA path: shut error IRQs, re-arm RXNE, mark ready */
                U1_CR1 &= RX_ERR_OFF;
                U1_CR3 &= ~CR3_EIE;
                U1_CR1 |= CR1_RXNEIE;
                s_rx_state = UART_READY;
            }
        }
    }

    /* ---- transmit ---- */
    if (((isr & ISR_TXE) == 0) || ((cr1 & CR1_TXEIE) == 0)) {
        if ((isr & ISR_TC) && (cr1 & CR1_TCIE)) {
            U1_CR1 &= ~CR1_TCIE;
            s_tx_state = UART_READY;                  /* transmission done */
        }
    } else if (s_tx_ridx == s_tx_widx) {
        U1_CR1 &= ~CR1_TXEIE;                          /* ring empty: await TC */
        U1_CR1 |= CR1_TCIE;
    } else {
        uint8_t b = s_tx_buf[s_tx_ridx++];
        if (s_tx_ridx > (TX_SIZE - 1))
            s_tx_ridx = 0;
        U1_TDR = b;
    }
}
