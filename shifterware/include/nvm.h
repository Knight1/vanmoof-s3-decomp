#ifndef SHIFTER_NVM_H
#define SHIFTER_NVM_H

#include <stdint.h>
#include <stdbool.h>

#define NVM_MAGIC          (0x5346434Bu)   /* 'SFCK' — Shifter Factory Calibration */
#define NVM_VERSION        (1u)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;          /* payload bytes following this header */
    /* --- payload starts here --- */
    uint16_t steps_per_gear;
    uint16_t hall_gear1_mid;
    uint16_t hall_gear2_mid;
    uint16_t hall_window;
    uint16_t step_hz;
    uint16_t home_max_steps;
    uint8_t  reserved[12];
    /* --- footer --- */
    uint32_t crc32;
} nvm_calib_t;

void              nvm_init(void);
const nvm_calib_t *nvm_get(void);
void              nvm_load_defaults(nvm_calib_t *out);
bool              nvm_save(const nvm_calib_t *in);

#endif /* SHIFTER_NVM_H */
