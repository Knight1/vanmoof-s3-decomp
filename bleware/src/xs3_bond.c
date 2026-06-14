/* xs3_bond.c — GAP bond-management glue (OEM source/xs3_gap_bondmanagement.c).
 *
 * Two functions from the GAP bond manager:
 *
 *   bond_store_dump_log  @ 0x0000A07C  — walk the 10 bond slots and dump
 *                                        each entry (IRK, address mode,
 *                                        MAC, LTK, ediv, rand, keysize)
 *                                        to the debug console.
 *   bond_save_event_cb   @ 0x00014E10  — pairing/bond-completion callback
 *                                        dispatched from the bluetoothtask
 *                                        event loop on event type 0x03.
 *
 * Both go through two distinct OEM logging primitives:
 *
 *   log_emit_v (FUN_0001AC6C) — the generic TI-BLE-stack ICall
 *     request/reply helper (declared in bleware.h). The bond manager
 *     reuses it as a GAP-bond-storage *read*: log_emit_v(0x10, handle,
 *     item, len, out_buf) sends an ICall request that pulls one NV item
 *     for the addressed bond slot and returns the reply word. Same r0..r3
 *     + stack ABI as a log call, so the variadic declaration binds
 *     unchanged.
 *
 *   monitor_log (FUN_00006D90) — the location-aware debug-console
 *     formatter: monitor_log(file, line, fn, level, fmt, ...). When fmt
 *     is NULL the variadic tail is a hex-dump descriptor
 *     (label, hex_fmt, buf, count) instead of printf arguments — the same
 *     convention used across the src/monitor command translation units.
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* OEM debug-console logger, FUN_00006D90. Not in bleware.h; declared the
 * same way every src/monitor command translation unit declares it. */
extern void monitor_log(const char *file, int line, const char *fn,
                        int level, const char *fmt, ...);

/* Post a 1-byte control event to the bluetoothtask queue (kind 0x32).
 * OEM FUN_000265C4; strong definition lives in src/oad.c. */
extern void bleware_control_event_post(uint32_t status);

/* OEM source-path string baked into this TU's rodata
 * (flash 0x0000A1F0 / 0x00014E78). */
static const char K_FILE[] = "source/xs3_gap_bondmanagement.c";

/* Debug-console log level used by these handlers. 9 == INFO in the OEM
 * level table (see src/monitor/cmd_ble.c); 2 is the error/warn level the
 * pairing callback uses for failure lines; 0 is the success line's level. */
#define BOND_LOG_INFO   9
#define BOND_LOG_ERR    2
#define BOND_LOG_OK     0

/* ---- bond_store_dump_log ------------------------------------------ */

/* GAP bond-store layout constants, as read by the OEM dump loop. */
#define BOND_SLOT_COUNT      10      /* bond table holds 10 entries          */
#define BOND_IRK_LEN         0x10    /* identity resolving key, 16 B         */
#define BOND_ADDR_LEN        6       /* device address, 6 B                  */
#define BOND_LTK_REC_LEN     0x1C    /* {ltk[16], ediv[2], rand[8], ...}     */

/* GAP-bond NV item selectors, addressed via log_emit_v(0x10, handle, item).
 * The per-slot MAC/LTK items are biased by slot*6 (the OEM uses a signed
 * 16x16 multiply, smulbb, to form slot*6). */
#define BOND_NV_IRK          2       /* whole-store IRK item                 */
#define BOND_NV_ADDR_MODE    4       /* whole-store address mode (1 B)       */
#define BOND_NV_SLOT_MAC     0x20    /* per-slot base: MAC  = 0x20 + slot*6  */
#define BOND_NV_SLOT_LTK     0x21    /* per-slot base: LTK  = 0x21 + slot*6  */

#define BOND_NV_ICALL_SVC    0x10    /* ICall service id (== log_emit_v's)   */

