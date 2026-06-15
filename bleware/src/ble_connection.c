/* ble_connection.c — per-connection state accessors.
 *
 * The BLE stack tracks up to N concurrent connections. For each, bleware
 * keeps a 0x7C-byte state record holding the current
 * indicate-confirm sequence number, a per-record semaphore (so the
 * accessors below can run without coarse-grained locking), the
 * connection's session key, and other fields not yet decoded.
 *
 * Table base: RAM `g_ble_connection_table` (= flash literals
 * 0x000229AC / 0x0002314C / 0x00023200 / 0x00023DFC — all hold
 * 0x20004158). Stride 0x7C.
 *
 * Observed entry layout (only the fields these accessors touch):
 *
 *   +0x00 u16    indicate_seq        — next expected confirm seq num
 *   +0x02 u16    conn_latency          — slave latency
 *   +0x04 u16    conn_interval         — connection interval
 *   +0x06 u16    conn_timeout          — supervision timeout
 *   +0x24 void  *sem                  — Semaphore handle (pend/post)
 *   +0x48 u16    conn_handle           — set when entry is allocated
 *   +0x50 void  *session_key           — 16-byte AES session key buf
 *   +0x56 u8[6]  peer_addr             — peer BD address
 *   +0x74 u32    keepalive_ticks       — last-activity tick stamp
 *
 * Functions decoded:
 *
 *   `indicate_seq_peek`                @ 0x00022970
 *   `indicate_seq_advance`             @ 0x00023114
 *   `ble_connection_get_session_key`   @ 0x00023DCC
 *   `ble_connection_set_session_key`   @ 0x000231C8
 *
 * Each follows the same shape:
 *   1. resolve `entry = &table[conn]`
 *   2. Semaphore_pend(entry->sem, FOREVER)
 *   3. if (entry->conn_handle == conn) { do the read/write; rc = 0 }
 *      else                              { rc = -1 }
 *   4. Semaphore_post(entry->sem)
 *   5. return rc (read fns return the value instead)
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bleware.h"

/* ROM thunks (TI-RTOS Semaphore module). */
extern int  ti_semaphore_pend(void *sem, uint32_t timeout_ticks);  /* @ 0x1002BFB0 */
extern void ti_semaphore_post(void *sem);                          /* @ 0x1002CD20 */

/* Pseudo-random u15 helper (src/lcg_random.c) — used to seed each
 * advance with a fresh indicate sequence number. */
extern uint32_t lcg_random_u15(void);

struct ble_connection_state {
    uint16_t  indicate_seq;       /* +0x00 */
    uint8_t   pad02[0x22];        /* +0x02..0x23 */
    void     *sem;                /* +0x24 */
    uint8_t   pad28[0x20];        /* +0x28..0x47 */
    uint16_t  conn_handle;        /* +0x48 — sanity check */
    uint8_t   pad4a[6];           /* +0x4A..0x4F */
    void     *session_key;        /* +0x50 */
    uint8_t   pad54[0x25];        /* +0x54..0x78 */
    uint8_t   backoffice_authed;  /* +0x79 — set when backoffice has
                                   * pinned a session key for this conn */
    uint8_t   pad7a[2];           /* +0x7A..0x7B */
};

extern struct ble_connection_state * const g_ble_connection_table;

#define SEM_TIMEOUT_FOREVER  0xFFFFFFFFu

int indicate_seq_peek(uint16_t conn, uint16_t *out_seq)
{
    struct ble_connection_state *e = &g_ble_connection_table[conn];
    int rc;

    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    if (conn == e->conn_handle) {
        *out_seq = e->indicate_seq;
        rc = 0;
    } else {
        rc = -1;
    }
    ti_semaphore_post(e->sem);
    return rc;
}

int indicate_seq_advance(uint16_t conn)
{
    struct ble_connection_state *e = &g_ble_connection_table[conn];
    int rc;

    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    if (conn == e->conn_handle) {
        e->indicate_seq = (uint16_t)lcg_random_u15();
        rc = 0;
    } else {
        rc = -1;
    }
    ti_semaphore_post(e->sem);
    return rc;
}

