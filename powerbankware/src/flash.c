#include "powerbankware.h"

/*
 * STM32F0 FLASH driver + OTA program/erase wrappers (the backend shared by the
 * Modbus OTA write path and the binary OTA receiver), the RTC backup-register
 * write used to latch upgrade state across a reset, and the OTA system reset.
 *
 * FLASH @ 0x40022000: ACR +0x00, KEYR +0x04, SR +0x0C, CR +0x10, AR +0x14.
 *   SR: BSY 0x1, PGERR 0x4, WRPRTERR 0x10, EOP 0x20.
 *   CR: PG 0x1, PER 0x2, MER 0x4, STRT 0x40, LOCK 0x80.
 * HAL handle @ 0x20002618 (+0x18 Lock, +0x1C ErrorCode).
 *
 *   flash_unlock          = FUN_0801a24c   flash_wait            = FUN_0801a2ec
 *   flash_program_hw      = FUN_0801a2b4   flash_set_error       = FUN_0801a36c
 *   flash_setup_page_erase= FUN_0801a4e8   flash_setup_mass_erase= FUN_0801a4b8
 *   flash_ex_erase        = FUN_0801a3cc   flash_program         = FUN_0801a120
 *   flash_erase_page      = FUN_08013878   flash_program_words   = FUN_080138f4
 *   system_reset_ota      = FUN_08013b94   rtc_backup_write      = FUN_0801c6a2
 */

#define FLASH_BASE 0x40022000u
#define FLASH_KEYR (*(volatile uint32_t *)(FLASH_BASE + 0x04))
#define FLASH_SR   (*(volatile uint32_t *)(FLASH_BASE + 0x0C))
#define FLASH_CR   (*(volatile uint32_t *)(FLASH_BASE + 0x10))
#define FLASH_AR   (*(volatile uint32_t *)(FLASH_BASE + 0x14))

#define H_LOCK (*(volatile uint8_t  *)(0x20002618 + 0x18))
#define H_ERR  (*(volatile uint32_t *)(0x20002618 + 0x1c))

extern uint32_t tick_get(void);
extern void     spi_error_reset(void);   /* shared fatal-error path */

/* FUN_0801a36c — latch the FLASH_SR error flags into the handle error code. */
static void flash_set_error(void)
{
    uint32_t clr = 0;
    if ((FLASH_SR & 0x10) == 0x10) { H_ERR |= 2; clr = 0x10; }
    if ((FLASH_SR & 0x04) == 0x04) { H_ERR |= 1; clr |= 4; }
    FLASH_SR = clr;
}

/* FUN_0801a2ec — wait for BSY to clear; clear EOP; report PG/WRP errors. */
static int flash_wait(uint32_t timeout)
{
    uint32_t start = tick_get();
    while ((FLASH_SR & 1) == 1) {
        if (timeout != 0xffffffffu &&
            (timeout == 0 || timeout < (uint32_t)(tick_get() - start))) {
            return 3;
        }
    }
    if ((FLASH_SR & 0x20) == 0x20) {
        FLASH_SR = 0x20;                          /* clear EOP */
    }
    if ((FLASH_SR & 0x10) != 0x10 && (FLASH_SR & 4) != 4) {
        return 0;
    }
    flash_set_error();
    return 1;
}

/* FUN_0801a2b4 — set PG and write a halfword. */
static void flash_program_hw(volatile uint16_t *addr, uint16_t val)
{
    H_ERR = 0;
    FLASH_CR |= 1u;                               /* PG */
    *addr = val;
}

static void flash_setup_page_erase(uint32_t addr)
{
    H_ERR = 0;
    FLASH_CR |= 2u;                               /* PER */
    FLASH_AR  = addr;
    FLASH_CR |= 0x40u;                            /* STRT */
}

static void flash_setup_mass_erase(void)
{
    H_ERR = 0;
    FLASH_CR |= 4u;                               /* MER */
    FLASH_CR |= 0x40u;                            /* STRT */
}

/* FUN_0801a24c — unlock the flash with KEY1/KEY2. Returns 0 on success. */
static uint8_t flash_unlock(void)
{
    if ((FLASH_CR & 0x80) != 0) {                 /* LOCK set */
        FLASH_KEYR = 0x45670123u;
        FLASH_KEYR = 0xcdef89abu;
        if ((FLASH_CR & 0x80) != 0) {
            return 1;
        }
    }
    return 0;
}

/* FUN_0801a3cc — HAL_FLASHEx_Erase. cfg = {type, addr, count}; type 1 = mass,
 * else page erase across `count` 2 KB pages. Returns 0 / 1 / 2(busy). */
