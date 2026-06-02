#include "powerbankware.h"

/*
 * SPI HAL layer for the FEDL5236 AFE link.
 *
 *   spi_get_state        = OEM FUN_0801c9ec  (HAL handle State byte @ +0x5D)
 *   spi_transmit_receive = OEM FUN_0801c844  (HAL_SPI_TransmitReceive_IT)
 *   spi_error_reset      = OEM FUN_0800fe3a -> FUN_0800f50c (system reset)
 *
 * The handle is the standard STM32 HAL_SPI_HandleTypeDef; the OEM accesses it
 * by raw offset, reproduced here. Relevant fields:
 *   +0x00 Instance (SPI regs)  +0x04 Init.Mode   +0x08 Init.Direction
 *   +0x0C Init.DataSize        +0x38 pTxBuffPtr  +0x3C/3E TxXferSize/Count
 *   +0x40 pRxBuffPtr           +0x44/46 RxXferSize/Count
 *   +0x4C RxISR  +0x50 TxISR   +0x5C Lock  +0x5D State  +0x60 ErrorCode
 * SPI registers: +0x00 CR1, +0x04 CR2.
 *
 * HAL state/status codes: State 1=READY, 4=BUSY_RX, 5=BUSY_TX_RX;
 * return 0=HAL_OK, 1=HAL_ERROR, 2=HAL_BUSY.
 */

typedef void (*spi_isr_t)(void *handle);

/* The four interrupt-mode transfer ISRs (8- vs 16-bit data), selected by
 * Init.DataSize; defined at the bottom of this file. Their addresses are
 * stored into the handle's RxISR/TxISR fields, exactly as the OEM does. */
void spi_rx_isr_8bit(void *handle);    /* FUN_0801ca04 */
void spi_tx_isr_8bit(void *handle);    /* FUN_0801cac2 */
void spi_rx_isr_16bit(void *handle);   /* FUN_0801cb52 */
void spi_tx_isr_16bit(void *handle);   /* FUN_0801cbba */

/* HAL handle State byte (+0x5D): 1 == HAL_SPI_STATE_READY. */
uint8_t spi_get_state(void *handle)
{
    return *(volatile uint8_t *)((uint8_t *)handle + 0x5d);
}

/*
 * hal_error_reset — OEM FUN_0800f50c. Requests a system reset through
 * SCB->AIRCR (VECTKEY | SYSRESETREQ) and spins until it takes effect.
 */
static void hal_error_reset(void)
{
    __DSB();
    *(volatile uint32_t *)(0xE000ED00u + 0x0C) = 0x05FA0004u;   /* SCB_AIRCR */
    __DSB();
    for (;;) {
    }
}

void spi_error_reset(void)
{
    hal_error_reset();
}

/*
 * HAL_SPI_TransmitReceive_IT — arm a full-duplex interrupt-mode transfer.
 *
 * Validates lock/state, latches the TX/RX buffers and counts, picks the
 * 8- or 16-bit ISR pair, sets the FIFO RX threshold (FRXTH) for sub-2-byte
 * 8-bit transfers, enables the SPI error/RX/TX interrupts (CR2 |= 0xE0) and
 * the peripheral (CR1 SPE). The ISRs then move the data.
 */
