#ifndef MAINWARE_UART_H
#define MAINWARE_UART_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * UART HAL (CubeF4 stm32f4xx_hal_uart.c, init/config/deinit path).
 *
 * Every serial link on the S3 main controller (the eight uartX_init/usartX_init
 * wrappers feeding the modem, e-shifter, display, and inter-module buses) is
 * brought up through this layer. Runtime byte traffic then goes through the
 * IRQ + ring-buffer path (uart_send_byte below); the blocking HAL_UART_Transmit/
 * Receive entry points are not used by this firmware.
 *
 * Reconstructed from the OEM functions at 0x08026AF0 (UART_SetConfig),
 * 0x08026CFC (HAL_UART_Init), and 0x08026D5A (HAL_UART_DeInit). Field offsets
 * and register-bit masks match the stock CubeF4 HAL exactly.
 * ------------------------------------------------------------------------- */

/* STM32F4 USART/UART peripheral register block (Instance points here). */
typedef struct {
    volatile uint32_t SR;      /* 0x00  status */
    volatile uint32_t DR;      /* 0x04  data */
    volatile uint32_t BRR;     /* 0x08  baud rate */
    volatile uint32_t CR1;     /* 0x0C  control 1 */
    volatile uint32_t CR2;     /* 0x10  control 2 */
    volatile uint32_t CR3;     /* 0x14  control 3 */
    volatile uint32_t GTPR;    /* 0x18  guard time / prescaler */
} USART_TypeDef;

typedef struct {
    uint32_t BaudRate;         /* 0x00 */
    uint32_t WordLength;       /* 0x04  CR1.M  (0 = 8-bit, 0x1000 = 9-bit) */
    uint32_t StopBits;         /* 0x08  CR2.STOP */
    uint32_t Parity;           /* 0x0C  CR1.PCE|PS */
    uint32_t Mode;             /* 0x10  CR1.TE|RE */
    uint32_t HwFlowCtl;        /* 0x14  CR3.RTSE|CTSE */
    uint32_t OverSampling;     /* 0x18  CR1.OVER8 (0x8000 = 8x) */
} UART_InitTypeDef;

typedef struct {
    USART_TypeDef    *Instance;     /* 0x00 */
    UART_InitTypeDef  Init;         /* 0x04 */
    uint8_t          *pTxBuffPtr;   /* 0x20 */
    uint16_t          TxXferSize;   /* 0x24 */
    volatile uint16_t TxXferCount;  /* 0x26 */
    uint8_t          *pRxBuffPtr;   /* 0x28 */
    uint16_t          RxXferSize;   /* 0x2C */
    volatile uint16_t RxXferCount;  /* 0x2E */
    void             *hdmatx;       /* 0x30 */
    void             *hdmarx;       /* 0x34 */
    volatile uint8_t  Lock;         /* 0x38 */
    volatile uint8_t  gState;       /* 0x39 */
    volatile uint8_t  RxState;      /* 0x3A */
    volatile uint32_t ErrorCode;    /* 0x3C */
} UART_HandleTypeDef;

/* HAL_StatusTypeDef */
#ifndef HAL_OK
#define HAL_OK        0
#define HAL_ERROR     1
#define HAL_BUSY      2
#define HAL_TIMEOUT   3
#endif

/* HAL_UART_StateTypeDef (gState / RxState) */
#define HAL_UART_STATE_RESET   0x00u
#define HAL_UART_STATE_READY   0x20u
#define HAL_UART_STATE_BUSY    0x24u

#define HAL_UART_ERROR_NONE    0x00u

/* UART_InitTypeDef.OverSampling = CR1.OVER8 when 8x sampling is selected. */
#define UART_OVERSAMPLING_8    0x8000u

/* CubeF4 HAL entry points (OEM addresses in uart.c). 0 = HAL_OK. */
int HAL_UART_Init(UART_HandleTypeDef *huart);
int HAL_UART_DeInit(UART_HandleTypeDef *huart);

/* Transmit one byte on the serial link (OEM uart_send_byte, 0x080364F0).
 * Pushes the byte into the TX ring buffer with the peripheral TX interrupt
 * masked for atomicity; returns the ring-buffer push status (1 = queued,
 * 0 = ring full). */
int uart_send_byte(uint8_t b);

/* UART5 (BLE-coprocessor link) RX/TX byte-pump interrupt service routine, invoked
 * via a thin vector trampoline (OEM 0x08036560). */
void uart5_irq_handler(void);

/* UART8 — the BLE-coprocessor *debug* link, bridged to the console by the
 * `bledebug` command. Locked TX/RX byte primitives (OEM 0x08036A38 / 0x08036A70;
 * each returns the ring-buffer status, 1 = byte moved) and the RX/TX byte-pump
 * ISR (OEM 0x08036AA8). */
int  uart8_tx_byte(uint8_t b);
int  uart8_rx_byte(uint8_t *out);
void uart8_irq_handler(void);

#endif
