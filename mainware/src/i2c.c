#include <stdint.h>

#include "i2c.h"
#include "panic.h"
#include "systick.h"

/* I2C3 HAL handle in SRAM @ 0x20009B04 (also reached as g_eeprom_i2c_handle). */
#define I2C3_HANDLE  ((I2C_HandleTypeDef *)0x20009B04u)

/* --- CR1 / SR1 / SR2 register bits ------------------------------------- */
#define I2C_CR1_PE        0x0001u
#define I2C_CR1_START     0x0100u
#define I2C_CR1_STOP      0x0200u
#define I2C_CR1_ACK       0x0400u
#define I2C_CR1_POS       0x0800u
#define I2C_CR1_SWRST     0x8000u

#define I2C_SR1_BTF       0x0004u   /* byte transfer finished                */
#define I2C_SR1_STOPF     0x0010u   /* stop detected (slave)                 */
#define I2C_SR1_RXNE      0x0040u   /* data register not empty               */
#define I2C_SR1_TXE       0x0080u   /* data register empty                   */
#define I2C_SR1_AF        0x0400u   /* acknowledge failure                   */

/* HAL flag tokens: high half selects the status register (0x0001 = SR1,
 * 0x0010 = SR2), low half is the bit mask within it. */
#define I2C_FLAG_SB       0x00010001u
#define I2C_FLAG_ADDR     0x00010002u
#define I2C_FLAG_BTF      0x00010004u
#define I2C_FLAG_ADD10    0x00010008u
#define I2C_FLAG_BUSY     0x00100002u

/* HAL_I2C_StateTypeDef */
#define I2C_STATE_RESET    0x00u
#define I2C_STATE_READY    0x20u
#define I2C_STATE_BUSY     0x24u
#define I2C_STATE_BUSY_TX  0x21u
#define I2C_STATE_BUSY_RX  0x22u

/* HAL_I2C_ModeTypeDef */
#define I2C_MODE_NONE      0x00u
#define I2C_MODE_MASTER    0x10u
#define I2C_MODE_MEM       0x40u

/* HAL_I2C_ErrorCode bits */
#define I2C_ERROR_AF       0x04u
#define I2C_ERROR_TIMEOUT  0x20u
#define I2C_ERROR_WRONG_START  0x200u

/* Sequential-transfer XferOptions seen by the request helpers. */
#define I2C_FIRST_FRAME            0x00000001u
#define I2C_FIRST_AND_LAST_FRAME   0x00000008u
#define I2C_NO_OPTION_FRAME        0xFFFF0000u

/* Composite "previous state" values the request helpers test against. */
#define I2C_STATE_MASTER_BUSY_TX   0x11u
#define I2C_STATE_MASTER_BUSY_RX   0x12u

#define HAL_LOCKED         1u
#define HAL_UNLOCKED       0u
#define HAL_MAX_DELAY      0xFFFFFFFFu

#define I2C_TIMEOUT_BUSY   25u   /* ms, fixed bus-free wait at transfer start */

/* MSP + clock helpers live in other translation units. */
extern uint32_t HAL_RCC_GetPCLK1Freq(void);   /* 0x08027374 */
extern void     HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c);    /* 0x0803C69C */
extern void     HAL_I2C_MspDeInit(I2C_HandleTypeDef *hi2c);  /* 0x0803C820 */

/* ----------------------------- flag waits ------------------------------ */

/* Latch a master error and unwind to READY/UNLOCKED. */
static void i2c_abort(I2C_HandleTypeDef *hi2c, uint32_t error_bits)
{
    hi2c->PreviousState = 0;
    hi2c->State = I2C_STATE_READY;
    hi2c->Mode  = I2C_MODE_NONE;
    hi2c->ErrorCode |= error_bits;
    hi2c->Lock  = HAL_UNLOCKED;
}

/* 0 = the addressed slave NACKed (AF set, cleared here); else flag absent. OEM 0x08023EEC. */
static int I2C_IsAcknowledgeFailed(I2C_HandleTypeDef *hi2c)
{
    if ((hi2c->Instance->SR1 & I2C_SR1_AF) == 0u) {
        return HAL_OK;
    }
    hi2c->Instance->SR1 = 0xFFFFFBFFu;   /* clear AF */
    i2c_abort(hi2c, I2C_ERROR_AF);
    return HAL_ERROR;
}

/* Spin until `flag` leaves the `status` state (1 = wait while SET, 0 = wait
 * while RESET) or `timeout` ms elapse. OEM 0x08023F3C. */
static int I2C_WaitOnFlagUntilTimeout(I2C_HandleTypeDef *hi2c, uint32_t flag,
                                      uint32_t status, uint32_t timeout,
                                      uint32_t tickstart)
{
    for (;;) {
        uint32_t sr = (((flag & 0xFFFFFFu) >> 16) == 1u) ? hi2c->Instance->SR1
                                                          : hi2c->Instance->SR2;
        uint32_t is_set = ((flag & ~sr & 0xFFFFu) == 0u);

        if (status != is_set) {
            return HAL_OK;
        }
        if (timeout != HAL_MAX_DELAY) {
            if (timeout < (systick_now() - tickstart) || timeout == 0u) {
                i2c_abort(hi2c, I2C_ERROR_TIMEOUT);
                return HAL_ERROR;
            }
        }
    }
}