int ble_connection_get_session_key(uint32_t conn)
{
    struct ble_connection_state *e = &g_ble_connection_table[conn];
    int key;

    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    if ((uint16_t)conn == e->conn_handle) {
        key = (int)(uintptr_t)e->session_key;
    } else {
        key = 0;
    }
    ti_semaphore_post(e->sem);
    return key;
}

/* Check if a BLE connection handle is currently active (conn_handle
 * sanity-check under semaphore). OEM @ 0x00023D30 (36 B). */
int ble_connection_is_active(uint32_t conn)
{
    struct ble_connection_state *e = &g_ble_connection_table[conn];

    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    uint16_t stored_conn = e->conn_handle;
    ti_semaphore_post(e->sem);

    return (conn == stored_conn) ? 1 : 0;
}

/* Touch (keep-alive) a BLE connection entry: stamp the per-connection
 * last-activity timestamp (+0x74) with the ROM monotonic tick count,
 * under the entry semaphore. OEM @ 0x00023608.
 *
 * The OEM always returns -1 (even on the matching path); the sole
 * caller discards the result, so this is modelled as void. */
void ble_connection_touch(uint32_t conn)
{
    /* ROM monotonic tick source (flash thunk 0x00027E58 -> ROM
     * 0x1002DC10); returns the current keep-alive timestamp value. */
    extern uint32_t rom_clock_get_ticks(void);

    struct ble_connection_state *e = &g_ble_connection_table[conn];

    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    if (conn == e->conn_handle) {
        *(uint32_t *)((uint8_t *)e + 0x74) = rom_clock_get_ticks();
    }
    ti_semaphore_post(e->sem);
}

/* Backoffice on-success hook — build and persist the M-ID record
 * (slot 124 / 0x7C). The caller passes the keyed word that lands at
 * record+0x10; the "M-ID" tag word is placed at record+0x18; the
 * CRC-32/zlib over the first 28 bytes is stored at record+0x1C.
 * OEM @ 0x00022BE8. */
void backoffice_on_success_hook(uint32_t key_word)
{
    extern uint32_t g_backoffice_success_tag;
    uint8_t buf[32];

    memset(buf, 0, 32);
    memcpy(buf + 0x10, &key_word, 4);
    memcpy(buf + 0x18, &g_backoffice_success_tag, 4);
    uint32_t crc = crc32_le(0xFFFFFFFFu, buf, 28);
    memcpy(buf + 0x1C, &crc, 4);
    secrets_record_write_verify(0x7C, buf);
}

/* Read a per-connection state byte at offset 0x65. Same Semaphore_pend/
 * conn-sanity-check / Semaphore_post template as the other accessors.
 * Returns 0 on success (byte written to *out_byte), -1 if conn doesn't
 * match. OEM at 0x000228B0 (60 B). */
int ble_conn_state_byte(uint32_t conn, uint8_t *out_byte)
{
    struct ble_connection_state *e = &g_ble_connection_table[conn];
    int rc;

    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    if (conn == e->conn_handle) {
        *out_byte = *(uint8_t *)((uint8_t *)e + 0x65);
        rc = 0;
    } else {
        rc = -1;
    }
    ti_semaphore_post(e->sem);
    return rc;
}

/* Store a 16-bit value at connection-record offset 0x54. Same accessor
 * template. The state machine writes the per-connection request value here
 * (see sm_handler_ev10) before signalling the SSP fetch. The exact field
 * meaning is not yet pinned down, so it is named by offset. Returns 0 on
 * success, -1 if conn doesn't match. OEM @ 0x0002309C. */
int ble_connection_set_field54(uint32_t conn, uint16_t value)
{
    struct ble_connection_state *e = &g_ble_connection_table[conn];
    int rc;

    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    if (conn == e->conn_handle) {
        *(uint16_t *)((uint8_t *)e + 0x54) = value;
        rc = 0;
    } else {
        rc = -1;
    }
    ti_semaphore_post(e->sem);
    return rc;
}