/* Opaque GAP-bond-storage request handle embedded in the OEM literal pool
 * at 0x0000A260. It is the Thumb address of the bond-manager get-param
 * thunk (FUN_00024EF8); the bond manager passes it verbatim as the ICall
 * request token. Kept as the raw constant — it is a runtime selector, not
 * a value we synthesise — mirroring the OEM image. */
#define BOND_STORE_HANDLE    ((void *)(uintptr_t)0x00024EF9u)

/* OEM `fn` string at 0x0002A36E, passed as monitor_log's source-function
 * argument for the formatted (non-hexdump) lines. */
static const char K_FN_DUMP[] = "gap_bondmanager_print_all_bonds";

/* Runtime address-mode string table (RAM 0x200058E0), indexed by the low
 * two bits of the address-mode byte. Populated by the GAP init path; only
 * read here. */
extern const char *const g_bond_addr_mode_strings[4];   /* DAT_0000A268 -> 0x200058E0 */

/* "is every byte == fill" predicate, FUN_00025BFE. Returns 1 when all
 * `len` bytes at `buf` equal `fill` (an erased/empty slot), 0 otherwise
 * (including buf == NULL). */
extern int bytes_all_equal(const void *buf, unsigned int fill, unsigned int len);

/* Dump every bond slot to the debug console.
 *
 * Reads the whole-store IRK and address-mode items once, then loops the 10
 * slots; for each non-empty slot (MAC item present and not all-0xFF) it
 * emits the MAC, the LTK record (LTK bytes, ediv, rand, keysize), and a
 * trailing blank line.
 *
 * OEM @ 0x0000A07C. Always returns 0 (the OEM tail moves r9 == 0 into r0).
 *
 * OEM quirks preserved:
 *   - The LTK hex-dump count is the slot's keysize byte, so only `keysize`
 *     bytes of the 16-byte LTK are printed.
 *   - The "keysize" line's format string is literally "keysize" (no
 *     conversion specifier) yet still passes the keysize byte as a
 *     trailing argument; the value is formatted away.
 */
int bond_store_dump_log(void)
{
    uint8_t irk[BOND_IRK_LEN];
    uint8_t addr_mode;

    log_emit_v(BOND_NV_ICALL_SVC, (const char *)BOND_STORE_HANDLE,
               BOND_NV_IRK, BOND_IRK_LEN, irk);
    monitor_log(K_FILE, 0xDD, 0, BOND_LOG_INFO, 0,
                "IRK", "", irk, BOND_IRK_LEN);

    log_emit_v(BOND_NV_ICALL_SVC, (const char *)BOND_STORE_HANDLE,
               BOND_NV_ADDR_MODE, 1, &addr_mode);
    monitor_log(K_FILE, 0xE0, K_FN_DUMP, BOND_LOG_INFO, "Address-mode: %s",
                g_bond_addr_mode_strings[addr_mode & 3]);

    for (int slot = 0; slot < BOND_SLOT_COUNT; slot++) {
        /* OEM forms slot*6 via a signed 16x16 multiply (smulbb). */
        int item_base = (int)(short)slot * BOND_ADDR_LEN;

        uint8_t mac[BOND_ADDR_LEN];
        if (log_emit_v(BOND_NV_ICALL_SVC, (const char *)BOND_STORE_HANDLE,
                       item_base + BOND_NV_SLOT_MAC, BOND_ADDR_LEN, mac) != 0) {
            continue;
        }
        if (bytes_all_equal(mac, 0xFF, BOND_ADDR_LEN) != 0) {
            continue;   /* erased slot */
        }

        monitor_log(K_FILE, 0xE8, 0, BOND_LOG_INFO, 0,
                    "MAC", ":", mac, BOND_ADDR_LEN);

        /* LTK record: ltk[16] @ +0x00, ediv @ +0x10, rand[8] @ +0x12,
         * keysize @ +0x1A. */
        uint8_t ltk_rec[BOND_LTK_REC_LEN];
        log_emit_v(BOND_NV_ICALL_SVC, (const char *)BOND_STORE_HANDLE,
                   item_base + BOND_NV_SLOT_LTK, BOND_LTK_REC_LEN, ltk_rec);

        uint16_t ediv;
        uint8_t  keysize = ltk_rec[0x1A];
        __builtin_memcpy(&ediv, &ltk_rec[0x10], sizeof ediv);

        monitor_log(K_FILE, 0xEB, K_FN_DUMP, BOND_LOG_INFO, "> Local LTK");
        /* OEM dumps only `keysize` bytes of the LTK. */
        monitor_log(K_FILE, 0xEC, 0, BOND_LOG_INFO, 0,
                    "LTK", "", ltk_rec, keysize);
        monitor_log(K_FILE, 0xED, K_FN_DUMP, BOND_LOG_INFO, "div 0x%04x", ediv);
        monitor_log(K_FILE, 0xEE, 0, BOND_LOG_INFO, 0,
                    "rand", "", &ltk_rec[0x12], 8);
        /* OEM passes keysize as a trailing arg even though the format has
         * no conversion specifier. */
        monitor_log(K_FILE, 0xEF, K_FN_DUMP, BOND_LOG_INFO, "keysize", keysize);
        monitor_log(K_FILE, 0xF0, K_FN_DUMP, BOND_LOG_INFO, "");
    }

    return 0;
}

