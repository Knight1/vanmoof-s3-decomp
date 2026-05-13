/* spi.c — SPI1 blocking 8-bit master. */

#include "spi.h"
#include "gpio.h"
#include "mm32f031.h"

static void spi1_pins_init(void)
{
    /* PA5 = SCK, PA6 = MISO, PA7 = MOSI on the MM32F031F6U6, AF0. */
    gpio_port_clock_enable(GPIOA);
    gpio_pin_alt_func(GPIOA, 5, 0);
    gpio_pin_alt_func(GPIOA, 6, 0);
    gpio_pin_alt_func(GPIOA, 7, 0);
    gpio_pin_mode(GPIOA, 5, GPIO_MODE_AF);
    gpio_pin_mode(GPIOA, 6, GPIO_MODE_AF);
    gpio_pin_mode(GPIOA, 7, GPIO_MODE_AF);
    gpio_pin_speed(GPIOA, 5, GPIO_SPEED_HIGH);
    gpio_pin_speed(GPIOA, 7, GPIO_SPEED_HIGH);
}

void spi1_init(spi_mode_t mode, uint8_t prescaler)
{
    spi1_pins_init();
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN_Msk;

    SPI1->CR1 = 0u;
    uint32_t cr1 = SPI_CR1_MSTR_Msk | SPI_CR1_SSI_Msk | SPI_CR1_SSM_Msk
                 | (((uint32_t)prescaler & 0x7u) << 3);
    if (mode & 0x1u) cr1 |= SPI_CR1_CPHA_Msk;
    if (mode & 0x2u) cr1 |= SPI_CR1_CPOL_Msk;

    SPI1->CR2 = SPI_CR2_DS_8BIT | SPI_CR2_FRXTH_Msk;
    SPI1->CR1 = cr1 | SPI_CR1_SPE_Msk;
}

uint8_t spi1_xfer(uint8_t tx)
{
    while ((SPI1->SR & SPI_SR_TXE_Msk) == 0u) { }
    *(volatile uint8_t *)&SPI1->DR = tx;
    while ((SPI1->SR & SPI_SR_RXNE_Msk) == 0u) { }
    return *(volatile uint8_t *)&SPI1->DR;
}

void spi1_xfer_buf(const uint8_t *tx, uint8_t *rx, size_t len)
{
    for (size_t i = 0u; i < len; i++) {
        const uint8_t  out = (tx != NULL) ? tx[i] : 0xFFu;
        const uint8_t  in  = spi1_xfer(out);
        if (rx != NULL) rx[i] = in;
    }
}