/* Store two 32-bit words (the first 8 bytes of the SM event payload) at
 * connection-record offsets 0x6C and 0x70. Same accessor template; offset-
 * named pending field-meaning identification. Returns 0 on success, -1 if
 * conn doesn't match. OEM @ 0x0002168C. */
int ble_connection_set_field6c(uint32_t conn, const uint32_t *words2)
{
    struct ble_connection_state *e = &g_ble_connection_table[conn];
    int rc;

    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    if (conn == e->conn_handle) {
        *(uint32_t *)((uint8_t *)e + 0x6C) = words2[0];
        *(uint32_t *)((uint8_t *)e + 0x70) = words2[1];
        rc = 0;
    } else {
        rc = -1;
    }
    ti_semaphore_post(e->sem);
    return rc;
}

/* Request a connection-parameter update for `conn`. Builds the param-update
 * request (conn, interval ×2, latency, timeout) on the stack and issues it via
 * the generic ICall request helper to the GAP service. If the stack replies
 * 0x11 (update must be re-issued asynchronously), a 12-byte message carrying
 * the conn handle is allocated and posted to the worker queue. Returns the
 * ICall/queue status (the sole caller, sm_handler_ev1a, discards it).
 * OEM @ 0x000211F8. Field semantics (latency/timeout) are inferred from the
 * standard BLE connection-parameter layout. */
int ble_connection_param_update_request(uint16_t conn, uint16_t latency,
                                        uint16_t timeout, uint32_t interval,
                                        uint16_t interval_dup)
{
    /* log_emit_v (FUN_0001AC6C, generic ICall request) and monitor_alloc
     * (FUN_00013470) are declared in bleware.h. */
    extern const uint8_t g_conn_param_update_desc[];   /* DAT_0002123C */
    extern int        task_queue_post(const void *q, void **msg);  /* FUN_00025126 */
    extern const void *g_conn_param_update_queue;      /* DAT_00021240 */

    struct {
        uint16_t conn;          /* +0x00 */
        uint16_t interval;      /* +0x02 */
        uint16_t interval_dup;  /* +0x04 */
        uint16_t latency;       /* +0x06 */
        uint16_t timeout;       /* +0x08 */
        uint16_t interval_hi;   /* +0x0A */
    } req;

    req.conn         = conn;
    req.interval     = (uint16_t)interval;
    req.interval_dup = interval_dup;
    req.latency      = latency;
    req.timeout      = timeout;
    req.interval_hi  = (uint16_t)(interval >> 16);

    uint8_t reply = (uint8_t)log_emit_v(0x10, (const char *)g_conn_param_update_desc, &req);
    if (reply == 0x11) {
        void *msg = monitor_alloc(0xc);
        if (msg == NULL) {
            return 0;
        }
        *(uint16_t *)((uint8_t *)msg + 8) = conn;
        return task_queue_post(g_conn_param_update_queue, &msg);
    }
    return 0;
}

int ble_connection_set_session_key(uint32_t conn, const void *key_16)
{
    struct ble_connection_state *e = &g_ble_connection_table[conn];
    int rc;

    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    if ((uint16_t)conn == e->conn_handle) {
        e->session_key = (void *)(uintptr_t)key_16;
        rc = 0;
    } else {
        rc = -1;
    }
    ti_semaphore_post(e->sem);
    return rc;
}

/* `backoffice_auth_session_init` — pin a session key for a connection
 * authenticated through the backoffice GATT service (svc 0x5500 char
 * 0x01). Sets the `backoffice_authed` flag unconditionally inside the
 * lock (even on stale conn — matching OEM behavior); then, only if
 * the conn handle still matches, pins the key and notifies the state
 * machine of the new auth state. OEM @ 0x0001A218. */
/* `state_machine_post` declared in bleware.h. */

#define MAX_BACKOFFICE_AUTH_CONN  3   /* OEM check `conn < 3` */

