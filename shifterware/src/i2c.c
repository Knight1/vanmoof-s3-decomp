/* i2c.c — I2C1 blocking master. */

#include "i2c.h"
#include "gpio.h"
#include "mm32f031.h"

/* TIMINGR for 100 kHz on a 48 MHz I2C clock. From MM32F031 reference
 * manual table "I2C_TIMINGR settings". */
#define I2C_TIMING_100K   (0xB0420F13u)

static void i2c1_pins_init(void)
{
    /* PB6 = I2C1_SCL, PB7 = I2C1_SDA, AF1. Open-drain with internal PU. */
    gpio_port_clock_enable(GPIOB);
    gpio_pin_alt_func(GPIOB, 6, 1);
    gpio_pin_alt_func(GPIOB, 7, 1);
    gpio_pin_mode(GPIOB, 6, GPIO_MODE_AF);
    gpio_pin_mode(GPIOB, 7, GPIO_MODE_AF);
    gpio_pin_output_type(GPIOB, 6, GPIO_OTYPE_OD);
    gpio_pin_output_type(GPIOB, 7, GPIO_OTYPE_OD);
    gpio_pin_pull(GPIOB, 6, GPIO_PULL_UP);
    gpio_pin_pull(GPIOB, 7, GPIO_PULL_UP);
}

void i2c1_init(void)
{
    i2c1_pins_init();
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN_Msk;

    I2C1->CR1     = 0u;
    I2C1->TIMINGR = I2C_TIMING_100K;
    I2C1->CR1     = I2C_CR1_PE_Msk;
}

static bool wait_flag(volatile uint32_t *reg, uint32_t mask, bool set)
{
    for (uint32_t i = 0u; i < 1000000u; i++) {
        const bool now = (*reg & mask) != 0u;
        if (now == set) return true;
    }
    return false;
}

static void i2c1_start(uint8_t addr7, size_t len, bool read, bool autoend)
{
    uint32_t cr2 = ((uint32_t)(addr7 & 0x7Fu) << 1)
                 | ((uint32_t)(len & 0xFFu) << 16)
                 | I2C_CR2_START_Msk;
    if (read)    cr2 |= I2C_CR2_RD_WRN_Msk;
    if (autoend) cr2 |= I2C_CR2_AUTOEND_Msk;
    I2C1->CR2 = cr2;
}

bool i2c1_write(uint8_t addr7, const uint8_t *data, size_t len)
{
    i2c1_start(addr7, len, false, true);
    for (size_t i = 0u; i < len; i++) {
        if (!wait_flag(&I2C1->ISR, I2C_ISR_TXIS_Msk | I2C_ISR_NACKF_Msk, true)) {
            return false;
        }
        if ((I2C1->ISR & I2C_ISR_NACKF_Msk) != 0u) {
            I2C1->ICR = I2C_ICR_NACKCF_Msk;
            return false;
        }
        I2C1->TXDR = data[i];
    }
    if (!wait_flag(&I2C1->ISR, I2C_ISR_STOPF_Msk, true)) return false;
    I2C1->ICR = I2C_ICR_STOPCF_Msk;
    return true;
}

bool i2c1_read(uint8_t addr7, uint8_t *data, size_t len)
{
    i2c1_start(addr7, len, true, true);
    for (size_t i = 0u; i < len; i++) {
        if (!wait_flag(&I2C1->ISR, I2C_ISR_RXNE_Msk | I2C_ISR_NACKF_Msk, true)) {
            return false;
        }
        if ((I2C1->ISR & I2C_ISR_NACKF_Msk) != 0u) {
            I2C1->ICR = I2C_ICR_NACKCF_Msk;
            return false;
        }
        data[i] = (uint8_t)(I2C1->RXDR & 0xFFu);
    }
    if (!wait_flag(&I2C1->ISR, I2C_ISR_STOPF_Msk, true)) return false;
    I2C1->ICR = I2C_ICR_STOPCF_Msk;
    return true;
}

bool i2c1_write_read(uint8_t addr7,
                     const uint8_t *tx, size_t tx_len,
                     uint8_t *rx, size_t rx_len)
{
    i2c1_start(addr7, tx_len, false, false);
    for (size_t i = 0u; i < tx_len; i++) {
        if (!wait_flag(&I2C1->ISR, I2C_ISR_TXIS_Msk | I2C_ISR_NACKF_Msk, true)) {
            return false;
        }
        if ((I2C1->ISR & I2C_ISR_NACKF_Msk) != 0u) {
            I2C1->ICR = I2C_ICR_NACKCF_Msk;
            return false;
        }
        I2C1->TXDR = tx[i];
    }
    if (!wait_flag(&I2C1->ISR, I2C_ISR_TC_Msk, true)) return false;
    return i2c1_read(addr7, rx, rx_len);
}
