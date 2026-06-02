#include "powerbankware.h"

/*
 * STM32F0 I2C EEPROM driver — polling-mode HAL_I2C_Mem_Write / _Read and the
 * helper cluster they share. This is the BMS-record persistence backend used
 * by fedl5236_record_save (EEPROM device 0xa0). No batteryware analogue —
 * batteryware persists to memory-mapped flash; powerbank has a discrete I2C
 * EEPROM, so this is derived entirely from this image.
 *
 *   eeprom_mem_write  = FUN_0801a9cc   eeprom_mem_read   = FUN_0801ac4c
 *   i2c_request_write = FUN_0801aecc   i2c_request_read  = FUN_0801afa8
 *   i2c_transfer_cfg  = FUN_0801b2e8   i2c_wait_flag     = FUN_0801b0c4
 *   i2c_wait_txis     = FUN_0801b136   i2c_wait_stop     = FUN_0801b1b4
 *   i2c_ack_failed    = FUN_0801b22c   i2c_flush_txdr    = FUN_0801b080
 *
 * HAL_I2C_HandleTypeDef offsets used:
 *   +0x00 Instance  +0x24 pBuffPtr  +0x28 XferSize(u16)  +0x2a XferCount(u16)
 *   +0x34 XferOptions  +0x40 Lock  +0x41 State  +0x42 Mode  +0x44 ErrorCode
 * I2C registers (Instance): +0x00 CR1, +0x04 CR2, +0x18 ISR, +0x1c ICR,
 *   +0x24 RXDR, +0x28 TXDR.
 *
 * Return codes: 0=HAL_OK, 1=HAL_ERROR(AF), 2=HAL_BUSY, 3=HAL_TIMEOUT.
 * State: ' '(0x20)=READY, 0x21=BUSY_TX, 0x22=BUSY_RX, 0x24=BUSY.
 */

/* CR2 bit groups (STM32F0 I2Cv2). */
#define I2C_RELOAD_MODE   0x01000000u    /* RELOAD  (bit 24) */
#define I2C_AUTOEND_MODE  0x02000000u    /* AUTOEND (bit 25) */
#define I2C_GENERATE_START_WRITE 0x00002000u   /* START (bit 13)               */
#define I2C_GENERATE_START_READ  0x00002400u   /* START | RD_WRN (bit 13 | 10) */
#define I2C_CR2_CLEAR     0xfe00e800u    /* clear SADD/NBYTES/RELOAD/AUTOEND/RD_WRN/START/STOP */
#define I2C_TRANSFER_KEEP 0xfc009800u    /* preserve mask in TransferConfig    */

#define INSTANCE(h)  (*(volatile uint32_t **)((uint8_t *)(h) + 0))
#define I2C_CR2(h)   (INSTANCE(h)[1])    /* +0x04 */
#define I2C_ISR(h)   (INSTANCE(h)[6])    /* +0x18 */
#define I2C_ICR(h)   (INSTANCE(h)[7])    /* +0x1c */
#define I2C_RXDR(h)  (INSTANCE(h)[9])    /* +0x24 */
#define I2C_TXDR(h)  (INSTANCE(h)[10])   /* +0x28 */

#define H_PBUF(h)    (*(uint8_t **)((uint8_t *)(h) + 0x24))
#define H_XSIZE(h)   (*(volatile uint16_t *)((uint8_t *)(h) + 0x28))
#define H_XCOUNT(h)  (*(volatile uint16_t *)((uint8_t *)(h) + 0x2a))
#define H_LOCK(h)    (*(volatile uint8_t  *)((uint8_t *)(h) + 0x40))
#define H_STATE(h)   (*(volatile uint8_t  *)((uint8_t *)(h) + 0x41))
#define H_MODE(h)    (*(volatile uint8_t  *)((uint8_t *)(h) + 0x42))
#define H_ERR(h)     (*(volatile uint32_t *)((uint8_t *)(h) + 0x44))

static void i2c_reset_state(uint8_t *h)
{
    H_STATE(h) = 0x20;
    H_MODE(h)  = 0;
    H_LOCK(h)  = 0;
}

/* FUN_0801b2e8 — load CR2 with the slave address, byte count, mode and request. */
static void i2c_transfer_cfg(uint8_t *h, uint16_t dev, uint8_t nbytes,
                             uint32_t mode, uint32_t request)
{
    I2C_CR2(h) = request | (dev & 0x3ff) | ((uint32_t)nbytes << 16) | mode |
                 (I2C_CR2(h) & I2C_TRANSFER_KEEP);
}

