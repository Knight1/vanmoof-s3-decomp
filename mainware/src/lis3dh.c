#include <stdint.h>

#include "lis3dh.h"
#include "log.h"             /* g_log_func, log_print_timestamp_prefix */
#include "systick.h"         /* systick_delay */
#include "watchdog.h"        /* watchdog_kick */
#include "stm32f413_gpio.h"  /* GPIOC_BASE */

/* ===========================================================================
 * ST LIS3DH 3-axis accelerometer driver.
 *
 * The chip hangs off I2C3 (8-bit address 0x33 = 7-bit 0x19, SA0 high). Every
 * register access goes through a tiny per-device vtable so the higher-level
 * config code is transport-agnostic: { write, read, wait } at struct offsets
 * +0x00 / +0x04 / +0x08, installed by lis3dh_accel_init from the I2C-transport
 * leaves below. The HAL mem-transfers are blocking, so the "wait" slot is a
 * no-op that always reports success.
 *
 * Register-config helpers share one shape: read the register, wait for the
 * bus, modify the field, write it back; status 0 = OK, 3 = bus wait failed.
 * Register sub-addresses are OR'd with 0x80 to set the LIS3DH auto-increment
 * bit (harmless for the single-byte transfers used here).
 * ======================================================================== */

/* Device handle: a vtable plus a one-byte scratch the WHO_AM_I read lands in.
 * The OEM instance lives at SRAM 0x2000838C. */
typedef struct lis3dh_dev {
    int (*write)(uint8_t reg, const uint8_t *buf, uint16_t len);  /* +0x00 */
    int (*read )(uint8_t reg, uint8_t *buf, uint16_t len);        /* +0x04 */
    int (*wait )(uint32_t timeout);                               /* +0x08 */
    uint8_t whoami;                                               /* +0x0C */
} lis3dh_dev_t;

static lis3dh_dev_t g_lis3dh_dev;   /* OEM SRAM 0x2000838C */

/* Last INT1_SRC value seen by lis3dh_int1_clear; a change is logged once.
 * OEM byte at SRAM 0x200001E0. */
static uint8_t g_lis3dh_int1_last_src;

/* LIS3DH register sub-addresses (datasheet). */
#define LIS3DH_WHO_AM_I        0x0F
#define LIS3DH_CTRL_REG1       0x20   /* [7:4]=ODR [3]=LPen [2:0]=Z/Y/X enable */
#define LIS3DH_CTRL_REG2       0x21   /* [3]=FDS [2:0]=HPCLICK/HPIS2/HPIS1     */
#define LIS3DH_CTRL_REG3       0x22   /* INT1 source routing (I1_IA1 = bit6)   */
#define LIS3DH_CTRL_REG4       0x23   /* [5:4]=FS [3]=HR                       */
#define LIS3DH_CTRL_REG5       0x24   /* [3]=LIR_INT1                          */
#define LIS3DH_REFERENCE       0x26   /* reading it clears the HP filter       */
#define LIS3DH_INT1_CFG        0x30
#define LIS3DH_INT1_SRC        0x31
#define LIS3DH_INT1_THS        0x32   /* [6:0] threshold                       */
#define LIS3DH_INT1_DURATION   0x33   /* [6:0] duration                        */

#define LIS3DH_AUTOINC         0x80   /* sub-address auto-increment bit        */
#define LIS3DH_I2C_ADDR_8BIT   0x33   /* 7-bit 0x19, SA0 = 1                   */
#define LIS3DH_I2C_TIMEOUT_MS  0x32   /* 50 ms HAL blocking timeout            */

/* Shared CubeF4 I2C3 HAL handle (SRAM 0x20009B04 — the EEPROM uses it too). */
#define LIS3DH_I2C_HANDLE  ((void *)0x20009B04u)

/* INT1 line: GPIOC, IDR bit-3 mask (the OEM passes the literal 8). */
#define LIS3DH_INT1_GPIO   ((void *)GPIOC_BASE)
#define LIS3DH_INT1_PIN    0x08u