int spi_transmit_receive(void *handle, volatile uint8_t *tx,
                         volatile uint8_t *rx, uint32_t count)
{
    uint8_t  *h    = (uint8_t *)handle;
    uint32_t *spi  = *(uint32_t **)(h + 0x00);          /* Instance */
    uint32_t  mode = *(uint32_t *)(h + 0x04);
    uint32_t  dir  = *(uint32_t *)(h + 0x08);
    uint32_t  data_size = *(uint32_t *)(h + 0x0C);
    volatile uint8_t *state = (volatile uint8_t *)(h + 0x5d);

    if (*(volatile uint8_t *)(h + 0x5c) == 1) {         /* Lock held */
        return 2;                                       /* HAL_BUSY */
    }
    *(volatile uint8_t *)(h + 0x5c) = 1;                /* take Lock */

    int ret = 0;
    if (*state == 1 || (mode == 0x104 && dir == 0 && *state == 4)) {
        if (tx == 0 || rx == 0 || count == 0) {
            ret = 1;                                    /* HAL_ERROR */
        } else {
            if (*state != 4) {
                *state = 5;                             /* BUSY_TX_RX */
            }
            *(uint32_t *)(h + 0x60) = 0;                /* ErrorCode = NONE */

            *(uint8_t **)(h + 0x38) = (uint8_t *)tx;    /* pTxBuffPtr */
            *(uint16_t *)(h + 0x3c) = (uint16_t)count;  /* TxXferSize  */
            *(uint16_t *)(h + 0x3e) = (uint16_t)count;  /* TxXferCount */
            *(uint8_t **)(h + 0x40) = (uint8_t *)rx;    /* pRxBuffPtr */
            *(uint16_t *)(h + 0x44) = (uint16_t)count;  /* RxXferSize  */
            *(uint16_t *)(h + 0x46) = (uint16_t)count;  /* RxXferCount */

            if (data_size < 0x701) {                    /* <= 8-bit data */
                *(spi_isr_t *)(h + 0x4c) = spi_rx_isr_8bit;
                *(spi_isr_t *)(h + 0x50) = spi_tx_isr_8bit;
            } else {
                *(spi_isr_t *)(h + 0x4c) = spi_rx_isr_16bit;
                *(spi_isr_t *)(h + 0x50) = spi_tx_isr_16bit;
            }

            if (data_size < 0x701 && *(uint16_t *)(h + 0x46) < 2) {
                spi[1] |= 0x1000u;                      /* CR2 FRXTH */
            } else {
                spi[1] &= 0xFFFFEFFFu;
            }
            spi[1] |= 0xE0u;                            /* CR2 ERRIE|RXNEIE|TXEIE */
            if ((spi[0] & 0x40u) != 0x40u) {
                spi[0] |= 0x40u;                        /* CR1 SPE */
            }
        }
    } else {
        ret = 2;                                        /* HAL_BUSY */
    }

    *(volatile uint8_t *)(h + 0x5c) = 0;                /* release Lock */
    return ret;
}

/* ----------------------------------------------------------------------- *
 * Interrupt-mode transfer ISRs + close path.
 *
 * These are the STM32 HAL SPI 2-line TX/RX ISRs the IT transfer arms. The
 * handle's RxISR/TxISR pointers (set in spi_transmit_receive) land here; the
 * SPI IRQ handler (own pass) dispatches through them. SPI registers off the
 * Instance pointer (handle +0x00): +0x00 CR1, +0x04 CR2, +0x08 SR, +0x0C DR.
 */

uint32_t tick_get(void)
{
    return *(volatile uint32_t *)0x20002614;
}

/* HAL_SPI_TxRxCpltCallback (FUN_08010eec): raise CS (PA15) + PA8 to release
 * the FEDL5236 after the full-duplex transfer. */
static void spi_txrx_cplt_cb(void *handle)
{
    (void)handle;
    gpio_bit_write(0x48000000u, 0x8000, 1);   /* PA15 = CS high */
    gpio_bit_write(0x48000000u, 0x100, 1);    /* PA8 high       */
}

/* HAL_SPI_RxCpltCallback (FUN_0801c9dc): empty in this firmware. */
static void spi_rx_cplt_cb(void *handle)
{
    (void)handle;
}

/* HAL_SPI_ErrorCallback (FUN_08010f1c): fatal — system reset. */
static void spi_error_cb(void *handle)
{
    (void)handle;
    spi_error_reset();
}

