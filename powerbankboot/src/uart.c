/* uart.c — interrupt-driven USART2 ring-buffer comms (the OTA bus).
 *
 * Reconstructed from uart_rx_drain (0x080026F0), uart_tx_pump (0x08002768),
 * uart_tx_flush (0x08002810), uart_tx_string (0x0800264C), uart_tx_byte
 * (0x080026A4) and USART2_IRQHandler (0x08002248).
 *
 * The loader runs a hand-rolled HAL-style UART ISR over two ring buffers. The
 * main loop drains the RX ring into ota_process_byte() and primes the TX ring;
 * the ISR moves bytes between the rings and the USART2 data registers. The
 * driver state bytes mirror the HAL UART handle: gState/RxState live at handle
 * offsets +0x69/+0x6A and use HAL_UART_STATE_* values (READY = 0x20 = ' ').
 *
 * USART2 register layout (STM32F0): CR1 @ +0x00, ISR @ +0x1C, ICR @ +0x20,
 * RDR @ +0x24, TDR @ +0x28.
 */
#include "powerbankboot.h"

#define U2_CR1   REG32(USART2_BASE + 0x00)
#define U2_ISR   REG32(USART2_BASE + 0x1C)
#define U2_ICR   REG32(USART2_BASE + 0x20)
#define U2_RDR   REG32(USART2_BASE + 0x24)
#define U2_TDR   REG16(USART2_BASE + 0x28)

/* CR1 / ISR bit masks */
#define CR1_RXNEIE  0x20u
#define CR1_TCIE    0x40u
#define CR1_TXEIE   0x80u
#define ISR_ERRORS  0x0Fu   /* PE | FE | NE | ORE                              */
#define ISR_RXNE    0x20u
#define ISR_TC      0x40u
#define ISR_TXE     0x80u

/* HAL_UART_StateTypeDef subset */
#define UART_RESET    0x00u
#define UART_READY    0x20u   /* ' ' */
#define UART_BUSY_TX  0x21u   /* '!' */

/* Ring sizes pinned from the OEM literal-pool wrap limits (the loader compares
 * "limit < index+1 -> wrap", so the buffer holds limit+1 bytes):
 *   RX ring @ 0x20000BC4, wrap limit 0x3FF -> 1024 bytes
 *   TX ring @ 0x20000FCC, wrap limit 0xFFF -> 4096 bytes  (abuts the USART2
 *                                              handle at 0x20001FCC) */
#define RX_SIZE  1024
#define TX_SIZE  4096

static volatile uint8_t  s_rx_buf[RX_SIZE];
static volatile uint16_t s_rx_head, s_rx_tail;
static volatile uint8_t  s_tx_buf[TX_SIZE];
static volatile uint16_t s_tx_head, s_tx_tail;
static volatile uint8_t  s_tx_state = UART_READY;   /* handle +0x69 (gState)   */
static volatile uint8_t  s_rx_state = UART_READY;   /* handle +0x6A (RxState)  */

/* uart_tx_string() — enqueue a NUL-terminated string into the TX ring. */
void uart_tx_string(const char *s)
{
    for (; *s != '\0'; s++) {
        s_tx_buf[s_tx_head++] = (uint8_t)*s;
        if (s_tx_head > (TX_SIZE - 1))
            s_tx_head = 0;
    }
}

/* uart_tx_byte() — enqueue a single byte into the TX ring. */
void uart_tx_byte(uint8_t b)
{
    s_tx_buf[s_tx_head++] = b;
    if (s_tx_head > (TX_SIZE - 1))
        s_tx_head = 0;
}

/* uart_rx_drain() — feed every queued RX byte to the OTA protocol machine. */
void uart_rx_drain(void)
{
    if (s_rx_state != UART_READY)
        return;
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
        if (s_tx_head != s_tx_tail) {
            uint8_t b = s_tx_buf[s_tx_tail++];
            s_tx_state = UART_BUSY_TX;
            if (s_tx_tail > (TX_SIZE - 1))
                s_tx_tail = 0;
            U2_TDR = b;
            U2_CR1 |= CR1_TXEIE;
        }
    } else if (s_tx_state == UART_RESET) {
        s_tx_head = 0;
        s_tx_tail = 0;
    }
}

/* uart_tx_flush() — kick the TX engine and block until the ring has drained. */
void uart_tx_flush(void)
{
    if (s_tx_state != UART_RESET) {
        uart_tx_pump();
        while (s_tx_state != UART_READY)
            ;
    }
}

/* USART2_IRQHandler() — RXNE -> RX ring; TXE -> TX ring; TC -> mark idle.
 * Mirrors the OEM hand-rolled HAL UART IRQ (it keeps ring buffers rather than
 * the stock xfer counters). Error flags (PE/FE/NE/ORE) are cleared via ICR. */
void USART2_IRQHandler(void)
{
    uint32_t isr = U2_ISR;
    uint32_t cr1 = U2_CR1;

    /* ---- receive ---- */
    if ((isr & ISR_ERRORS) == 0) {
        if ((isr & ISR_RXNE) && (cr1 & CR1_RXNEIE)) {
            s_rx_buf[s_rx_head++] = (uint8_t)U2_RDR;
            if (s_rx_head > (RX_SIZE - 1))
                s_rx_head = 0;
        }
    } else {
        /* clear the latched error flags (PE/FE/NE/ORE) */
        U2_ICR = isr & ISR_ERRORS;
    }

    /* ---- transmit ---- */
    if ((isr & ISR_TXE) && (cr1 & CR1_TXEIE)) {
        if (s_tx_head == s_tx_tail) {
            U2_CR1 = (U2_CR1 & ~CR1_TXEIE) | CR1_TCIE;   /* ring empty: await TC */
        } else {
            uint8_t b = s_tx_buf[s_tx_tail++];
            if (s_tx_tail > (TX_SIZE - 1))
                s_tx_tail = 0;
            U2_TDR = b;
        }
    } else if ((isr & ISR_TC) && (cr1 & CR1_TCIE)) {
        U2_CR1 &= ~CR1_TCIE;
        s_tx_state = UART_READY;                          /* transmission done   */
    }
}
