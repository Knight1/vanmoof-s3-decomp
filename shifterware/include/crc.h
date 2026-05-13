#ifndef SHIFTER_CRC_H
#define SHIFTER_CRC_H

#include <stdint.h>
#include <stddef.h>

/* OEM-confirmed. */
uint32_t crc32_word(uint32_t word);                                 /* @ 0x08005CF0 */
uint32_t crc32_words(const uint32_t *array, int count);             /* @ 0x08005D08 */
void     crc_reset(void);                                           /* @ 0x08005CE8 */

/* Speculative — no OEM evidence yet; kept to satisfy nvm.c. */
void     crc_init(void);
uint32_t crc32_block(const void *data, size_t len_bytes);
uint32_t crc32_continue(const void *data, size_t len_bytes);

#endif /* SHIFTER_CRC_H */