/*
 * SPI_WaitFifoStateUntilTimeout (FUN_0801ccf8): poll SR masked bits until
 * they equal `expected`, or abort on timeout. SPI_WaitFlagStateUntilTimeout
 * (FUN_0801cc1c) is the same but matches the all-bits-set boolean. Both, on
 * timeout, disable the SPI interrupts, drop SPE for master 1-line/RX-only,
 * re-arm CRC, mark the handle READY/unlocked and return 3.
 */
static int spi_wait_fifo_state(uint8_t *h, uint32_t mask, uint32_t expected,
                               uint32_t timeout, uint32_t start)
{
    volatile uint32_t *spi = *(volatile uint32_t **)(h + 0);
    for (;;) {
        if (expected == (spi[2] & mask)) {
            return 0;
        }
        if (!(timeout == 0xffffffffu ||
              (timeout != 0 && (uint32_t)(tick_get() - start) < timeout))) {
            break;
        }
    }
    spi[1] &= 0xffffff1fu;                                /* disable IE bits */
    if (*(uint32_t *)(h + 4) == 0x104 &&
        (*(uint32_t *)(h + 8) == 0x8000 || *(uint32_t *)(h + 8) == 0x400)) {
        spi[0] &= 0xffffffbfu;                            /* SPE off */
    }
    if (*(uint32_t *)(h + 0x28) == 0x2000) {              /* CRC enabled */
        spi[0] &= 0xffffdfffu;
        spi[0] |= 0x2000u;
    }
    *(volatile uint8_t *)(h + 0x5d) = 1;                  /* State = READY */
    *(volatile uint8_t *)(h + 0x5c) = 0;                  /* unlock */
    return 3;
}

static int spi_wait_flag_state(uint8_t *h, uint32_t mask, uint32_t expected,
                               uint32_t timeout, uint32_t start)
{
    volatile uint32_t *spi = *(volatile uint32_t **)(h + 0);
    for (;;) {
        if ((uint32_t)(mask == (spi[2] & mask)) == expected) {
            return 0;
        }
        if (!(timeout == 0xffffffffu ||
              (timeout != 0 && (uint32_t)(tick_get() - start) < timeout))) {
            break;
        }
    }
    spi[1] &= 0xffffff1fu;
    if (*(uint32_t *)(h + 4) == 0x104 &&
        (*(uint32_t *)(h + 8) == 0x8000 || *(uint32_t *)(h + 8) == 0x400)) {
        spi[0] &= 0xffffffbfu;
    }
    if (*(uint32_t *)(h + 0x28) == 0x2000) {
        spi[0] &= 0xffffdfffu;
        spi[0] |= 0x2000u;
    }
    *(volatile uint8_t *)(h + 0x5d) = 1;
    *(volatile uint8_t *)(h + 0x5c) = 0;
    return 3;
}

/* SPI_EndRxTxTransaction (FUN_0801cdec): drain TX FIFO (FTLVL), wait !BSY,
 * drain RX FIFO (FRLVL). Returns 0 / 3 (flagging the error). */
static int spi_wait_fifo_idle(uint8_t *h, uint32_t timeout, uint32_t start)
{
    if (spi_wait_fifo_state(h, 0x1800, 0, timeout, start) == 0 &&
        spi_wait_flag_state(h, 0x80, 0, timeout, start) == 0 &&
        spi_wait_fifo_state(h, 0x600, 0, timeout, start) == 0) {
        return 0;
    }
    *(uint32_t *)(h + 0x60) |= 0x20;
    return 3;
}