/* ---- bond_save_event_cb ------------------------------------------- */

/* Persisted "last bond-manager state" byte at RAM 0x200058DC; the OEM
 * stores the raw status code here on every callback. */
extern volatile uint8_t g_bond_last_state;   /* DAT_00014EDC -> 0x200058DC */

/* OEM `fn` string at 0x0002A3AE for this callback's monitor_log lines. */
static const char K_FN_PAIRSTATE[] = "gap_bondmanager_process_pairstate";

/* Notification codes posted to the bluetoothtask control queue. */
#define BOND_NOTIFY_OK    0x0A
#define BOND_NOTIFY_FAIL  0x0B

/* Pairing / bond-completion callback. `status` selects the path:
 *
 *   1  bond-store update in progress — notify FAIL only when `has_data`.
 *   2  encryption result — when `has_data`, notify FAIL and log the code.
 *   3  bond-save result — `has_data != 0` logs the failure and notifies
 *      FAIL; otherwise notifies OK and logs success.
 *
 * Any other status is ignored. The raw status is always latched into
 * g_bond_last_state first.
 *
 * OEM @ 0x00014E10. Dispatched from the bluetoothtask loop (FUN_000067C8)
 * on user-message type 0x03 with three arguments. The OEM body uses only
 * `status` (r0) and `has_data` (r2, the byte at message offset 4) for every
 * decision and every logged value — there is no fourth argument.
 */
void bond_save_event_cb(int status, unsigned short p2, int has_data)
{
    (void)p2;   /* loaded by the caller, unused by the OEM body */

    g_bond_last_state = (uint8_t)status;

    if (status == 1) {
        if (has_data != 0) {
            bleware_control_event_post(BOND_NOTIFY_FAIL);
        }
        return;
    }

    if (status == 2) {
        if (has_data == 0) {
            return;
        }
        bleware_control_event_post(BOND_NOTIFY_FAIL);
        monitor_log(K_FILE, 0x89, K_FN_PAIRSTATE, BOND_LOG_ERR,
                    "Encryption failed: %d", has_data);
        return;
    }

    if (status != 3) {
        return;
    }

    if (has_data != 0) {
        monitor_log(K_FILE, 0x9C, K_FN_PAIRSTATE, BOND_LOG_ERR,
                    "Bond save failed: %d", has_data);
        bleware_control_event_post(BOND_NOTIFY_FAIL);
        return;
    }

    bleware_control_event_post(BOND_NOTIFY_OK);
    monitor_log(K_FILE, 0x98, K_FN_PAIRSTATE, BOND_LOG_OK,
                "Bond save success");
}
