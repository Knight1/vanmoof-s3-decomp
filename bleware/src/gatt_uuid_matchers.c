/* gatt_uuid_matchers.c — per-service UUID-to-index matchers.
 *
 * Each GATT service (0x5510..0x55C0 except 0x5540 which uses the
 * vanmoof_ssp matcher, and 0x5500 which is backoffice) has a
 * dedicated function that maps a 128-bit attribute UUID to a
 * characteristic index within that service.
 *
 * The TI BLE stack passes an attribute pointer; the matcher:
 *   1. Unwinds past any CCCD descriptors (type 0x02, UUID 0x2902)
 *   2. Checks the attr type is 0x10 (128-bit UUID)
 *   3. Compares the 16-byte UUID against a per-service table
 *   4. Returns 0..N-1 on match, 0xFF on no match
 *
 * All 8 matchers are structurally identical — only the table pointer
 * and entry count vary. We generate them with a macro.
 *
 * OEM addresses:
 *   svc_5510_char_uuid_to_index @ 0x0001D744  (3 entries)
 *   svc_5520_char_uuid_to_index @ 0x0001AB6C  (4 entries)
 *   svc_5530_char_uuid_to_index @ 0x00010EB8  (11 entries)
 *   svc_5560_char_uuid_to_index @ 0x00012FA8  (9 entries)
 *   svc_5570_char_uuid_to_index @ 0x0001D814  (4 entries)
 *   svc_5590_char_uuid_to_index @ 0x00020A28  (2 entries)
 *   svc_55a0_char_uuid_to_index @ 0x0001ABEC  (4 entries)
 *   svc_55c0_char_uuid_to_index @ 0x0001D6DC  (3 entries)
 */

#include <stdint.h>
#include <stddef.h>

/* Shared attribute-struct layout (TI BLE stack internal):
 *   +0x00 u8  type  (0x10 = 128-bit UUID, 0x02 = CCCD descriptor)
 *   +0x04 u16 desc_uuid (0x2902 for CCCD)
 *   +0x04 u8  uuid[16] (when type == 0x10)
 */
#define ATTR_TYPE_128BIT_UUID  0x10
#define ATTR_TYPE_CCCD         0x02
#define CCCD_DESC_UUID         0x2902u

/* Generic UUID matcher — compares the 16-byte UUID at attr+4 against
 * `count` entries in `table` (each entry is 16 bytes at stride 0x10).
 * Returns the index on match, 0xFF if no match. */
static uint8_t uuid_matcher_generic(const uint8_t *attr,
                                     const uint8_t *table,
                                     uint8_t count)
{
    const uint8_t *uuid = attr + 4;

    for (uint8_t i = 0; i < count; i++) {
        int match = 1;
        for (uint8_t j = 0; j < 16; j++) {
            if (uuid[j] != table[i * 16 + j]) {
                match = 0;
                break;
            }
        }
        if (match) return i;
    }
    return 0xFF;
}

/* Core dispatcher — shared by all matchers. Unwinds CCCD descriptors,
 * checks type, delegates to uuid_matcher_generic. */
static uint8_t char_uuid_to_index_core(const uint8_t *attr,
                                        const uint8_t *table,
                                        uint8_t count)
{
    /* unwind past CCCD descriptors */
    while (attr[0] == ATTR_TYPE_CCCD &&
           *(const uint16_t *)(attr + 4) == CCCD_DESC_UUID) {
        attr -= 0x10;  /* OEM walks backward through the attr list */
    }

    if (attr[0] != ATTR_TYPE_128BIT_UUID) {
        return 0xFF;
    }

    return uuid_matcher_generic(attr, table, count);
}

/* ---- Per-service UUID tables (flash literal pools) ---------------- */

#define DECLARE_UUID_TABLE(svc, count) \
    extern const uint8_t g_svc_##svc##_uuid_table[count * 16]

DECLARE_UUID_TABLE(5510, 3);
DECLARE_UUID_TABLE(5520, 4);
DECLARE_UUID_TABLE(5530, 11);
DECLARE_UUID_TABLE(5560, 9);
DECLARE_UUID_TABLE(5570, 4);
DECLARE_UUID_TABLE(5590, 2);
DECLARE_UUID_TABLE(55a0, 4);
DECLARE_UUID_TABLE(55c0, 3);

/* ---- Generated matcher functions ---------------------------------- */

#define DEFINE_CHAR_UUID_MATCHER(svc, count)               \
    uint8_t svc_##svc##_char_uuid_to_index(uint8_t *attr)  \
    {                                                       \
        return char_uuid_to_index_core(attr,                \
                   g_svc_##svc##_uuid_table, count);        \
    }

DEFINE_CHAR_UUID_MATCHER(5510, 3)
DEFINE_CHAR_UUID_MATCHER(5520, 4)
DEFINE_CHAR_UUID_MATCHER(5530, 11)
DEFINE_CHAR_UUID_MATCHER(5560, 9)
DEFINE_CHAR_UUID_MATCHER(5570, 4)
DEFINE_CHAR_UUID_MATCHER(5590, 2)
DEFINE_CHAR_UUID_MATCHER(55a0, 4)
DEFINE_CHAR_UUID_MATCHER(55c0, 3)

/* ---- Weak UUID table placeholders — real data in flash literal pools */

#define WEAK_UUID_TABLE(svc, count) \
    __attribute__((weak)) const uint8_t g_svc_##svc##_uuid_table[count * 16]

WEAK_UUID_TABLE(5510, 3);
WEAK_UUID_TABLE(5520, 4);
WEAK_UUID_TABLE(5530, 11);
WEAK_UUID_TABLE(5560, 9);
WEAK_UUID_TABLE(5570, 4);
WEAK_UUID_TABLE(5590, 2);
WEAK_UUID_TABLE(55a0, 4);
WEAK_UUID_TABLE(55c0, 3);

/* ---- vanmoof_ssp_uuid_to_index --------------------------------------
 * Service 0x5540 uses a shared SSP UUID matcher with 18 entries.
 * OEM @ 0x0000A26C. */

extern const uint8_t g_vanmoof_ssp_uuid_table[18 * 16];

uint8_t vanmoof_ssp_uuid_to_index(uint8_t *attr)
{
    return char_uuid_to_index_core(attr, g_vanmoof_ssp_uuid_table, 18);
}

__attribute__((weak))
const uint8_t g_vanmoof_ssp_uuid_table[18 * 16];
