/* xs3_hci.c — HCI / LE controller event glue.
 *
 * OEM source file: source/xs3_hci.c (path string embedded at flash
 * 0x0001C6C8). Only one function in this translation unit:
 *
 *   hci_handle_phy_update_event   OEM @ 0x0001C684  (68 B body)
 *
 * It is the application-side handler for the HCI LE PHY-Update-Complete
 * meta-event. The OEM build registers it under the callback name
 * "on_hci_phy_update_completed" (string at flash 0x0002AF48, passed as a
 * varargs argument to monitor_log on the failure path).
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* Location-aware log helper — OEM FUN_00006D90. Same prototype the rest
 * of the bleware tree uses (file, line, fn, level, fmt, ...). */
extern void monitor_log(const char *file, int line, const char *fn,
                        int level, const char *fmt, ...);

/* ---- Per-connection PHY accessors (src/ble_connection.c, conn table at
 * the 0x7C-byte-stride record array) -------------------------------------
 *
 * These three helpers all take the per-record semaphore (record+0x24),
 * sanity-check that the requested conn handle matches the record's own
 * handle (record+0x48), and return 0 on success or -1 on a handle
 * mismatch. They are not yet decoded in their own TU; declared here and
 * stubbed weak in hal_stubs.S. */

/* OEM FUN_00022870 — read the connection's current PHY (record+0x68)
 * into *out_phy. */
extern int conn_phy_get_current(uint16_t conn, uint8_t *out_phy);

/* OEM FUN_000228F0 — store the requested PHY (record+0x66) and clear the
 * re-apply counter (record+0x67). */
extern int conn_phy_set_requested(uint16_t conn, uint8_t phy);

/* OEM FUN_00022050 — bump the re-apply counter (record+0x67), i.e.
 * request that the controller re-negotiate the PHY for this link. */
extern int conn_phy_reapply(uint16_t conn);

/* HCI LE PHY-Update-Complete event handler.
 *
 * `evt` points at the decoded HCI meta-event record:
 *   +0x02 u8   sub-event opcode (0x0C = LE PHY Update Complete)
 *   +0x03 u8   status (0 = success)
 *   +0x04 u16  connection handle
 *   +0x07 u8   new PHY
 *
 * The OEM signature is (evt, _, _, slot) — only `evt` is used; the fourth
 * argument's incoming-register save slot is immediately reused by
 * conn_phy_get_current as the &out_phy scratch buffer (the compiler
 * folded the read-back into the pushed-argument area). That register-slot
 * reuse is preserved here via the `cur_phy` local: it is seeded from the
 * incoming `slot` value and then overwritten by conn_phy_get_current.
 *
 * Behaviour, on a 0x0C sub-event:
 *   - log "PHY Change failure" when status != 0;
 *   - read back the link's current PHY;
 *   - on success (status == 0) latch the new PHY as the requested PHY;
 *   - if the new PHY differs from the link's current PHY, ask the
 *     controller to re-apply it.
 *
 * OEM @ 0x0001C684. */
void hci_handle_phy_update_event(const uint8_t *evt, uint32_t a2,
                                 uint32_t a3, uint32_t slot)
{
    (void)a2;
    (void)a3;

    if (evt[2] != 0x0C) {
        return;
    }

    /* The fourth argument's stack save-slot doubles as the read-back
     * buffer for conn_phy_get_current — preserved from the OEM codegen. */
    uint8_t cur_phy = (uint8_t)slot;

    if (evt[3] != 0) {
        monitor_log("source/xs3_hci.c", 0x6B, "on_hci_phy_update_completed",
                    0, "PHY Change failure");
    }

    uint16_t conn = (uint16_t)(evt[4] | ((uint16_t)evt[5] << 8));
    conn_phy_get_current(conn, &cur_phy);

    if (evt[3] == 0) {
        conn_phy_set_requested(conn, evt[7]);
    }

    if (evt[7] != cur_phy) {
        conn_phy_reapply(conn);
    }
}