int backoffice_auth_session_init(uint16_t conn, const void *session_key)
{
    struct ble_connection_state *e;
    uint16_t notify = conn;
    int      rc     = -1;

    if (conn >= MAX_BACKOFFICE_AUTH_CONN || session_key == NULL) {
        return -1;
    }

    e = &g_ble_connection_table[conn];
    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    e->backoffice_authed = 1;
    if (conn == e->conn_handle) {
        e->session_key = (void *)(uintptr_t)session_key;
        state_machine_post(0x18, &notify, sizeof notify);
        rc = 0;
    }
    ti_semaphore_post(e->sem);
    return rc;
}

/* Read the ATT MTU for a connection and clamp the caller's length
 * to that value. Per-connection template: Semaphore_pend, check
 * conn_handle, read mtu at offset 0x5C, store, Semaphore_post.
 * Returns 0 on success, -1 if conn handle doesn't match.
 * OEM @ 0x000229B0 (58 B). */
int att_mtu_clamp(uint32_t conn, uint16_t *len_inout)
{
    struct ble_connection_state *e = &g_ble_connection_table[conn];

    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    if (conn == e->conn_handle) {
        *len_inout = *(uint16_t *)((uint8_t *)e + 0x5C);
        ti_semaphore_post(e->sem);
        return 0;
    }
    ti_semaphore_post(e->sem);
    return -1;
}

/* Return the count of active BLE connections. OEM @ 0x00026A7C: the
 * argument is ignored; the real OEM issues a synchronous ICall query
 * (FUN_0001AC6C(0x10, ...) -> FUN_00025100 / FUN_0001134C with a 1000 ms
 * timeout) and returns the (uint16) reply. The local 3-slot scan below
 * is a behavioural approximation only — the faithful RPC body is not yet
 * reconstructed (depends on the ICall request/reply infrastructure). */
int ble_connection_count(int unused)
{
    int count = 0;
    (void)unused;
    for (int i = 0; i < 3; i++) {
        if (ble_connection_is_active((uint32_t)i) != 0) {
            count++;
        }
    }
    return count;
}

/* Check if connection slot `index` has a valid conn_handle. The OEM
 * ble_info command (FUN_00007A58) tests presence by calling
 * ble_connection_is_active (FUN_00023D30) directly. */
int ble_connection_present(int index)
{
    return ble_connection_is_active((uint32_t)index);
}

/* Read the 6-byte peer BLE address for connection `index`. The OEM
 * copies a 4-byte word at entry+0x56 followed by a 2-byte halfword at
 * entry+0x5A into the caller's buffer (6 bytes total). OEM @ 0x000201E8.
 *
 * The OEM returns 0 on a matching conn / -1 otherwise; the sole caller
 * discards the result, so this is modelled as void. */
void ble_connection_addr(int index, uint8_t *dst)
{
    struct ble_connection_state *e = &g_ble_connection_table[index];
    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    if (index == e->conn_handle) {
        memcpy(dst, (uint8_t *)e + 0x56, 6);
    }
    ti_semaphore_post(e->sem);
}

/* Read connection parameters (interval, latency, timeout) for `index`.
 * The OEM reads interval at entry+0x04, latency at entry+0x02 and
 * timeout at entry+0x06, and skips any output whose pointer is NULL.
 * OEM @ 0x0001F640. (Returns 0/-1 in the OEM; the sole caller passes
 * all three out-pointers and discards the result, so modelled void.) */
void ble_connection_params(int index, uint16_t *interval,
                           uint16_t *latency, uint16_t *timeout)
{
    struct ble_connection_state *e = &g_ble_connection_table[index];
    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    if (index == e->conn_handle) {
        if (interval != NULL) {
            *interval = *(uint16_t *)((uint8_t *)e + 0x04);
        }
        if (latency != NULL) {
            *latency = *(uint16_t *)((uint8_t *)e + 0x02);
        }
        if (timeout != NULL) {
            *timeout = *(uint16_t *)((uint8_t *)e + 0x06);
        }
    }
    ti_semaphore_post(e->sem);
}

/* Check if connection `index` is a rider-app connection.
 * OEM @ ~0x00025E74. */