/* --- platform / vendor-stock externs --- */
extern int  HAL_I2C_Mem_Read (void *hi2c, uint16_t addr, uint16_t mem,
                              uint16_t memsize, uint8_t *data, uint16_t size,
                              uint32_t timeout);                  /* 0x08024E90 */
extern int  HAL_I2C_Mem_Write(void *hi2c, uint16_t addr, uint16_t mem,
                              uint16_t memsize, uint8_t *data, uint16_t size,
                              uint32_t timeout);                  /* 0x08024D2C */
extern int  HAL_GPIO_ReadPin(void *GPIOx, uint16_t pin_mask);     /* 0x08026AB8 */
extern void NVIC_DisableIRQ(int IRQn);                            /* 0x080270FC */

/* ----------------------------- I2C transport ---------------------------- */

/* dev->wait leaf: the HAL transfers are blocking, so there is nothing to poll. */
static int lis3dh_i2c_wait(uint32_t timeout)
{
    (void)timeout;
    return 0;
}

/* dev->read leaf: blocking HAL mem-read; returns the HAL status verbatim. */
static int lis3dh_i2c_read(uint8_t reg, uint8_t *buf, uint16_t len)
{
    return HAL_I2C_Mem_Read(LIS3DH_I2C_HANDLE, LIS3DH_I2C_ADDR_8BIT,
                            (uint16_t)(reg | LIS3DH_AUTOINC), 1,
                            buf, len, LIS3DH_I2C_TIMEOUT_MS);
}

/* dev->write leaf: blocking HAL mem-write; returns the HAL status verbatim. */
static int lis3dh_i2c_write(uint8_t reg, const uint8_t *buf, uint16_t len)
{
    return HAL_I2C_Mem_Write(LIS3DH_I2C_HANDLE, LIS3DH_I2C_ADDR_8BIT,
                             (uint16_t)(reg | LIS3DH_AUTOINC), 1,
                             (uint8_t *)buf, len, LIS3DH_I2C_TIMEOUT_MS);
}

/* --- vtable thunks (forward to the installed transport, propagate status) - */

static int lis3dh_read(lis3dh_dev_t *dev, uint8_t reg, uint8_t *buf, uint16_t len)
{
    return dev->read(reg, buf, len);
}

static int lis3dh_wait(lis3dh_dev_t *dev, uint32_t timeout)
{
    return dev->wait(timeout);
}

static int lis3dh_write(lis3dh_dev_t *dev, uint8_t reg, const uint8_t *buf, uint16_t len)
{
    return dev->write(reg, buf, len);
}

/* --------------------------- register helpers --------------------------- */

/* LPen (CTRL_REG1[3]) + HR (CTRL_REG4[3]) select the operating mode:
 *   0 = high-resolution (LPen=0, HR=1), 1 = normal (0,0), 2 = low-power (1,0). */
static int lis3dh_set_power_mode(lis3dh_dev_t *dev, int mode)
{
    uint8_t reg1;
    uint8_t reg4;
    int rc;

    rc = lis3dh_read(dev, LIS3DH_CTRL_REG1, &reg1, 1);
    if (lis3dh_wait(dev, LIS3DH_I2C_TIMEOUT_MS) != 0) {
        return 3;
    }
    if (rc == 0) {
        rc = lis3dh_read(dev, LIS3DH_CTRL_REG4, &reg4, 1);
        if (lis3dh_wait(dev, LIS3DH_I2C_TIMEOUT_MS) != 0) {
            return 3;
        }
    }
    if (rc == 0) {
        if (mode == 0) { reg1 &= 0xF7u; reg4 |= 0x08u; }
        if (mode == 1) { reg1 &= 0xF7u; reg4 &= 0xF7u; }
        if (mode == 2) { reg1 |= 0x08u; reg4 &= 0xF7u; }
        rc = lis3dh_write(dev, LIS3DH_CTRL_REG1, &reg1, 1);
    }
    if (rc == 0) {
        rc = lis3dh_write(dev, LIS3DH_CTRL_REG4, &reg4, 1);
    }
    return rc;
}

