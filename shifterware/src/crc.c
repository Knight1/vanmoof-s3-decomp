/* crc.c — hardware CRC32 (MM32F031 CRC peripheral at 0x40023000).
 *
 * Mix of OEM-confirmed and speculative code, marked per function.
 * crc.c.bak holds the pre-decomp version. */

#include "crc.h"
#include "mm32f031.h"
#include <stddef.h>

/* ---- OEM-confirmed -------------------------------------------------- */

/* OEM @ 0x08005CF0 — feeds one word into the running CRC and returns
 * the new accumulator value. The 2-iteration nop loop is a deliberate
 * settle delay between the DR write and the read-back. */
uint32_t crc32_word(uint32_t word)
{
    CRC_HW->DR = word;
    for (volatile int i = 2; i != 0; i--) { /* settle */ }
    return CRC_HW->DR;
}

/* OEM @ 0x08005D08 — feeds `count` words into the running CRC and
 * returns the accumulator. No per-iteration delay; the final DR read
 * after the loop drains the pipeline. */
uint32_t crc32_words(const uint32_t *array, int count)
{
    for (int i = 0; i < count; i++) {
        CRC_HW->DR = array[i];
    }
    return CRC_HW->DR;
}

/* OEM @ 0x08005CE8 — resets the CRC engine to its initial value. */
void crc_reset(void)
{
    CRC_HW->CR = CRC_CR_RESET_Msk;
}

/* ---- Speculative (no OEM evidence yet; kept to satisfy nvm.c) ------- */

void crc_init(void)
{
    RCC->AHBENR |= RCC_AHBENR_CRCEN_Msk;
    crc_reset();
}

uint32_t crc32_continue(const void *data, size_t len_bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    volatile uint8_t *dr8 = (volatile uint8_t *)&CRC_HW->DR;
    for (size_t i = 0u; i < len_bytes; i++) {
        *dr8 = p[i];
    }
    return CRC_HW->DR;
}

uint32_t crc32_block(const void *data, size_t len_bytes)
{
    crc_reset();
    return crc32_continue(data, len_bytes);
}
