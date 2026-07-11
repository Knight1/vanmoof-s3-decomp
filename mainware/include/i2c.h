#ifndef MAINWARE_I2C_H
#define MAINWARE_I2C_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * I2C HAL (CubeF4 stm32f4xx_hal_i2c.c, blocking master/memory transfers).
 *
 * The S3 main controller drives every on-board I2C peripheral (config EEPROM,
 * STC3115 gas-gauge, LIS3DH accelerometer, audio amp, display, LED driver,
 * light sensor) through this layer. The transfer engine, the flag-wait
 * helpers, and the address/memory request helpers were reconstructed from the
 * OEM functions at 0x08023EEC..0x08025170. Field offsets and the (reg<<16)|bit
 * I2C_FLAG_* encoding match the stock CubeF4 HAL exactly.
 *
 * The I2C3 handle lives in SRAM at 0x20009B04 (also reached as
 * g_eeprom_i2c_handle); i2c3_handle_init/_deinit bracket the SCL bit-bang
 * bus-recovery in clock_pulse_gpioa8_until_pc9.
 * ------------------------------------------------------------------------- */

/* STM32F4 I2C peripheral register block (Instance points here). */
typedef struct {
    volatile uint32_t CR1;     /* 0x00 */
    volatile uint32_t CR2;     /* 0x04 */
    volatile uint32_t OAR1;    /* 0x08 */
    volatile uint32_t OAR2;    /* 0x0C */
    volatile uint32_t DR;      /* 0x10 */
    volatile uint32_t SR1;     /* 0x14 */
    volatile uint32_t SR2;     /* 0x18 */
    volatile uint32_t CCR;     /* 0x1C */
    volatile uint32_t TRISE;   /* 0x20 */
    volatile uint32_t FLTR;    /* 0x24 */
} I2C_TypeDef;

typedef struct {
    uint32_t ClockSpeed;       /* 0x00 */
    uint32_t DutyCycle;        /* 0x04  0 = DUTYCYCLE_2, 0x4000 = DUTYCYCLE_16_9 */
    uint32_t OwnAddress1;      /* 0x08 */
    uint32_t AddressingMode;   /* 0x0C  0x4000 = 7-bit, 0xC000 = 10-bit */
    uint32_t DualAddressMode;  /* 0x10 */
    uint32_t OwnAddress2;      /* 0x14 */
    uint32_t GeneralCallMode;  /* 0x18 */
    uint32_t NoStretchMode;    /* 0x1C */
} I2C_InitTypeDef;

typedef struct {
    I2C_TypeDef       *Instance;       /* 0x00 */
    I2C_InitTypeDef    Init;           /* 0x04 */
    uint8_t           *pBuffPtr;       /* 0x24 */
    uint16_t           XferSize;       /* 0x28 */
    volatile uint16_t  XferCount;      /* 0x2A */
    volatile uint32_t  XferOptions;    /* 0x2C */
    volatile uint32_t  PreviousState;  /* 0x30 */
    void              *hdmatx;         /* 0x34 */
    void              *hdmarx;         /* 0x38 */
    volatile uint8_t   Lock;           /* 0x3C */
    volatile uint8_t   State;          /* 0x3D */
    volatile uint8_t   Mode;           /* 0x3E */
    volatile uint32_t  ErrorCode;      /* 0x40 */
    uint32_t           Devaddress;     /* 0x44 */
    uint32_t           Memaddress;     /* 0x48 */
    uint32_t           MemaddSize;     /* 0x4C */
    uint32_t           EventCount;     /* 0x50 */
} I2C_HandleTypeDef;

/* HAL_StatusTypeDef */
#define HAL_OK        0
#define HAL_ERROR     1
#define HAL_BUSY      2
#define HAL_TIMEOUT   3

/* De-init (abort/reset) the I2C3 peripheral handle. OEM 0x0803C8E4. */
void i2c3_handle_deinit(void);

/* Populate + HAL-init the I2C3 handle (Instance I2C3, 100 kHz, 7-bit). OEM 0x0803C660. */
void i2c3_handle_init(void);

/* Populate + HAL-init the general-purpose sensor I2C bus (OEM i2c2_init,
 * 0x0803C624 — Instance is actually I2C1 @0x40005400, 400 kHz, 7-bit). */
void i2c2_init(void);

/* De-init the general-purpose sensor I2C bus (Instance I2C1); pre-sleep teardown.
 * OEM i2c_handle_deinit, 0x0803C8D4. */
void i2c_handle_deinit(void);

/* CubeF4 HAL entry points (OEM addresses in i2c.c). 0 = HAL_OK. */
int HAL_I2C_Init(I2C_HandleTypeDef *hi2c);
int HAL_I2C_DeInit(I2C_HandleTypeDef *hi2c);
int HAL_I2C_Master_Transmit(I2C_HandleTypeDef *hi2c, uint16_t dev_addr,
                            uint8_t *data, uint16_t size, uint32_t timeout);
int HAL_I2C_Master_Receive(I2C_HandleTypeDef *hi2c, uint16_t dev_addr,
                           uint8_t *data, uint16_t size, uint32_t timeout);
int HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c, uint16_t dev_addr, uint16_t mem_addr,
                      uint16_t mem_addr_size, uint8_t *data, uint16_t size,
                      uint32_t timeout);
int HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c, uint16_t dev_addr, uint16_t mem_addr,
                     uint16_t mem_addr_size, uint8_t *data, uint16_t size,
                     uint32_t timeout);

/* I2C transfer-complete / error callbacks (the HAL fires these) + diagnostic
 * bus scan. `hi2c` is the I2C_HandleTypeDef. */
void i2c_tx_complete_callback(void *hi2c);   /* 0x0803D200 */
void i2c_error_callback(void *hi2c);         /* 0x0803D238 */
void i2c_bus_scan(void);                     /* 0x08043A3C (diagnostics_run_step) */

#endif
