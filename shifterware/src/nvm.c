/* nvm.c — calibration record stored in the last flash page.
 *
 * Layout: a single nvm_calib_t at NVM_BASE. On boot we validate the
 * magic, version, and CRC32; if any check fails we load defaults and do
 * NOT auto-write them (the bike behaves with sane values, and saving is
 * deferred to whatever calibration message arrives over the protocol). */

#include "nvm.h"
#include "crc.h"
#include "flash_store.h"
#include "motor.h"
#include <string.h>

/* Last 1 KB page of a 32 KB flash. */
#define NVM_BASE          (0x08007C00u)

static nvm_calib_t s_ram;

static uint32_t calib_crc(const nvm_calib_t *c)
{
    return crc32_block((const uint8_t *)c,
                       sizeof(nvm_calib_t) - sizeof(uint32_t));
}

void nvm_load_defaults(nvm_calib_t *out)
{
    memset(out, 0, sizeof *out);
    out->magic           = NVM_MAGIC;
    out->version         = NVM_VERSION;
    out->length          = (uint16_t)(sizeof(nvm_calib_t) - 8u);
    out->steps_per_gear  = MOTOR_STEPS_PER_GEAR;
    out->hall_gear1_mid  = 1900u;
    out->hall_gear2_mid  = 2700u;
    out->hall_window     = 300u;
    out->step_hz         = MOTOR_STEP_HZ_DEFAULT;
    out->home_max_steps  = MOTOR_STEPS_PER_GEAR * 4u;
    out->crc32           = calib_crc(out);
}

void nvm_init(void)
{
    crc_init();

    const nvm_calib_t *flash_copy = (const nvm_calib_t *)NVM_BASE;
    if (flash_copy->magic == NVM_MAGIC
        && flash_copy->version == NVM_VERSION
        && flash_copy->length == (uint16_t)(sizeof(nvm_calib_t) - 8u)
        && calib_crc(flash_copy) == flash_copy->crc32) {
        s_ram = *flash_copy;
    } else {
        nvm_load_defaults(&s_ram);
    }
}

const nvm_calib_t *nvm_get(void)
{
    return &s_ram;
}

bool nvm_save(const nvm_calib_t *in)
{
    if (in == NULL) return false;

    nvm_calib_t buf = *in;
    buf.magic   = NVM_MAGIC;
    buf.version = NVM_VERSION;
    buf.length  = (uint16_t)(sizeof(nvm_calib_t) - 8u);
    buf.crc32   = calib_crc(&buf);

    /* flash_erase_page is the OEM single-page wrapper: it owns the
     * unlock/lock cycle. Programming still needs its own unlock. */
    flash_erase_page(NVM_BASE);
    flash_unlock();
    if (!flash_program_block(NVM_BASE, &buf, sizeof buf)) {
        flash_lock();
        return false;
    }
    flash_lock();

    s_ram = buf;
    return true;
}