/* SPI_CloseRxTx_ISR (FUN_0801ce78). */
static void spi_close_rxtx(uint8_t *h)
{
    volatile uint32_t *spi = *(volatile uint32_t **)(h + 0);
    uint32_t start = tick_get();

    spi[1] &= 0xffffffdfu;                                /* ERRIE off */
    if (spi_wait_fifo_idle(h, 100, start) != 0) {
        *(uint32_t *)(h + 0x60) |= 0x20;
    }
    if (*(uint32_t *)(h + 0x60) == 0) {
        if (*(volatile uint8_t *)(h + 0x5d) == 4) {
            *(volatile uint8_t *)(h + 0x5d) = 1;
            spi_rx_cplt_cb(h);
        } else {
            *(volatile uint8_t *)(h + 0x5d) = 1;
            spi_txrx_cplt_cb(h);
        }
    } else {
        *(volatile uint8_t *)(h + 0x5d) = 1;
        spi_error_cb(h);
    }
}

void spi_tx_isr_8bit(void *handle)
{
    uint8_t *h = handle;
    volatile uint32_t *dr = (volatile uint32_t *)(*(uint32_t *)(h + 0) + 0x0c);
    volatile uint16_t *txcnt = (volatile uint16_t *)(h + 0x3e);
    uint8_t *p = *(uint8_t **)(h + 0x38);

    if (*txcnt < 2) {
        *(volatile uint8_t *)dr = *p;
        *(uint8_t **)(h + 0x38) = p + 1;
        (*txcnt)--;
    } else {
        *dr = *(uint16_t *)p;
        *(uint8_t **)(h + 0x38) = p + 2;
        *txcnt = (uint16_t)(*txcnt - 2);
    }
    if (*txcnt == 0) {
        *(volatile uint32_t *)(*(uint32_t *)(h + 0) + 4) &= 0xffffff7fu;   /* TXEIE off */
        if (*(volatile uint16_t *)(h + 0x46) == 0) {
            spi_close_rxtx(h);
        }
    }
}

void spi_rx_isr_8bit(void *handle)
{
    uint8_t *h = handle;
    volatile uint32_t *cr2 = (volatile uint32_t *)(*(uint32_t *)(h + 0) + 4);
    volatile uint32_t *dr  = (volatile uint32_t *)(*(uint32_t *)(h + 0) + 0x0c);
    volatile uint16_t *rxcnt = (volatile uint16_t *)(h + 0x46);
    uint8_t *p = *(uint8_t **)(h + 0x40);

    if (*rxcnt < 2) {
        *p = *(volatile uint8_t *)dr;
        *(uint8_t **)(h + 0x40) = p + 1;
        (*rxcnt)--;
    } else {
        *(uint16_t *)p = (uint16_t)*dr;
        *(uint8_t **)(h + 0x40) = p + 2;
        *rxcnt = (uint16_t)(*rxcnt - 2);
        if (*rxcnt == 1) {
            *cr2 |= 0x1000u;                                              /* FRXTH */
        }
    }
    if (*rxcnt == 0) {
        *cr2 &= 0xffffff9fu;                                              /* RXNEIE|ERRIE off */
        if (*(volatile uint16_t *)(h + 0x3e) == 0) {
            spi_close_rxtx(h);
        }
    }
}

void spi_tx_isr_16bit(void *handle)
{
    uint8_t *h = handle;
    volatile uint32_t *dr = (volatile uint32_t *)(*(uint32_t *)(h + 0) + 0x0c);
    volatile uint16_t *txcnt = (volatile uint16_t *)(h + 0x3e);
    uint8_t *p = *(uint8_t **)(h + 0x38);

    *dr = *(uint16_t *)p;
    *(uint8_t **)(h + 0x38) = p + 2;
    (*txcnt)--;
    if (*txcnt == 0) {
        *(volatile uint32_t *)(*(uint32_t *)(h + 0) + 4) &= 0xffffff7fu;
        if (*(volatile uint16_t *)(h + 0x46) == 0) {
            spi_close_rxtx(h);
        }
    }
}