/* Wait for the master address phase to ACK; abort on NACK (AF). OEM 0x08023FB0. */
static int I2C_WaitOnMasterAddressFlagUntilTimeout(I2C_HandleTypeDef *hi2c,
                                                   uint32_t flag, uint32_t timeout,
                                                   uint32_t tickstart)
{
    for (;;) {
        uint32_t pending;
        if (((flag & 0xFFFFFFu) >> 16) == 1u) {
            pending = ((flag & ~hi2c->Instance->SR1 & 0xFFFFu) != 0u);
        } else {
            pending = ((flag & ~hi2c->Instance->SR2 & 0xFFFFu) != 0u);
        }
        if (!pending) {
            return HAL_OK;
        }
        if ((hi2c->Instance->SR1 & I2C_SR1_AF) != 0u) {
            hi2c->Instance->CR1 |= I2C_CR1_STOP;
            hi2c->Instance->SR1 = 0xFFFFFBFFu;   /* clear AF */
            i2c_abort(hi2c, I2C_ERROR_AF);
            return HAL_ERROR;
        }
        if (timeout != HAL_MAX_DELAY) {
            if ((systick_now() - tickstart) > timeout || timeout == 0u) {
                i2c_abort(hi2c, I2C_ERROR_TIMEOUT);
                return HAL_ERROR;
            }
        }
    }
}

/* Wait for TXE (data register empty), aborting on NACK. OEM 0x08024230. */
static int I2C_WaitOnTXEFlagUntilTimeout(I2C_HandleTypeDef *hi2c, uint32_t timeout,
                                         uint32_t tickstart)
{
    for (;;) {
        if ((hi2c->Instance->SR1 & I2C_SR1_TXE) != 0u) {
            return HAL_OK;
        }
        if (I2C_IsAcknowledgeFailed(hi2c) != HAL_OK) {
            return HAL_ERROR;
        }
        if (timeout != HAL_MAX_DELAY) {
            if (timeout < (systick_now() - tickstart) || timeout == 0u) {
                i2c_abort(hi2c, I2C_ERROR_TIMEOUT);
                return HAL_ERROR;
            }
        }
    }
}

/* Wait for BTF (byte transfer finished), aborting on NACK. OEM 0x080244B0. */
static int I2C_WaitOnBTFFlagUntilTimeout(I2C_HandleTypeDef *hi2c, uint32_t timeout,
                                         uint32_t tickstart)
{
    for (;;) {
        if ((hi2c->Instance->SR1 & I2C_SR1_BTF) != 0u) {
            return HAL_OK;
        }
        if (I2C_IsAcknowledgeFailed(hi2c) != HAL_OK) {
            return HAL_ERROR;
        }
        if (timeout != HAL_MAX_DELAY) {
            if (timeout < (systick_now() - tickstart) || timeout == 0u) {
                i2c_abort(hi2c, I2C_ERROR_TIMEOUT);
                return HAL_ERROR;
            }
        }
    }
}

/* Wait for RXNE; a STOPF instead means the slave terminated the frame. The
 * timeout here is unconditional (no HAL_MAX_DELAY escape). OEM 0x08024504. */
static int I2C_WaitOnRXNEFlagUntilTimeout(I2C_HandleTypeDef *hi2c, uint32_t timeout,
                                          uint32_t tickstart)
{
    for (;;) {
        if ((hi2c->Instance->SR1 & I2C_SR1_RXNE) != 0u) {
            return HAL_OK;
        }
        if ((hi2c->Instance->SR1 & I2C_SR1_STOPF) != 0u) {
            hi2c->Instance->SR1 = 0xFFFFFFEFu;   /* clear STOPF */
            hi2c->PreviousState = 0;
            hi2c->State = I2C_STATE_READY;
            hi2c->Mode  = I2C_MODE_NONE;
            hi2c->Lock  = HAL_UNLOCKED;
            return HAL_ERROR;
        }
        if (timeout < (systick_now() - tickstart) || timeout == 0u) {
            i2c_abort(hi2c, I2C_ERROR_TIMEOUT);
            return HAL_ERROR;
        }
    }
}

/* --------------------------- request helpers --------------------------- */

/* Generate START + send the slave address for a write. OEM 0x0802405C. */
static int I2C_MasterRequestWrite(I2C_HandleTypeDef *hi2c, uint32_t dev_addr,
                                  uint32_t timeout, uint32_t tickstart)
{
    uint32_t xo = hi2c->XferOptions;

    if (xo == I2C_FIRST_AND_LAST_FRAME || xo == I2C_FIRST_FRAME ||
        xo == I2C_NO_OPTION_FRAME) {
        hi2c->Instance->CR1 |= I2C_CR1_START;
    } else if (hi2c->PreviousState == I2C_STATE_MASTER_BUSY_RX) {
        hi2c->Instance->CR1 |= I2C_CR1_START;
    }

    if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_SB, 0, timeout, tickstart) != HAL_OK) {
        if ((hi2c->Instance->CR1 & I2C_CR1_START) != 0u) {
            hi2c->ErrorCode = I2C_ERROR_WRONG_START;
        }
        return HAL_TIMEOUT;
    }

    if (hi2c->Init.AddressingMode == 0x4000u) {        /* 7-bit */
        hi2c->Instance->DR = dev_addr & 0xFEu;
    } else {                                            /* 10-bit */
        hi2c->Instance->DR = (((int32_t)dev_addr >> 7) & 6u) | 0xF0u;
        if (I2C_WaitOnMasterAddressFlagUntilTimeout(hi2c, I2C_FLAG_ADD10, timeout,
                                                    tickstart) != HAL_OK) {
            return HAL_ERROR;
        }
        hi2c->Instance->DR = dev_addr & 0xFFu;
    }

    if (I2C_WaitOnMasterAddressFlagUntilTimeout(hi2c, I2C_FLAG_ADDR, timeout,
                                                tickstart) != HAL_OK) {
        return HAL_ERROR;
    }
    return HAL_OK;
}