/* Output data rate: CTRL_REG1[7:4] = odr (0 = power-down). */
static int lis3dh_set_odr(lis3dh_dev_t *dev, uint8_t odr)
{
    uint8_t reg1;
    int rc = lis3dh_read(dev, LIS3DH_CTRL_REG1, &reg1, 1);
    if (lis3dh_wait(dev, LIS3DH_I2C_TIMEOUT_MS) != 0) {
        return 3;
    }
    if (rc == 0) {
        reg1 = (uint8_t)((reg1 & 0x0Fu) | (uint8_t)(odr << 4));
        rc = lis3dh_write(dev, LIS3DH_CTRL_REG1, &reg1, 1);
    }
    return rc;
}

/* Filtered-data selection: CTRL_REG2[3] = FDS. */
static int lis3dh_set_filtered_data(lis3dh_dev_t *dev, uint8_t enable)
{
    uint8_t reg2;
    int rc = lis3dh_read(dev, LIS3DH_CTRL_REG2, &reg2, 1);
    if (lis3dh_wait(dev, LIS3DH_I2C_TIMEOUT_MS) != 0) {
        return 3;
    }
    if (rc == 0) {
        reg2 = (uint8_t)((reg2 & 0xF7u) | (uint8_t)((enable & 1u) << 3));
        rc = lis3dh_write(dev, LIS3DH_CTRL_REG2, &reg2, 1);
    }
    return rc;
}

/* Full-scale: CTRL_REG4[5:4] = fs (0=2g, 1=4g, 2=8g, 3=16g). */
static int lis3dh_set_full_scale(lis3dh_dev_t *dev, uint8_t fs)
{
    uint8_t reg4;
    int rc = lis3dh_read(dev, LIS3DH_CTRL_REG4, &reg4, 1);
    if (lis3dh_wait(dev, LIS3DH_I2C_TIMEOUT_MS) != 0) {
        return 3;
    }
    if (rc == 0) {
        reg4 = (uint8_t)((reg4 & 0xCFu) | (uint8_t)((fs & 3u) << 4));
        rc = lis3dh_write(dev, LIS3DH_CTRL_REG4, &reg4, 1);
    }
    return rc;
}

/* Read REFERENCE (0x26); the read clears the high-pass filter. */
static int lis3dh_read_reference(lis3dh_dev_t *dev, uint8_t *buf)
{
    int rc = lis3dh_read(dev, LIS3DH_REFERENCE, buf, 1);
    if (lis3dh_wait(dev, LIS3DH_I2C_TIMEOUT_MS) != 0) {
        rc = 3;
    }
    return rc;
}

/* Read WHO_AM_I (0x0F); a genuine LIS3DH returns 0x33. */
static int lis3dh_read_whoami(lis3dh_dev_t *dev, uint8_t *buf)
{
    int rc = lis3dh_read(dev, LIS3DH_WHO_AM_I, buf, 1);
    if (lis3dh_wait(dev, LIS3DH_I2C_TIMEOUT_MS) != 0) {
        rc = 3;
    }
    return rc;
}

/* High-pass-filter enables in CTRL_REG2[2:0] = HPCLICK/HPIS2/HPIS1.
 * Distinct control flow: a failing read falls straight through to the
 * modify+write; only a successful read followed by a wait failure returns 3. */
static int lis3dh_set_hpf_int(lis3dh_dev_t *dev, uint8_t bits)
{
    uint8_t reg2;
    int rc = lis3dh_read(dev, LIS3DH_CTRL_REG2, &reg2, 1);
    if (rc == 0 && (rc = lis3dh_wait(dev, LIS3DH_I2C_TIMEOUT_MS)) != 0) {
        return 3;
    }
    reg2 = (uint8_t)((reg2 & 0xF8u) | (uint8_t)(bits & 7u));
    return lis3dh_write(dev, LIS3DH_CTRL_REG2, &reg2, 1);
}