void spi_rx_isr_16bit(void *handle)
{
    uint8_t *h = handle;
    volatile uint32_t *dr = (volatile uint32_t *)(*(uint32_t *)(h + 0) + 0x0c);
    volatile uint16_t *rxcnt = (volatile uint16_t *)(h + 0x46);
    uint8_t *p = *(uint8_t **)(h + 0x40);

    *(uint16_t *)p = (uint16_t)*dr;
    *(uint8_t **)(h + 0x40) = p + 2;
    (*rxcnt)--;
    if (*rxcnt == 0) {
        *(volatile uint32_t *)(*(uint32_t *)(h + 0) + 4) &= 0xffffffbfu;   /* RXNEIE off */
        if (*(volatile uint16_t *)(h + 0x3e) == 0) {
            spi_close_rxtx(h);
        }
    }
}

extern int FUN_0801c700(void *hspi);   /* HAL_SPI_Init (own pass) */

/*
 * spi1_init — OEM FUN_08010d90. One of board_init's peripheral sub-inits.
 * Bring up the SPI1 master that drives the FEDL5236 AFE / Extend_IO: enable the
 * SPI1 (APB2) + GPIOB (AHB) clocks, put PB3/PB4/PB5 in AF0 (SCK/MISO/MOSI),
 * populate the master handle (8-bit, software NSS, /32) at 0x20000634, run
 * HAL_SPI_Init, enable the SPI1 IRQ (25), and prime the two 32-byte scratch
 * buffers to 0xFF. Handle field values disasm-confirmed against the OEM image.
 */
void spi1_init(void)
{
    volatile uint32_t * const RCC_AHBENR  = (volatile uint32_t *)(0x40021000u + 0x14);
    volatile uint32_t * const RCC_APB2ENR = (volatile uint32_t *)(0x40021000u + 0x18);
    uint32_t * const hspi = (uint32_t *)0x20000634u;

    gpio_pin_cfg_t cfg;
    mem_set(&cfg, 0, sizeof cfg);

    *RCC_APB2ENR |= 0x1000u;    (void)(*RCC_APB2ENR & 0x1000u);    /* SPI1EN */
    *RCC_AHBENR  |= 0x40000u;   (void)(*RCC_AHBENR  & 0x40000u);   /* IOPBEN */

    cfg.pin_mask = 0x38;   /* PB3, PB4, PB5 */
    cfg.mode     = GPIO_MODE_AF;
    cfg.pupd     = 0;
    cfg.speed    = 3;
    cfg.af       = 0;      /* AF0 = SPI1 */
    gpio_pin_config((uint32_t *)0x48000400u, &cfg);

    hspi[0]  = 0x40013000u;   /* Instance (SPI1)        */
    hspi[1]  = 0x104u;        /* Init.Mode (master)     */
    hspi[2]  = 0;             /* Init.Direction (2line) */
    hspi[3]  = 0x700u;        /* Init.DataSize (8-bit)  */
    hspi[4]  = 0;             /* Init.CLKPolarity       */
    hspi[5]  = 0;             /* Init.CLKPhase          */
    hspi[6]  = 0x200u;        /* Init.NSS (software)    */
    hspi[7]  = 0x20u;         /* Init.BaudRatePrescaler */
    hspi[8]  = 0;             /* Init.FirstBit (MSB)    */
    hspi[9]  = 0;             /* Init.TIMode            */
    hspi[10] = 0;             /* Init.CRCCalculation    */
    hspi[11] = 7;             /* Init.CRCPolynomial     */
    hspi[12] = 0;             /* Init.CRCLength         */
    hspi[13] = 0;             /* Init.NSSPMode          */
    *(volatile uint32_t *)0x20002614u = 0;

    if (FUN_0801c700(hspi) != 0) {
        spi_error_reset();
    }

    nvic_set_priority(0x19, 3, 0);   /* SPI1_IRQn */
    nvic_enable_irq(0x19);

    for (uint8_t i = 0; i < 0x20; i++) {
        *(volatile uint8_t *)(0x200005f4u + i) = 0xff;
        *(volatile uint8_t *)(0x20000614u + i) = 0xff;
    }
}