/* Generate START + send the slave address for a read. OEM 0x08024110. */
static int I2C_MasterRequestRead(I2C_HandleTypeDef *hi2c, uint32_t dev_addr,
                                 uint32_t timeout, uint32_t tickstart)
{
    uint32_t xo = hi2c->XferOptions;

    hi2c->Instance->CR1 |= I2C_CR1_ACK;
    if (xo == I2C_FIRST_AND_LAST_FRAME || xo == I2C_FIRST_FRAME ||
        xo == I2C_NO_OPTION_FRAME) {
        hi2c->Instance->CR1 |= I2C_CR1_START;
    } else if (hi2c->PreviousState == I2C_STATE_MASTER_BUSY_TX) {
        hi2c->Instance->CR1 |= I2C_CR1_START;
    }

    if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_SB, 0, timeout, tickstart) != HAL_OK) {
        if ((hi2c->Instance->CR1 & I2C_CR1_START) != 0u) {
            hi2c->ErrorCode = I2C_ERROR_WRONG_START;
        }
        return HAL_TIMEOUT;
    }

    if (hi2c->Init.AddressingMode == 0x4000u) {        /* 7-bit */
        hi2c->Instance->DR = (dev_addr & 0xFFu) | 1u;
    } else {                                            /* 10-bit */
        uint32_t header = ((int32_t)dev_addr >> 7) & 6u;
        hi2c->Instance->DR = header | 0xF0u;
        if (I2C_WaitOnMasterAddressFlagUntilTimeout(hi2c, I2C_FLAG_ADD10, timeout,
                                                    tickstart) != HAL_OK) {
            return HAL_ERROR;
        }
        hi2c->Instance->DR = dev_addr & 0xFFu;
        if (I2C_WaitOnMasterAddressFlagUntilTimeout(hi2c, I2C_FLAG_ADDR, timeout,
                                                    tickstart) != HAL_OK) {
            return HAL_ERROR;
        }
        hi2c->Instance->CR1 |= I2C_CR1_START;           /* repeated start */
        if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_SB, 0, timeout,
                                       tickstart) != HAL_OK) {
            if ((hi2c->Instance->CR1 & I2C_CR1_START) != 0u) {
                hi2c->ErrorCode = I2C_ERROR_WRONG_START;
            }
            return HAL_TIMEOUT;
        }
        hi2c->Instance->DR = header | 0xF1u;            /* read header */
    }

    if (I2C_WaitOnMasterAddressFlagUntilTimeout(hi2c, I2C_FLAG_ADDR, timeout,
                                                tickstart) != HAL_OK) {
        return HAL_ERROR;
    }
    return HAL_OK;
}

/* START, slave write-address, then the 1- or 2-byte memory address. OEM 0x08024284. */
static int I2C_RequestMemoryWrite(I2C_HandleTypeDef *hi2c, uint32_t dev_addr,
                                  uint32_t mem_addr, uint32_t mem_addr_size,
                                  uint32_t timeout, uint32_t tickstart)
{
    hi2c->Instance->CR1 |= I2C_CR1_START;

    if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_SB, 0, timeout, tickstart) != HAL_OK) {
        if ((hi2c->Instance->CR1 & I2C_CR1_START) != 0u) {
            hi2c->ErrorCode = I2C_ERROR_WRONG_START;
        }
        return HAL_TIMEOUT;
    }

    hi2c->Instance->DR = dev_addr & 0xFEu;
    if (I2C_WaitOnMasterAddressFlagUntilTimeout(hi2c, I2C_FLAG_ADDR, timeout,
                                                tickstart) != HAL_OK) {
        return HAL_ERROR;
    }

    (void)hi2c->Instance->SR1;   /* clear ADDR: read SR1 then SR2 */
    (void)hi2c->Instance->SR2;
    if (I2C_WaitOnTXEFlagUntilTimeout(hi2c, timeout, tickstart) != HAL_OK) {
        if (hi2c->ErrorCode == I2C_ERROR_AF) {
            hi2c->Instance->CR1 |= I2C_CR1_STOP;
        }
        return HAL_ERROR;
    }

    if (mem_addr_size == 1u) {
        hi2c->Instance->DR = mem_addr & 0xFFu;
    } else {
        hi2c->Instance->DR = mem_addr >> 8;
        if (I2C_WaitOnTXEFlagUntilTimeout(hi2c, timeout, tickstart) != HAL_OK) {
            if (hi2c->ErrorCode == I2C_ERROR_AF) {
                hi2c->Instance->CR1 |= I2C_CR1_STOP;
            }
            return HAL_ERROR;
        }
        hi2c->Instance->DR = mem_addr & 0xFFu;
    }
    return HAL_OK;
}