/* FUN_0801b080 — flush a stale TXDR and clear TXE. */
static void i2c_flush_txdr(uint8_t *h)
{
    if ((I2C_ISR(h) & 2) == 2) {
        I2C_TXDR(h) = 0;
    }
    if ((I2C_ISR(h) & 1) != 1) {
        I2C_ISR(h) |= 1u;
    }
}

/* FUN_0801b22c — detect an address/data NACK; on STOPF, clear and fault. */
static int i2c_ack_failed(uint8_t *h, uint32_t timeout, uint32_t start)
{
    if ((I2C_ISR(h) & 0x10) != 0x10) {
        return 0;
    }
    do {
        if ((I2C_ISR(h) & 0x20) == 0x20) {
            I2C_ICR(h) = 0x10;                 /* NACKCF */
            I2C_ICR(h) = 0x20;                 /* STOPCF */
            i2c_flush_txdr(h);
            I2C_CR2(h) &= I2C_CR2_CLEAR;
            H_ERR(h) = 4;
            i2c_reset_state(h);
            return 1;
        }
    } while (timeout == 0xffffffffu ||
             (timeout != 0 && (uint32_t)(tick_get() - start) <= timeout));
    i2c_reset_state(h);
    return 3;
}

/* FUN_0801b0c4 — wait until ISR flag `mask` differs from `status`. */
static int i2c_wait_flag(uint8_t *h, uint32_t mask, int status,
                         uint32_t timeout, uint32_t start)
{
    for (;;) {
        if ((int)((I2C_ISR(h) & mask) == mask) != status) {
            return 0;
        }
        if (!(timeout == 0xffffffffu ||
              (timeout != 0 && (uint32_t)(tick_get() - start) <= timeout))) {
            break;
        }
    }
    i2c_reset_state(h);
    return 3;
}

/* FUN_0801b136 — wait for TXIS, aborting on NACK. */
static int i2c_wait_txis(uint8_t *h, uint32_t timeout, uint32_t start)
{
    while ((I2C_ISR(h) & 2) != 2) {
        if (i2c_ack_failed(h, timeout, start) != 0) {
            return 1;
        }
        if (timeout != 0xffffffffu &&
            (timeout == 0 || timeout < (uint32_t)(tick_get() - start))) {
            H_ERR(h) |= 0x20;
            i2c_reset_state(h);
            return 3;
        }
    }
    return 0;
}

/* FUN_0801b1b4 — wait for STOPF, aborting on NACK. */
static int i2c_wait_stop(uint8_t *h, uint32_t timeout, uint32_t start)
{
    while ((I2C_ISR(h) & 0x20) != 0x20) {
        if (i2c_ack_failed(h, timeout, start) != 0) {
            return 1;
        }
        if (timeout == 0 || timeout < (uint32_t)(tick_get() - start)) {
            H_ERR(h) |= 0x20;
            i2c_reset_state(h);
            return 3;
        }
    }
    return 0;
}

/* FUN_0801aecc — address the EEPROM word for a write (RELOAD + START). */
static int i2c_request_write(uint8_t *h, uint16_t dev, uint16_t addr,
                             int16_t addrsz, uint32_t timeout, uint32_t start)
{
    i2c_transfer_cfg(h, dev, (uint8_t)addrsz, I2C_RELOAD_MODE, I2C_GENERATE_START_WRITE);
    if (i2c_wait_txis(h, timeout, start) != 0) {
        return (H_ERR(h) == 4) ? 1 : 3;
    }
    if (addrsz == 1) {
        I2C_TXDR(h) = (uint8_t)addr;
    } else {
        I2C_TXDR(h) = (uint8_t)(addr >> 8);
        if (i2c_wait_txis(h, timeout, start) != 0) {
            return (H_ERR(h) == 4) ? 1 : 3;
        }
        I2C_TXDR(h) = (uint8_t)addr;
    }
    return (i2c_wait_flag(h, 0x80, 0, timeout, start) == 0) ? 0 : 3;   /* TCR */
}

/* FUN_0801afa8 — address the EEPROM word for a read (no reload, START). */
static int i2c_request_read(uint8_t *h, uint16_t dev, uint16_t addr,
                            int16_t addrsz, uint32_t timeout, uint32_t start)
{
    i2c_transfer_cfg(h, dev, (uint8_t)addrsz, 0, I2C_GENERATE_START_WRITE);
    if (i2c_wait_txis(h, timeout, start) != 0) {
        return (H_ERR(h) == 4) ? 1 : 3;
    }
    if (addrsz == 1) {
        I2C_TXDR(h) = (uint8_t)addr;
    } else {
        I2C_TXDR(h) = (uint8_t)(addr >> 8);
        if (i2c_wait_txis(h, timeout, start) != 0) {
            return (H_ERR(h) == 4) ? 1 : 3;
        }
        I2C_TXDR(h) = (uint8_t)addr;
    }
    return (i2c_wait_flag(h, 0x40, 0, timeout, start) == 0) ? 0 : 3;   /* TC */
}