/* Latch the INT1 request: CTRL_REG5[3] = LIR_INT1. */
static int lis3dh_set_latch_int1(lis3dh_dev_t *dev, uint8_t enable)
{
    uint8_t reg5;
    int rc = lis3dh_read(dev, LIS3DH_CTRL_REG5, &reg5, 1);
    if (lis3dh_wait(dev, LIS3DH_I2C_TIMEOUT_MS) != 0) {
        return 3;
    }
    if (rc == 0) {
        reg5 = (uint8_t)((reg5 & 0xF7u) | (uint8_t)((enable & 1u) << 3));
        rc = lis3dh_write(dev, LIS3DH_CTRL_REG5, &reg5, 1);
    }
    return rc;
}

/* Write CTRL_REG3 (INT1 source routing) — pure write, no read-back. */
static int lis3dh_write_ctrl_reg3(lis3dh_dev_t *dev, const uint8_t *buf)
{
    return lis3dh_write(dev, LIS3DH_CTRL_REG3, buf, 1);
}

/* Read back CTRL_REG3. */
static int lis3dh_read_ctrl_reg3(lis3dh_dev_t *dev, uint8_t *buf)
{
    int rc = lis3dh_read(dev, LIS3DH_CTRL_REG3, buf, 1);
    if (lis3dh_wait(dev, LIS3DH_I2C_TIMEOUT_MS) != 0) {
        rc = 3;
    }
    return rc;
}

/* Write INT1_CFG (event-enable bits) — pure write, no read-back. */
static int lis3dh_write_int1_cfg(lis3dh_dev_t *dev, const uint8_t *buf)
{
    return lis3dh_write(dev, LIS3DH_INT1_CFG, buf, 1);
}

/* Read back INT1_CFG. */
static int lis3dh_read_int1_cfg(lis3dh_dev_t *dev, uint8_t *buf)
{
    int rc = lis3dh_read(dev, LIS3DH_INT1_CFG, buf, 1);
    if (lis3dh_wait(dev, LIS3DH_I2C_TIMEOUT_MS) != 0) {
        rc = 3;
    }
    return rc;
}

/* Read INT1_SRC (0x31); reading it clears the latched interrupt. */
static int lis3dh_read_int1_src(lis3dh_dev_t *dev, uint8_t *buf)
{
    int rc = lis3dh_read(dev, LIS3DH_INT1_SRC, buf, 1);
    if (lis3dh_wait(dev, LIS3DH_I2C_TIMEOUT_MS) != 0) {
        rc = 3;
    }
    return rc;
}

/* INT1 threshold: INT1_THS[6:0] = ths (bit 7 reserved). */
static int lis3dh_set_int1_threshold(lis3dh_dev_t *dev, uint8_t ths)
{
    uint8_t reg;
    int rc = lis3dh_read(dev, LIS3DH_INT1_THS, &reg, 1);
    if (lis3dh_wait(dev, LIS3DH_I2C_TIMEOUT_MS) != 0) {
        return 3;
    }
    if (rc == 0) {
        reg = (uint8_t)((reg & 0x80u) | (ths & 0x7Fu));
        rc = lis3dh_write(dev, LIS3DH_INT1_THS, &reg, 1);
    }
    return rc;
}

/* INT1 duration: INT1_DURATION[6:0] = dur (bit 7 reserved). */
static int lis3dh_set_int1_duration(lis3dh_dev_t *dev, uint8_t dur)
{
    uint8_t reg;
    int rc = lis3dh_read(dev, LIS3DH_INT1_DURATION, &reg, 1);
    if (lis3dh_wait(dev, LIS3DH_I2C_TIMEOUT_MS) != 0) {
        return 3;
    }
    if (rc == 0) {
        reg = (uint8_t)((reg & 0x80u) | (dur & 0x7Fu));
        rc = lis3dh_write(dev, LIS3DH_INT1_DURATION, &reg, 1);
    }
    return rc;
}