/* START, slave write-address, memory address, repeated START, read-address. OEM 0x0802435C. */
static int I2C_RequestMemoryRead(I2C_HandleTypeDef *hi2c, uint32_t dev_addr,
                                 uint32_t mem_addr, uint32_t mem_addr_size,
                                 uint32_t timeout, uint32_t tickstart)
{
    hi2c->Instance->CR1 |= I2C_CR1_ACK;
    hi2c->Instance->CR1 |= I2C_CR1_START;

    if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_SB, 0, timeout, tickstart) != HAL_OK) {
        if ((hi2c->Instance->CR1 & I2C_CR1_START) != 0u) {
            hi2c->ErrorCode = I2C_ERROR_WRONG_START;
        }
        return HAL_TIMEOUT;
    }

    hi2c->Instance->DR = dev_addr & 0xFEu;
    if (I2C_WaitOnMasterAddressFlagUntilTimeout(hi2c, I2C_FLAG_ADDR, timeout,
                                                tickstart) != HAL_OK) {
        return HAL_ERROR;
    }

    (void)hi2c->Instance->SR1;   /* clear ADDR: read SR1 then SR2 */
    (void)hi2c->Instance->SR2;
    if (I2C_WaitOnTXEFlagUntilTimeout(hi2c, timeout, tickstart) != HAL_OK) {
        if (hi2c->ErrorCode == I2C_ERROR_AF) {
            hi2c->Instance->CR1 |= I2C_CR1_STOP;
        }
        return HAL_ERROR;
    }

    if (mem_addr_size == 1u) {
        hi2c->Instance->DR = mem_addr & 0xFFu;
    } else {
        hi2c->Instance->DR = mem_addr >> 8;
        if (I2C_WaitOnTXEFlagUntilTimeout(hi2c, timeout, tickstart) != HAL_OK) {
            if (hi2c->ErrorCode == I2C_ERROR_AF) {
                hi2c->Instance->CR1 |= I2C_CR1_STOP;
            }
            return HAL_ERROR;
        }
        hi2c->Instance->DR = mem_addr & 0xFFu;
    }

    if (I2C_WaitOnTXEFlagUntilTimeout(hi2c, timeout, tickstart) != HAL_OK) {
        if (hi2c->ErrorCode == I2C_ERROR_AF) {
            hi2c->Instance->CR1 |= I2C_CR1_STOP;
        }
        return HAL_ERROR;
    }

    hi2c->Instance->CR1 |= I2C_CR1_START;               /* repeated start */
    if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_SB, 0, timeout, tickstart) != HAL_OK) {
        if ((hi2c->Instance->CR1 & I2C_CR1_START) != 0u) {
            hi2c->ErrorCode = I2C_ERROR_WRONG_START;
        }
        return HAL_TIMEOUT;
    }

    hi2c->Instance->DR = (dev_addr & 0xFFu) | 1u;
    if (I2C_WaitOnMasterAddressFlagUntilTimeout(hi2c, I2C_FLAG_ADDR, timeout,
                                                tickstart) != HAL_OK) {
        return HAL_ERROR;
    }
    return HAL_OK;
}

/* ------------------------------ init/deinit ---------------------------- */

int HAL_I2C_Init(I2C_HandleTypeDef *hi2c)
{
    uint32_t pclk1;
    uint32_t freqrange;
    uint32_t trise;
    uint32_t ccr;
    uint32_t clockspeed;
    int bad_clock;

    if (hi2c == 0) {
        return HAL_ERROR;
    }

    if (hi2c->State == I2C_STATE_RESET) {
        hi2c->Lock = HAL_UNLOCKED;
        HAL_I2C_MspInit(hi2c);
    }

    hi2c->State = I2C_STATE_BUSY;
    hi2c->Instance->CR1 &= ~I2C_CR1_PE;
    hi2c->Instance->CR1 |= I2C_CR1_SWRST;
    hi2c->Instance->CR1 &= ~I2C_CR1_SWRST;

    pclk1 = HAL_RCC_GetPCLK1Freq();
    clockspeed = hi2c->Init.ClockSpeed;

    if (clockspeed > 100000u) {            /* fast mode needs PCLK1 >= 4 MHz  */
        bad_clock = (pclk1 <= 3999999u);
    } else {                               /* standard mode needs >= 2 MHz    */
        bad_clock = !(pclk1 > 1999999u);
    }
    if (bad_clock) {
        return HAL_ERROR;
    }

    freqrange = pclk1 / 1000000u;
    hi2c->Instance->CR2 = (hi2c->Instance->CR2 & ~0x3Fu) | freqrange;

    if (clockspeed > 100000u) {
        trise = (freqrange * 300u) / 1000u;
    } else {
        trise = freqrange;
    }
    hi2c->Instance->TRISE = (hi2c->Instance->TRISE & ~0x3Fu) | (trise + 1u);

    if (clockspeed > 100000u) {            /* fast-mode CCR */
        uint32_t div = (hi2c->Init.DutyCycle == 0u) ? (clockspeed * 3u)
                                                     : (clockspeed * 25u);
        uint32_t field = (pclk1 - 1u) / div;
        if (((field + 1u) & 0xFFFu) == 0u) {
            ccr = 1u;
        } else if (hi2c->Init.DutyCycle == 0u) {
            ccr = (((pclk1 - 1u) / (clockspeed * 3u) + 1u) & 0xFFFu) | 0x8000u;
        } else {
            ccr = (((pclk1 - 1u) / (clockspeed * 25u) + 1u) & 0xFFFu) | 0xC000u;
        }
    } else {                               /* standard-mode CCR */
        ccr = (pclk1 - 1u) / (clockspeed * 2u) + 1u;
        if ((ccr & 0xFFCu) == 0u) {
            ccr = 4u;
        } else {
            ccr &= 0xFFFu;
        }
    }
    hi2c->Instance->CCR = ccr | (hi2c->Instance->CCR & 0xFFFF3000u);

    hi2c->Instance->CR1 = (hi2c->Instance->CR1 & 0xFFFFFF3Fu) |
                          hi2c->Init.GeneralCallMode | hi2c->Init.NoStretchMode;
    hi2c->Instance->OAR1 = (hi2c->Instance->OAR1 & 0xFFFF7C00u) |
                           hi2c->Init.AddressingMode | hi2c->Init.OwnAddress1;
    hi2c->Instance->OAR2 = (hi2c->Instance->OAR2 & 0xFFFFFF00u) |
                           hi2c->Init.DualAddressMode | hi2c->Init.OwnAddress2;
    hi2c->Instance->CR1 |= I2C_CR1_PE;

    hi2c->ErrorCode = 0;
    hi2c->State = I2C_STATE_READY;
    hi2c->PreviousState = 0;
    hi2c->Mode = I2C_MODE_NONE;
    return HAL_OK;
}