/* HAL_I2C_Mem_Write (FUN_0801a9cc). */
int eeprom_mem_write(void *handle, uint8_t dev, uint16_t addr, uint16_t addrsz,
                     void *buf, uint16_t len, uint32_t timeout)
{
    uint8_t *h = handle;

    if (H_STATE(h) != ' ') {
        return 2;
    }
    if (buf == 0 || len == 0) {
        return 1;
    }
    if (H_LOCK(h) == 1) {
        return 2;
    }
    H_LOCK(h) = 1;

    uint32_t start = tick_get();
    if (i2c_wait_flag(h, 0x8000, 1, 0x19, start) != 0) {     /* wait !BUSY (25 ms) */
        return 3;
    }
    H_STATE(h) = 0x21;
    H_MODE(h)  = 0x40;
    H_ERR(h)   = 0;
    H_PBUF(h)  = buf;
    H_XCOUNT(h) = len;
    *(volatile uint32_t *)(h + 0x34) = 0;

    if (i2c_request_write(h, dev, addr, (int16_t)addrsz, timeout, start) != 0) {
        H_LOCK(h) = 0;
        return (H_ERR(h) == 4) ? 1 : 3;
    }

    if (H_XCOUNT(h) < 0x100) {
        H_XSIZE(h) = H_XCOUNT(h);
        i2c_transfer_cfg(h, dev, (uint8_t)H_XSIZE(h), I2C_AUTOEND_MODE, 0);
    } else {
        H_XSIZE(h) = 0xff;
        i2c_transfer_cfg(h, dev, 0xff, I2C_RELOAD_MODE, 0);
    }

    do {
        if (i2c_wait_txis(h, timeout, start) != 0) {
            return (H_ERR(h) == 4) ? 1 : 3;
        }
        I2C_TXDR(h) = *H_PBUF(h);
        H_PBUF(h)++;
        H_XCOUNT(h)--;
        H_XSIZE(h)--;
        if (H_XSIZE(h) == 0 && H_XCOUNT(h) != 0) {
            if (i2c_wait_flag(h, 0x80, 0, timeout, start) != 0) {   /* TCR */
                return 3;
            }
            if (H_XCOUNT(h) < 0x100) {
                H_XSIZE(h) = H_XCOUNT(h);
                i2c_transfer_cfg(h, dev, (uint8_t)H_XSIZE(h), I2C_AUTOEND_MODE, 0);
            } else {
                H_XSIZE(h) = 0xff;
                i2c_transfer_cfg(h, dev, 0xff, I2C_RELOAD_MODE, 0);
            }
        }
    } while (H_XCOUNT(h) != 0);

    if (i2c_wait_stop(h, timeout, start) != 0) {
        return (H_ERR(h) == 4) ? 1 : 3;
    }
    I2C_ICR(h) = 0x20;                       /* STOPCF */
    I2C_CR2(h) &= I2C_CR2_CLEAR;
    i2c_reset_state(h);
    return 0;
}

