#ifndef SHIFTER_I2C_H
#define SHIFTER_I2C_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void i2c1_init(void);
bool i2c1_write(uint8_t addr7, const uint8_t *data, size_t len);
bool i2c1_read(uint8_t addr7, uint8_t *data, size_t len);
bool i2c1_write_read(uint8_t addr7,
                     const uint8_t *tx, size_t tx_len,
                     uint8_t *rx, size_t rx_len);

#endif /* SHIFTER_I2C_H */
