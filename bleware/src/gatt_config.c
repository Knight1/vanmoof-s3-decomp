/* gatt_config.c — GATT service & characteristic registry lookup.
 *
 * OEM source file: source/xs3_gatt_config.c
 *
 * The service table at flash 0x0002A2F8 holds 11 entries of 8 bytes each:
 *   +0x00 u16 service_short_id  (e.g. 0x5500)
 *   +0x02 u16 item_count        (characteristics under this service)
 *   +0x04 u32 *items            (→ 28-byte item array)
 *
 * Per-item entries are 28 bytes:
 *   +0x00 u16 char_short_id
 *   +0x02 u16 flags_a
 *   ...
 *   +0x1A u8  refresh_from_modbus
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* GATT service table — OEM flash 0x0002A2F8, 11 entries × 8 bytes. */
struct gatt_svc_entry {
    uint16_t svc_id;
    uint16_t item_count;
    uint32_t items_ptr;
};

struct gatt_item_entry {
    uint16_t char_id;
    uint16_t flags_a;
    uint8_t  pad04[8];
    uint8_t  flags_c;
    uint8_t  pad0d[3];
    uint32_t required_mask;
    uint8_t  pad14[6];
    uint8_t  refresh;
    uint8_t  pad1b[13];
};

extern const struct gatt_svc_entry g_gatt_svc_table[];    /* 0x0002A2F8 */
#define GATT_SVC_TABLE_COUNT  11

/* Look up a GATT characteristic item entry by (svc_id, char_idx).
 * Linear scan over the 11-entry service table; returns pointer to
 * the 28-byte item entry, or NULL with monitor_log on error.
 * OEM @ 0x00011228 (~200 B). */
struct gatt_item_entry *gatt_config_lookup_item(uint16_t svc_uuid,
                                                 uint16_t char_idx)
{
    for (int i = 0; i < GATT_SVC_TABLE_COUNT; i++) {
        if (g_gatt_svc_table[i].svc_id == svc_uuid) {
            if (char_idx < g_gatt_svc_table[i].item_count) {
                return (struct gatt_item_entry *)
                    (g_gatt_svc_table[i].items_ptr
                     + (uint32_t)char_idx * 28);
            }
            /* index out of range for this service */
            extern void monitor_log(const char *file, int line,
                                    const char *func, int level,
                                    const char *fmt, ...);
            monitor_log("source/xs3_gatt_config.c", 0x412, NULL, 1,
                        "An attempt of reading to non existing "
                        "characteristic was made, svc=0x%04X", svc_uuid);
            return NULL;
        }
    }
    /* service not found */
    extern void monitor_log(const char *file, int line,
                            const char *func, int level,
                            const char *fmt, ...);
    monitor_log("source/xs3_gatt_config.c", 0x40D, NULL, 1,
                "An attempt of reading to non existing service "
                "was made, svc=0x%04X", svc_uuid);
    return NULL;
}

/* Weak service table placeholder — real data at flash 0x0002A2F8. */
__attribute__((weak))
const struct gatt_svc_entry g_gatt_svc_table[GATT_SVC_TABLE_COUNT];