/* Read INT1_SRC and report it; log a bus error but return the byte regardless.
 * OEM 0x0803D1D4. */
static uint8_t lis3dh_int1_read_source(void)
{
    uint8_t src;

    if (lis3dh_read_int1_src(&g_lis3dh_dev, &src) != 0) {
        g_log_func(" ERR LIS1\r\n");
    }
    return src;
}

/* ------------------------------ public API ------------------------------ */

int lis3dh_accel_init(void)
{
    lis3dh_dev_t *dev = &g_lis3dh_dev;

    dev->write = lis3dh_i2c_write;
    dev->read  = lis3dh_i2c_read;
    dev->wait  = lis3dh_i2c_wait;

    /* Mask the two EXTI lines wired to INT1/INT2 while probing + configuring. */
    NVIC_DisableIRQ(0x48);
    NVIC_DisableIRQ(0x49);

    if (lis3dh_read_whoami(dev, &dev->whoami) != 0) {
        return 1;
    }
    if (dev->wait(LIS3DH_I2C_TIMEOUT_MS) != 0) {
        return 2;
    }
    return (dev->whoami == 0x33) ? 0 : 3;   /* LIS3DH WHO_AM_I = 0x33 */
}

void lis3dh_config_motion_int(int mode, int threshold)
{
    lis3dh_dev_t *dev = &g_lis3dh_dev;
    uint8_t reg3;
    uint8_t reference;
    uint8_t int1_cfg;

    /* Route the motion event through the high-pass filter onto INT1. */
    lis3dh_set_hpf_int(dev, 1);          /* HPIS1 */
    lis3dh_set_filtered_data(dev, 1);    /* FDS   */

    lis3dh_read_ctrl_reg3(dev, &reg3);
    reg3 |= 0x40u;                       /* CTRL_REG3.I1_IA1 -> route IA1 to INT1 */
    lis3dh_write_ctrl_reg3(dev, &reg3);

    lis3dh_set_latch_int1(dev, 0);       /* non-latched */
    lis3dh_set_full_scale(dev, 0);       /* +/-2 g */
    lis3dh_set_int1_threshold(dev, (uint8_t)threshold);
    lis3dh_set_int1_duration(dev, 0);

    lis3dh_read_reference(dev, &reference);   /* clears the HP filter */

    lis3dh_read_int1_cfg(dev, &int1_cfg);
    if (mode == 0) {
        int1_cfg |= 0x2Au;                          /* XHIE|YHIE|ZHIE */
    } else {
        int1_cfg = (uint8_t)((int1_cfg & 0xD7u) | 0x02u);  /* clear YHIE+ZHIE, set XHIE */
    }
    int1_cfg &= 0x7Fu;                              /* clear AOI (bit 7) */
    lis3dh_write_int1_cfg(dev, &int1_cfg);

    lis3dh_set_power_mode(dev, 0);       /* high-resolution */
    lis3dh_set_odr(dev, 5);              /* 100 Hz */
}

int lis3dh_powerdown(void)
{
    return lis3dh_set_odr(&g_lis3dh_dev, 0);   /* ODR = 0 -> power-down */
}

void lis3dh_int1_clear(void)
{
    int8_t tries = 100;
    uint8_t src;

    /* Drain INT1 while the interrupt pin (GPIOC, mask 0x08) stays asserted. */
    while ((tries = (int8_t)(tries - 1)) != 0 &&
           HAL_GPIO_ReadPin(LIS3DH_INT1_GPIO, LIS3DH_INT1_PIN) != 0) {
        watchdog_kick();
        src = lis3dh_int1_read_source();
        if (g_lis3dh_int1_last_src != src) {
            g_lis3dh_int1_last_src = src;
            log_print_timestamp_prefix();
            g_log_func("Clear Lis 0x%02X\r\n", src);
        }
        systick_delay(10);
    }
    if (tries == 0) {            /* pin never released within the retry budget */
        log_print_timestamp_prefix();
        g_log_func("Err Clear Lis\r\n");
    }
}
