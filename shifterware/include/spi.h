#ifndef SHIFTER_SPI_H
#define SHIFTER_SPI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    SPI_MODE_0 = 0u,    /* CPOL=0, CPHA=0 */
    SPI_MODE_1 = 1u,
    SPI_MODE_2 = 2u,
    SPI_MODE_3 = 3u,
} spi_mode_t;

void    spi1_init(spi_mode_t mode, uint8_t prescaler /* 0..7 -> pclk/2 .. pclk/256 */);
uint8_t spi1_xfer(uint8_t tx);
void    spi1_xfer_buf(const uint8_t *tx, uint8_t *rx, size_t len);

#endif /* SHIFTER_SPI_H */