static char flash_ex_erase(int *cfg, uint32_t *err_out)
{
    char st = 1;
    if (H_LOCK == 1) {
        return 2;
    }
    H_LOCK = 1;
    if (cfg[0] == 1) {
        if (flash_wait(50000) == 0) {
            flash_setup_mass_erase();
            st = (char)flash_wait(50000);
            FLASH_CR &= 0xfffffffbu;              /* clear MER */
        }
    } else {
        if (flash_wait(50000) == 0) {
            *err_out = 0xffffffffu;
            for (uint32_t a = (uint32_t)cfg[1];
                 a < (uint32_t)(cfg[2] * 0x800 + cfg[1]); a += 0x800) {
                flash_setup_page_erase(a);
                st = (char)flash_wait(50000);
                FLASH_CR &= 0xfffffffdu;          /* clear PER */
                if (st != 0) {
                    *err_out = a;
                    break;
                }
            }
        }
    }
    H_LOCK = 0;
    return st;
}

/* FUN_0801a120 — HAL_FLASH_Program. type 1 = halfword, 2 = word, else double. */
static char flash_program(int type, uint32_t addr, uint32_t data_lo, uint32_t data_hi)
{
    if (H_LOCK == 1) {
        return 2;
    }
    H_LOCK = 1;
    char st = (char)flash_wait(50000);
    if (st == 0) {
        int n = (type == 1) ? 1 : (type == 2) ? 2 : 4;
        for (int i = 0; i < n; i++) {
            uint32_t shift = (uint32_t)i * 0x10;
            uint16_t hw;
            if (shift < 0x20) {
                uint32_t up = (0x20 - shift < 32) ? (data_hi << (0x20 - shift)) : 0;
                hw = (uint16_t)((data_lo >> shift) | up);
            } else {
                hw = (uint16_t)(data_hi >> (shift - 0x20));
            }
            flash_program_hw((volatile uint16_t *)(addr + (uint32_t)i * 2), hw);
            st = (char)flash_wait(50000);
            FLASH_CR &= 0xfffffffeu;              /* clear PG */
            if (st != 0) {
                break;
            }
        }
    }
    H_LOCK = 0;
    return st;
}

/* FUN_08013878 — erase the 2 KB page at `addr`, retrying up to 50 times. */
void flash_erase_page(uint32_t addr)
{
    int * const cfg = (int *)0x2000019c;
    uint32_t * const err = (uint32_t *)0x200001a8;
    uint8_t retry = 0;

    flash_unlock();
    cfg[0] = 0;
    cfg[1] = (int)addr;
    cfg[2] = 1;
    do {
        *(volatile uint32_t *)0x20002614 = 0;
        if (flash_ex_erase(cfg, err) == 0) {
            retry = 0x32;
        } else if (++retry > 0x31) {
            spi_error_reset();
        }
    } while (retry < 0x32);
}

/* FUN_080138f4 — program `len` bytes from `src` into flash at `addr` as words,
 * verifying each. Returns 0 on success, 1 on program/verify failure. */
int flash_program_words(uint32_t addr, uint16_t len, const void *src)
{
    const uint8_t *s = (const uint8_t *)src;
    volatile uint32_t *dst = (volatile uint32_t *)addr;
    uint16_t i = 0;

    while (i < len) {
        uint32_t w = (uint32_t)s[i] | ((uint32_t)s[i + 1] << 8) |
                     ((uint32_t)s[i + 2] << 16) | ((uint32_t)s[i + 3] << 24);
        i = (uint16_t)(i + 4);
        **(volatile uint32_t **)0x200006ac = 0x0000AAAAu;   /* IWDG kick */
        *(volatile uint32_t *)0x20002614 = 0;
        if (flash_program(2, (uint32_t)dst, w, 0) != 0) {
            return 1;
        }
        if (*dst != w) {
            return 1;
        }
        dst++;
    }
    return 0;
}

/* FUN_0801a298 — re-lock the flash (CR LOCK). */
void flash_lock(void)
{
    FLASH_CR |= 0x80u;
}

/* FUN_0801c6a2 — write an RTC backup register (RTC base + 0x50 + idx*4). */
void rtc_backup_write(void *hrtc, int idx, uint8_t val)
{
    uint32_t inst = *(volatile uint32_t *)hrtc;
    *(volatile uint32_t *)(inst + 0x50 + idx * 4) = val;
}

/* FUN_08013b94 — persist + reset for OTA. */
void system_reset_ota(void)
{
    **(volatile uint32_t **)0x200006ac = 0x0000AAAAu;       /* IWDG kick */
    fedl5236_record_save();
    __DSB();
    *(volatile uint32_t *)(0xE000ED00u + 0x0C) = 0x05FA0004u; /* SYSRESETREQ */
    __DSB();
    for (;;) {
    }
}