int HAL_I2C_DeInit(I2C_HandleTypeDef *hi2c)
{
    if (hi2c == 0) {
        return HAL_ERROR;
    }
    hi2c->State = I2C_STATE_BUSY;
    hi2c->Instance->CR1 &= ~I2C_CR1_PE;
    HAL_I2C_MspDeInit(hi2c);
    hi2c->ErrorCode = 0;
    hi2c->State = I2C_STATE_RESET;
    hi2c->PreviousState = 0;
    hi2c->Mode = I2C_MODE_NONE;
    hi2c->Lock = HAL_UNLOCKED;
    return HAL_OK;
}

/* --------------------------- blocking transfers ------------------------ */

int HAL_I2C_Master_Transmit(I2C_HandleTypeDef *hi2c, uint16_t dev_addr,
                            uint8_t *data, uint16_t size, uint32_t timeout)
{
    uint32_t tickstart = systick_now();

    if (hi2c->State != I2C_STATE_READY) {
        return HAL_BUSY;
    }
    if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_BUSY, 1, I2C_TIMEOUT_BUSY,
                                   tickstart) != HAL_OK) {
        return HAL_BUSY;
    }
    if (hi2c->Lock == HAL_LOCKED) {
        return HAL_BUSY;
    }
    hi2c->Lock = HAL_LOCKED;

    if ((hi2c->Instance->CR1 & I2C_CR1_PE) == 0u) {
        hi2c->Instance->CR1 |= I2C_CR1_PE;
    }
    hi2c->Instance->CR1 &= ~I2C_CR1_POS;
    hi2c->State = I2C_STATE_BUSY_TX;
    hi2c->Mode = I2C_MODE_MASTER;
    hi2c->ErrorCode = 0;
    hi2c->pBuffPtr = data;
    hi2c->XferCount = size;
    hi2c->XferSize = hi2c->XferCount;
    hi2c->XferOptions = I2C_NO_OPTION_FRAME;

    if (I2C_MasterRequestWrite(hi2c, dev_addr, timeout, tickstart) != HAL_OK) {
        return HAL_ERROR;
    }

    (void)hi2c->Instance->SR1;   /* clear ADDR: read SR1 then SR2 (else SCL stays stretched) */
    (void)hi2c->Instance->SR2;

    for (;;) {
        if (hi2c->XferSize == 0u) {
            hi2c->Instance->CR1 |= I2C_CR1_STOP;
            hi2c->State = I2C_STATE_READY;
            hi2c->Mode = I2C_MODE_NONE;
            hi2c->Lock = HAL_UNLOCKED;
            return HAL_OK;
        }
        if (I2C_WaitOnTXEFlagUntilTimeout(hi2c, timeout, tickstart) != HAL_OK) {
            if (hi2c->ErrorCode == I2C_ERROR_AF) {
                hi2c->Instance->CR1 |= I2C_CR1_STOP;
            }
            return HAL_ERROR;
        }
        hi2c->Instance->DR = *hi2c->pBuffPtr++;
        hi2c->XferCount--;
        hi2c->XferSize--;
        if ((hi2c->Instance->SR1 & I2C_SR1_BTF) != 0u && hi2c->XferSize != 0u) {
            hi2c->Instance->DR = *hi2c->pBuffPtr++;
            hi2c->XferCount--;
            hi2c->XferSize--;
        }
        if (I2C_WaitOnBTFFlagUntilTimeout(hi2c, timeout, tickstart) != HAL_OK) {
            if (hi2c->ErrorCode == I2C_ERROR_AF) {
                hi2c->Instance->CR1 |= I2C_CR1_STOP;
            }
            return HAL_ERROR;
        }
    }
}

