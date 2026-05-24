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
 *   +0x24 void  *sem                  — Semaphore handle (pend/post)
 *   +0x48 u16    conn_handle           — set when entry is allocated
 *   +0x50 void  *session_key           — 16-byte AES session key buf
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
