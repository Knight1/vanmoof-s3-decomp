#include <stdint.h>

#include "i2c.h"

/* I2C3 HAL handle in SRAM @ 0x20009B04 (also reached as g_eeprom_i2c_handle). */
#define I2C3_HANDLE  ((volatile uint32_t *)0x20009B04u)

extern unsigned int HAL_I2C_DeInit(void *hi2c);        /* CubeF4 HAL, 0x0802472C */
extern int          HAL_I2C_Init(volatile uint32_t *hi2c); /* CubeF4 HAL, 0x08024570 */
extern void         i2c_init_error_trap(void);         /* 0x0803DDCC (no-return) */

/* De-init the I2C3 handle (OEM 0x0803C8E4) — a thunk forwarding the fixed handle
 * to HAL_I2C_DeInit; used before the SCL bit-bang in the bus-recovery path. */
void i2c3_handle_deinit(void)
{
    HAL_I2C_DeInit((void *)I2C3_HANDLE);
}

/* Populate + initialise the I2C3 handle (OEM 0x0803C660): Instance = I2C3
 * (0x40005C00), 100 kHz, 7-bit addressing, no dual-address/general-call/stretch.
 * Traps (never returns) on a HAL error. */
void i2c3_handle_init(void)
{
    volatile uint32_t *h = I2C3_HANDLE;

    h[0] = 0x40005C00u;   /* +0x00 Instance        = I2C3                    */
    h[1] = 100000u;       /* +0x04 ClockSpeed      = 100 kHz                 */
    h[2] = 0u;            /* +0x08 DutyCycle        = I2C_DUTYCYCLE_2         */
    h[3] = 0u;            /* +0x0C OwnAddress1      = 0                       */
    h[4] = 0x4000u;       /* +0x10 AddressingMode   = I2C_ADDRESSINGMODE_7BIT */
    h[5] = 0u;            /* +0x14 DualAddressMode  = disable                */
    h[6] = 0u;            /* +0x18 OwnAddress2      = 0                       */
    h[7] = 0u;            /* +0x1C GeneralCallMode  = disable                */
    h[8] = 0u;            /* +0x20 NoStretchMode    = disable                */

    if (HAL_I2C_Init(h) != 0) {
        i2c_init_error_trap();   /* never returns */
    }
}