int HAL_I2C_Master_Receive(I2C_HandleTypeDef *hi2c, uint16_t dev_addr,
                           uint8_t *data, uint16_t size, uint32_t timeout)
{
    uint32_t tickstart = systick_now();

    if (hi2c->State != I2C_STATE_READY) {
        return HAL_BUSY;
    }
    if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_BUSY, 1, I2C_TIMEOUT_BUSY,
                                   tickstart) != HAL_OK) {
        return HAL_BUSY;
    }
    if (hi2c->Lock == HAL_LOCKED) {
        return HAL_BUSY;
    }
    hi2c->Lock = HAL_LOCKED;

    if ((hi2c->Instance->CR1 & I2C_CR1_PE) == 0u) {
        hi2c->Instance->CR1 |= I2C_CR1_PE;
    }
    hi2c->Instance->CR1 &= ~I2C_CR1_POS;
    hi2c->State = I2C_STATE_BUSY_RX;
    hi2c->Mode = I2C_MODE_MASTER;
    hi2c->ErrorCode = 0;
    hi2c->pBuffPtr = data;
    hi2c->XferCount = size;
    hi2c->XferSize = hi2c->XferCount;
    hi2c->XferOptions = I2C_NO_OPTION_FRAME;

    if (I2C_MasterRequestRead(hi2c, dev_addr, timeout, tickstart) != HAL_OK) {
        return HAL_ERROR;
    }

    /* Clear the ADDR flag (read SR1 then SR2) before the RX loop; without it SCL
     * stays stretched and RXNE/BTF never advance. The clear's position relative
     * to the ACK/POS/STOP writes is per-branch (CubeF4). */
    if (hi2c->XferSize == 0u) {
        (void)hi2c->Instance->SR1;
        (void)hi2c->Instance->SR2;
        hi2c->Instance->CR1 |= I2C_CR1_STOP;
    } else if (hi2c->XferSize == 1u) {
        hi2c->Instance->CR1 &= ~I2C_CR1_ACK;
        (void)hi2c->Instance->SR1;
        (void)hi2c->Instance->SR2;
        hi2c->Instance->CR1 |= I2C_CR1_STOP;
    } else if (hi2c->XferSize == 2u) {
        hi2c->Instance->CR1 &= ~I2C_CR1_ACK;
        hi2c->Instance->CR1 |= I2C_CR1_POS;
        (void)hi2c->Instance->SR1;
        (void)hi2c->Instance->SR2;
    } else {
        hi2c->Instance->CR1 |= I2C_CR1_ACK;
        (void)hi2c->Instance->SR1;
        (void)hi2c->Instance->SR2;
    }

    while (hi2c->XferSize != 0u) {
        if (hi2c->XferSize < 4u) {
            if (hi2c->XferSize == 1u) {
                if (I2C_WaitOnRXNEFlagUntilTimeout(hi2c, timeout, tickstart) != HAL_OK) {
                    return HAL_ERROR;
                }
                *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
                hi2c->XferSize--;
                hi2c->XferCount--;
            } else if (hi2c->XferSize == 2u) {
                if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_BTF, 0, timeout,
                                               tickstart) != HAL_OK) {
                    return HAL_ERROR;
                }
                hi2c->Instance->CR1 |= I2C_CR1_STOP;
                *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
                hi2c->XferSize--;
                hi2c->XferCount--;
                *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
                hi2c->XferSize--;
                hi2c->XferCount--;
            } else {   /* XferSize == 3 */
                if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_BTF, 0, timeout,
                                               tickstart) != HAL_OK) {
                    return HAL_ERROR;
                }
                hi2c->Instance->CR1 &= ~I2C_CR1_ACK;
                *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
                hi2c->XferSize--;
                hi2c->XferCount--;
                if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_BTF, 0, timeout,
                                               tickstart) != HAL_OK) {
                    return HAL_ERROR;
                }
                hi2c->Instance->CR1 |= I2C_CR1_STOP;
                *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
                hi2c->XferSize--;
                hi2c->XferCount--;
                *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
                hi2c->XferSize--;
                hi2c->XferCount--;
            }
        } else {   /* XferSize >= 4 */
            if (I2C_WaitOnRXNEFlagUntilTimeout(hi2c, timeout, tickstart) != HAL_OK) {
                return HAL_ERROR;
            }
            *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
            hi2c->XferSize--;
            hi2c->XferCount--;
            if ((hi2c->Instance->SR1 & I2C_SR1_BTF) != 0u) {
                *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
                hi2c->XferSize--;
                hi2c->XferCount--;
            }
        }
    }

    hi2c->State = I2C_STATE_READY;
    hi2c->Mode = I2C_MODE_NONE;
    hi2c->Lock = HAL_UNLOCKED;
    return HAL_OK;
}

int HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c, uint16_t dev_addr, uint16_t mem_addr,
                      uint16_t mem_addr_size, uint8_t *data, uint16_t size,
                      uint32_t timeout)
{
    uint32_t tickstart = systick_now();

    if (hi2c->State != I2C_STATE_READY) {
        return HAL_BUSY;
    }
    if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_BUSY, 1, I2C_TIMEOUT_BUSY,
                                   tickstart) != HAL_OK) {
        return HAL_BUSY;
    }
    if (hi2c->Lock == HAL_LOCKED) {
        return HAL_BUSY;
    }
    hi2c->Lock = HAL_LOCKED;

    if ((hi2c->Instance->CR1 & I2C_CR1_PE) == 0u) {
        hi2c->Instance->CR1 |= I2C_CR1_PE;
    }
    hi2c->Instance->CR1 &= ~I2C_CR1_POS;
    hi2c->State = I2C_STATE_BUSY_TX;
    hi2c->Mode = I2C_MODE_MEM;
    hi2c->ErrorCode = 0;
    hi2c->pBuffPtr = data;
    hi2c->XferCount = size;
    hi2c->XferSize = hi2c->XferCount;
    hi2c->XferOptions = I2C_NO_OPTION_FRAME;

    if (I2C_RequestMemoryWrite(hi2c, dev_addr, mem_addr, mem_addr_size, timeout,
                               tickstart) != HAL_OK) {
        return HAL_ERROR;
    }

    while (hi2c->XferSize != 0u) {
        if (I2C_WaitOnTXEFlagUntilTimeout(hi2c, timeout, tickstart) != HAL_OK) {
            if (hi2c->ErrorCode == I2C_ERROR_AF) {
                hi2c->Instance->CR1 |= I2C_CR1_STOP;
            }
            return HAL_ERROR;
        }
        hi2c->Instance->DR = *hi2c->pBuffPtr++;
        hi2c->XferSize--;
        hi2c->XferCount--;
        if ((hi2c->Instance->SR1 & I2C_SR1_BTF) != 0u && hi2c->XferSize != 0u) {
            hi2c->Instance->DR = *hi2c->pBuffPtr++;
            hi2c->XferSize--;
            hi2c->XferCount--;
        }
    }

    if (I2C_WaitOnBTFFlagUntilTimeout(hi2c, timeout, tickstart) != HAL_OK) {
        if (hi2c->ErrorCode == I2C_ERROR_AF) {
            hi2c->Instance->CR1 |= I2C_CR1_STOP;
        }
        return HAL_ERROR;
    }
    hi2c->Instance->CR1 |= I2C_CR1_STOP;
    hi2c->State = I2C_STATE_READY;
    hi2c->Mode = I2C_MODE_NONE;
    hi2c->Lock = HAL_UNLOCKED;
    return HAL_OK;
}

int HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c, uint16_t dev_addr, uint16_t mem_addr,
                     uint16_t mem_addr_size, uint8_t *data, uint16_t size,
                     uint32_t timeout)
{
    uint32_t tickstart = systick_now();

    if (hi2c->State != I2C_STATE_READY) {
        return HAL_BUSY;
    }
    if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_BUSY, 1, I2C_TIMEOUT_BUSY,
                                   tickstart) != HAL_OK) {
        return HAL_BUSY;
    }
    if (hi2c->Lock == HAL_LOCKED) {
        return HAL_BUSY;
    }
    hi2c->Lock = HAL_LOCKED;

    if ((hi2c->Instance->CR1 & I2C_CR1_PE) == 0u) {
        hi2c->Instance->CR1 |= I2C_CR1_PE;
    }
    hi2c->Instance->CR1 &= ~I2C_CR1_POS;
    hi2c->State = I2C_STATE_BUSY_RX;
    hi2c->Mode = I2C_MODE_MEM;
    hi2c->ErrorCode = 0;
    hi2c->pBuffPtr = data;
    hi2c->XferCount = size;
    hi2c->XferSize = hi2c->XferCount;
    hi2c->XferOptions = I2C_NO_OPTION_FRAME;

    if (I2C_RequestMemoryRead(hi2c, dev_addr, mem_addr, mem_addr_size, timeout,
                              tickstart) != HAL_OK) {
        return HAL_ERROR;
    }

    /* Clear the read-phase ADDR flag (read SR1 then SR2) before the RX loop;
     * without it SCL stays stretched and RXNE never asserts. The clear's
     * position relative to the ACK/POS/STOP writes is per-branch (CubeF4). */
    if (hi2c->XferSize == 0u) {
        (void)hi2c->Instance->SR1;
        (void)hi2c->Instance->SR2;
        hi2c->Instance->CR1 |= I2C_CR1_STOP;
    } else if (hi2c->XferSize == 1u) {
        hi2c->Instance->CR1 &= ~I2C_CR1_ACK;
        (void)hi2c->Instance->SR1;
        (void)hi2c->Instance->SR2;
        hi2c->Instance->CR1 |= I2C_CR1_STOP;
    } else if (hi2c->XferSize == 2u) {
        hi2c->Instance->CR1 &= ~I2C_CR1_ACK;
        hi2c->Instance->CR1 |= I2C_CR1_POS;
        (void)hi2c->Instance->SR1;
        (void)hi2c->Instance->SR2;
    } else {
        (void)hi2c->Instance->SR1;
        (void)hi2c->Instance->SR2;
    }

    while (hi2c->XferSize != 0u) {
        if (hi2c->XferSize < 4u) {
            if (hi2c->XferSize == 1u) {
                if (I2C_WaitOnRXNEFlagUntilTimeout(hi2c, timeout, tickstart) != HAL_OK) {
                    return HAL_ERROR;
                }
                *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
                hi2c->XferSize--;
                hi2c->XferCount--;
            } else if (hi2c->XferSize == 2u) {
                if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_BTF, 0, timeout,
                                               tickstart) != HAL_OK) {
                    return HAL_ERROR;
                }
                hi2c->Instance->CR1 |= I2C_CR1_STOP;
                *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
                hi2c->XferSize--;
                hi2c->XferCount--;
                *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
                hi2c->XferSize--;
                hi2c->XferCount--;
            } else {   /* XferSize == 3 */
                if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_BTF, 0, timeout,
                                               tickstart) != HAL_OK) {
                    return HAL_ERROR;
                }
                hi2c->Instance->CR1 &= ~I2C_CR1_ACK;
                *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
                hi2c->XferSize--;
                hi2c->XferCount--;
                if (I2C_WaitOnFlagUntilTimeout(hi2c, I2C_FLAG_BTF, 0, timeout,
                                               tickstart) != HAL_OK) {
                    return HAL_ERROR;
                }
                hi2c->Instance->CR1 |= I2C_CR1_STOP;
                *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
                hi2c->XferSize--;
                hi2c->XferCount--;
                *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
                hi2c->XferSize--;
                hi2c->XferCount--;
            }
        } else {   /* XferSize >= 4 */
            if (I2C_WaitOnRXNEFlagUntilTimeout(hi2c, timeout, tickstart) != HAL_OK) {
                return HAL_ERROR;
            }
            *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
            hi2c->XferSize--;
            hi2c->XferCount--;
            if ((hi2c->Instance->SR1 & I2C_SR1_BTF) != 0u) {
                *hi2c->pBuffPtr++ = (uint8_t)hi2c->Instance->DR;
                hi2c->XferSize--;
                hi2c->XferCount--;
            }
        }
    }

    hi2c->State = I2C_STATE_READY;
    hi2c->Mode = I2C_MODE_NONE;
    hi2c->Lock = HAL_UNLOCKED;
    return HAL_OK;
}