int ble_connection_is_rider_app(int index)
{
    struct ble_connection_state *e = &g_ble_connection_table[index];
    int result = 0;
    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    if (index == e->conn_handle) {
        result = (*(uint8_t *)((uint8_t *)e + 0x64) != 0) ? 1 : 0;
    }
    ti_semaphore_post(e->sem);
    return result;
}

/* Get the local device BLE address. `addr_type` selects public (0) or
 * random (1) address. The OEM ble_info command (FUN_00007A58) obtains
 * this by tail-calling the ROM GAP accessor (flash thunk 0x00027FA0 ->
 * ROM 0x100221C4), which returns a pointer to the 6-byte address. */
uint8_t *ble_device_address(int addr_type)
{
    /* ROM GAP get-device-address (flash thunk 0x00027FA0 -> ROM
     * 0x100221C4); returns a pointer to the 6-byte BD address. */
    extern uint8_t *rom_gap_get_dev_address(int type);

    return rom_gap_get_dev_address(addr_type);
}

/* ---- xs3_hci.c PHY accessors (per-connection PHY fields) -------------
 *
 * The PHY state for each link lives in three bytes inside the same
 * 0x7C-byte connection record used by the accessors above:
 *
 *   +0x66 u8   phy_requested   — PHY the host wants the link to use
 *   +0x67 u8   phy_reapply     — re-apply request counter (bump => the
 *                                 controller is asked to re-negotiate)
 *   +0x68 u8   phy_current     — PHY the link is currently running
 *
 * All three follow the standard template (pend, conn-handle check,
 * read/write, post) and are the strong definitions that override the
 * WEAK_NOOP placeholders in hal_stubs.S. The sole caller of all three
 * is hci_handle_phy_update_event (src/xs3_hci.c, OEM @ 0x0001C684). */

/* Read the connection's current PHY (record+0x68) into *out_phy.
 * Returns 0 on a matching conn handle, -1 otherwise. OEM @ 0x00022870. */
int conn_phy_get_current(uint16_t conn, uint8_t *out_phy)
{
    struct ble_connection_state *e = &g_ble_connection_table[conn];
    int rc;

    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    if (conn == e->conn_handle) {
        *out_phy = *(uint8_t *)((uint8_t *)e + 0x68);
        rc = 0;
    } else {
        rc = -1;
    }
    ti_semaphore_post(e->sem);
    return rc;
}

/* Latch the host-requested PHY (record+0x66) and clear the re-apply
 * counter (record+0x67). Returns 0 on a matching conn handle, -1
 * otherwise. OEM @ 0x000228F0. */
int conn_phy_set_requested(uint16_t conn, uint8_t phy)
{
    struct ble_connection_state *e = &g_ble_connection_table[conn];
    int rc;

    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    if (conn == e->conn_handle) {
        *(uint8_t *)((uint8_t *)e + 0x67) = 0;
        *(uint8_t *)((uint8_t *)e + 0x66) = phy;
        rc = 0;
    } else {
        rc = -1;
    }
    ti_semaphore_post(e->sem);
    return rc;
}

/* Bump the re-apply counter (record+0x67), asking the controller to
 * re-negotiate the PHY for this link. Returns 0 on a matching conn
 * handle, -1 otherwise. OEM @ 0x00022050.
 *
 * The OEM loads the table base from a literal holding (base - 4) and
 * immediately adds 4 back before indexing — a codegen artefact that
 * resolves to the same g_ble_connection_table base used everywhere
 * else; modelled here as the plain indexed access. */
int conn_phy_reapply(uint16_t conn)
{
    struct ble_connection_state *e = &g_ble_connection_table[conn];
    int rc;

    ti_semaphore_pend(e->sem, SEM_TIMEOUT_FOREVER);
    if (conn == e->conn_handle) {
        *(uint8_t *)((uint8_t *)e + 0x67) += 1;
        rc = 0;
    } else {
        rc = -1;
    }
    ti_semaphore_post(e->sem);
    return rc;
}