/* HAL_I2C_Mem_Read (FUN_0801ac4c). */
int eeprom_mem_read(void *handle, uint8_t dev, uint16_t addr, uint16_t addrsz,
                    void *buf, uint16_t len, uint32_t timeout)
{
    uint8_t *h = handle;

    if (H_STATE(h) != ' ') {
        return 2;
    }
    if (buf == 0 || len == 0) {
        return 1;
    }
    if (H_LOCK(h) == 1) {
        return 2;
    }
    H_LOCK(h) = 1;

    uint32_t start = tick_get();
    if (i2c_wait_flag(h, 0x8000, 1, 0x19, start) != 0) {
        return 3;
    }
    H_STATE(h) = 0x22;
    H_MODE(h)  = 0x40;
    H_ERR(h)   = 0;
    H_PBUF(h)  = buf;
    H_XCOUNT(h) = len;
    *(volatile uint32_t *)(h + 0x34) = 0;

    if (i2c_request_read(h, dev, addr, (int16_t)addrsz, timeout, start) != 0) {
        H_LOCK(h) = 0;
        return (H_ERR(h) == 4) ? 1 : 3;
    }

    if (H_XCOUNT(h) < 0x100) {
        H_XSIZE(h) = H_XCOUNT(h);
        i2c_transfer_cfg(h, dev, (uint8_t)H_XSIZE(h), I2C_AUTOEND_MODE, I2C_GENERATE_START_READ);
    } else {
        H_XSIZE(h) = 0xff;
        i2c_transfer_cfg(h, dev, 0xff, I2C_RELOAD_MODE, I2C_GENERATE_START_READ);
    }

    do {
        if (i2c_wait_flag(h, 4, 0, timeout, start) != 0) {      /* RXNE */
            return 3;
        }
        *H_PBUF(h) = (uint8_t)I2C_RXDR(h);
        H_PBUF(h)++;
        H_XSIZE(h)--;
        H_XCOUNT(h)--;
        if (H_XSIZE(h) == 0 && H_XCOUNT(h) != 0) {
            if (i2c_wait_flag(h, 0x80, 0, timeout, start) != 0) {   /* TCR */
                return 3;
            }
            if (H_XCOUNT(h) < 0x100) {
                H_XSIZE(h) = H_XCOUNT(h);
                i2c_transfer_cfg(h, dev, (uint8_t)H_XSIZE(h), I2C_AUTOEND_MODE, 0);
            } else {
                H_XSIZE(h) = 0xff;
                i2c_transfer_cfg(h, dev, 0xff, I2C_RELOAD_MODE, 0);
            }
        }
    } while (H_XCOUNT(h) != 0);

    if (i2c_wait_stop(h, timeout, start) != 0) {
        return (H_ERR(h) == 4) ? 1 : 3;
    }
    I2C_ICR(h) = 0x20;                       /* STOPCF */
    I2C_CR2(h) &= I2C_CR2_CLEAR;
    i2c_reset_state(h);
    return 0;
}

extern int FUN_0801a890(void *hi2c);             /* HAL_I2C_Init                  */
extern int FUN_0801b354(void *hi2c, uint32_t f); /* HAL_I2CEx_ConfigAnalogFilter  */
extern int FUN_0801b3ec(void *hi2c, int f);      /* HAL_I2CEx_ConfigDigitalFilter */

/*
 * i2c2_init — OEM FUN_0800e32c. board_init's final peripheral sub-init.
 * Bring up the EEPROM I2C2 bus: enable GPIOB (AHB) + I2C2 (APB1) clocks, put
 * PB13/PB14 in AF5 open-drain with pull-ups (SCL/SDA), populate the I2C2 handle
 * (7-bit addressing, Timing 0x00300f38) at 0x20000434, run HAL_I2C_Init, then
 * enable the analog filter and a zero-stage digital filter. Disasm-confirmed.
 */
void i2c2_init(void)
{
    volatile uint32_t * const RCC_AHBENR  = (volatile uint32_t *)(0x40021000u + 0x14);
    volatile uint32_t * const RCC_APB1ENR = (volatile uint32_t *)(0x40021000u + 0x1c);
    uint32_t * const hi2c = (uint32_t *)0x20000434u;

    gpio_pin_cfg_t cfg;
    mem_set(&cfg, 0, sizeof cfg);

    *RCC_AHBENR  |= 0x40000u;    (void)(*RCC_AHBENR  & 0x40000u);    /* IOPBEN */
    *RCC_APB1ENR |= 0x400000u;   (void)(*RCC_APB1ENR & 0x400000u);   /* I2C2EN */

    cfg.pin_mask = 0x6000;   /* PB13, PB14                   */
    cfg.mode     = GPIO_MODE_AF | GPIO_OTYPE_OD;
    cfg.pupd     = GPIO_PUPD_UP;
    cfg.speed    = 3;
    cfg.af       = 5;        /* AF5 = I2C2 */
    gpio_pin_config((uint32_t *)0x48000400u, &cfg);

    hi2c[0] = 0x40005800u;   /* Instance (I2C2)       */
    hi2c[1] = 0x00300f38u;   /* Init.Timing (TIMINGR) */
    hi2c[2] = 0;             /* Init.OwnAddress1      */
    hi2c[3] = 1;             /* Init.AddressingMode   */
    hi2c[4] = 0;             /* Init.DualAddressMode  */
    hi2c[5] = 0;             /* Init.OwnAddress2      */
    hi2c[6] = 0;             /* Init.OwnAddress2Masks */
    hi2c[7] = 0;             /* Init.GeneralCallMode  */
    hi2c[8] = 0;             /* Init.NoStretchMode    */
    *(volatile uint32_t *)0x20002614u = 0;

    if (FUN_0801a890(hi2c) != 0)    { spi_error_reset(); }
    if (FUN_0801b354(hi2c, 0) != 0) { spi_error_reset(); }   /* analog filter on   */
    if (FUN_0801b3ec(hi2c, 0) != 0) { spi_error_reset(); }   /* digital filter = 0 */
}