/* ----------------------------- handle setup ---------------------------- */

/* De-init the I2C3 handle (OEM 0x0803C8E4) — a thunk forwarding the fixed
 * handle to HAL_I2C_DeInit; used before the SCL bit-bang bus-recovery path. */
void i2c3_handle_deinit(void)
{
    HAL_I2C_DeInit(I2C3_HANDLE);
}

/* Populate + initialise the I2C3 handle (OEM 0x0803C660): Instance = I2C3
 * (0x40005C00), 100 kHz, 7-bit addressing, no dual-address/general-call/stretch.
 * Traps (never returns) on a HAL error. */
void i2c3_handle_init(void)
{
    I2C_HandleTypeDef *h = I2C3_HANDLE;

    h->Instance = (I2C_TypeDef *)0x40005C00u;   /* I2C3 */
    h->Init.ClockSpeed = 100000u;
    h->Init.DutyCycle = 0u;                       /* I2C_DUTYCYCLE_2          */
    h->Init.OwnAddress1 = 0u;
    h->Init.AddressingMode = 0x4000u;             /* I2C_ADDRESSINGMODE_7BIT  */
    h->Init.DualAddressMode = 0u;
    h->Init.OwnAddress2 = 0u;
    h->Init.GeneralCallMode = 0u;
    h->Init.NoStretchMode = 0u;

    if (HAL_I2C_Init(h) != HAL_OK) {
        Error_Handler();   /* never returns */
    }
}

/* ── I2C transfer/error callbacks + diagnostic bus scan (cluster) ─────────── */

#include "log.h"   /* g_log_func */

extern void display_request_recovery(void);
extern int  HAL_I2C_IsDeviceReady(void *h, uint16_t addr, uint32_t trials, uint32_t tmo); /* 0x08025174 */

/* I2C master/mem transfer-complete callback (OEM 0x0803D200): log which
 * controller finished (I2C3 is intentionally silent). */
void i2c_tx_complete_callback(void *hi2c)
{
    if (*(volatile uint32_t *)hi2c == 0x40005400u) {            /* I2C1 */
        g_log_func("I2C1 Data\r\n");
    } else if (*(volatile uint32_t *)hi2c != 0x40005c00u) {     /* not I2C3 */
        g_log_func("I2C? Data\r\n");
    }
}

/* I2C error callback (OEM 0x0803D238): log the controller; an I2C1 error also
 * kicks the display recovery path. */
void i2c_error_callback(void *hi2c)
{
    if (*(volatile uint32_t *)hi2c == 0x40005400u) {            /* I2C1 */
        g_log_func("I2C1 Error\r\n");
        display_request_recovery();
    } else if (*(volatile uint32_t *)hi2c == 0x40005c00u) {     /* I2C3 */
        g_log_func("I2C3 Error\r\n");
    } else {
        g_log_func("I2C? Error\r\n");
    }
}

/* Diagnostic I2C bus scan (OEM 0x08043A3C, run by diagnostics_run_step): probe
 * every even 8-bit address on the two buses and log the ones that ACK. */
void i2c_bus_scan(void)
{
    uint32_t a;

    for (a = 0; (int)a < 0xff; a += 2) {
        if (HAL_I2C_IsDeviceReady((void *)0x20009bb8u, (uint16_t)a, 1, 100) == 0) {
            g_log_func("I2C1 0x%02X %d\r\n", a, a);
        }
    }
    for (a = 0; (int)a < 0xff; a += 2) {
        if (HAL_I2C_IsDeviceReady((void *)0x20009b04u, (uint16_t)a, 1, 100) == 0) {
            g_log_func("I2C3 0x%02X %d\r\n", a, a);
        }
    }
}
