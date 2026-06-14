/* xs3_gap_adv.c — GAP legacy + extended advertising configuration.
 *
 * OEM source: source/xs3_gap_adv.c (path string embedded twice at flash
 * 0x00012F70 and 0x00016B84).
 *
 * This TU owns the device's two advertising sets and the device-name /
 * BLE-address plumbing behind them:
 *
 *   set #1  legacy connectable advertising  (apply: gap_adv_apply_set1)
 *   set #2  secondary / extended advertising (apply: gap_adv_apply_set2)
 *
 * Both apply functions push the advertising parameters, scan-response,
 * advertising data, and enable command into the BLE5-Stack through the
 * ICall "service message" facility — the same `log_emit_v(0x10, ...)`
 * primitive used elsewhere for structured logging. Here the second
 * argument is a flash/ROM-resident GAP command descriptor rather than a
 * format string; the remaining arguments are the per-command operands
 * (parameter id, length, data pointer). A non-zero status from a
 * parameter-push is fatal and routed to util_assert_fail (an infinite
 * spin), exactly as in the OEM image.
 *
 * The advertising configuration lives in a single RAM struct at
 * `g_gap_adv_cfg` (RAM 0x20005290). Only the byte offsets this TU
 * touches are modelled:
 *
 *   +0x00  u8   set #1 adv-set handle (0xFF ⇒ not yet created)
 *   +0x01  u8   set #2 adv-set handle (0xFF ⇒ not yet created)
 *   +0x02  u8   set #1 "ready" flag   (1 ⇒ enabled OK)
 *   +0x03  u8   set #2 "ready" flag   (1 ⇒ enabled OK)
 *   +0x0C  u8[] device-name copy (16 B, mirror of +0x1E)
 *   +0x1C  u8[] set #1 scan-response data (0x18 B)
 *   +0x1E  u8[] device-name copy (16 B)
 *   +0x34  u8[] set #2 advertising data (0x1D B)
 *   +0x51  u8[] set #1 advertising data (0x1F B)
 *   +0x56  u8[8] formatted device-name string "ES3-<MAC>"
 *
 * OEM functions in this file:
 *   gap_adv_apply_set1          @ 0x00012EB0  (192 B)
 *   gap_adv_apply_set2          @ 0x00016B04  (128 B)
 *   gap_adv_set_device_name     @ 0x00017F90  (118 B)
 *   gap_adv_init_apply          @ 0x00027024  ( 16 B)
 *   gap_adv_set_addr_and_restart@ 0x0001CF14  ( 94 B)
 *   gap_adv_set_random_static_addr @ 0x00025A04 ( 32 B)
 *   bdaddr_get_reversed         @ 0x00024AE0  ( 40 B)
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* ICall service-message primitive — service id 0x10 reaches both the
 * structured-logger and the GAP command facility. Declared in bleware.h
 * as `log_emit_v(uint32_t service_id, const char *fmt, ...)`; the GAP
 * call sites pass a command descriptor in the `fmt` slot. */

/* Assert/panic handler (src/util.c, FUN_00020F4C). Logs the failing
 * location and spins forever; not in bleware.h by codebase convention. */
extern void util_assert_fail(uint32_t error_code, int line,
                             const char *fn_name);

/* TI CGT memcpy (FUN_00018654). */
extern void *memcpy(void *dst, const void *src, unsigned int n);

/* snprintf-style formatter (FUN_00022A30): writes the formatted string
 * to `out` and NUL-terminates. Not yet named/decoded elsewhere. */
extern void bleware_snprintf(char *out, const char *fmt, ...);

/* Disable-advertising helpers for the two sets are defined below
 * (gap_adv_disable_set1 @ 0x00024FE8, gap_adv_disable_set2 @ 0x00025010).
 * Forward-declared here because gap_adv_set_addr_and_restart calls them
 * before they appear in source order. */
int gap_adv_disable_set1(void);   /* OEM @ 0x00024FE8 */
int gap_adv_disable_set2(void);   /* OEM @ 0x00025010 */

/* The advertising configuration struct (RAM 0x20005290). */
extern uint8_t g_gap_adv_cfg[];

/* CC2642R1F FCFG1 BLE MAC address (device BD_ADDR): 6 bytes stored
 * little-endian as a word + halfword at flash-mapped reg 0x500012E8. */
#define FCFG1_BLE_MAC ((const volatile uint8_t *)0x500012E8u)

/* GAP advertising command descriptors (flash/ROM resident). These are
 * the opaque second argument to the ICall service message — each selects
 * a GAP advertising sub-command. Preserved as literal addresses because
 * the OEM has no symbol for them. */
#define GAP_ADV_CMD_CREATE_SET   ((const char *)0x10010199u)  /* create adv set */
#define GAP_ADV_CMD_SET_DATA     ((const char *)0x00013A11u)  /* set adv/scan-rsp data (set #1) */
#define GAP_ADV_CMD_LOAD_BUF     ((const char *)0x1001EF1Du)  /* load buffered data */
#define GAP_ADV_CMD_ENABLE       ((const char *)0x100174EDu)  /* enable advertising */
#define GAP_ADV_CMD_DISABLE      ((const char *)0x10020265u)  /* disable advertising */
#define GAP_ADV_CMD_SET_ADDR     ((const char *)0x1002455Du)  /* set random address */

/* Command operand templates in flash (random-address command and the
 * per-set parameter blocks). */
#define GAP_ADV_PARAMS_TEMPLATE  ((const char *)0x00026C43u)
#define GAP_ADV_SET1_PARAM_BLK   ((const char *)0x0002B154u)
#define GAP_ADV_SET2_PARAM_BLK   ((const char *)0x0002B170u)  /* SET1 + 0x1C */

/* Adv-set struct field offsets. */
#define ADV_OFF_HANDLE1   0x00u
#define ADV_OFF_HANDLE2   0x01u
#define ADV_OFF_READY1    0x02u
#define ADV_OFF_READY2    0x03u
#define ADV_OFF_NAME_A    0x0Cu   /* device-name copy A (16 B) */
#define ADV_OFF_SET1_SCAN 0x1Cu   /* set #1 scan-response data */
#define ADV_OFF_NAME_B    0x1Eu   /* device-name copy B (16 B) */
#define ADV_OFF_SET2_DATA 0x34u   /* set #2 advertising data */
#define ADV_OFF_SET1_DATA 0x51u   /* set #1 advertising data */
#define ADV_OFF_NAME_STR  0x56u   /* formatted "ES3-<MAC>" string */

#define ADV_HANDLE_NONE   0xFFu   /* sentinel for "set not created yet" */

/* Read the 6-byte BD_ADDR from FCFG1 and byte-reverse it into `out`.
 *
 * The hardware stores the address little-endian (LSB first) as a 32-bit
 * word at +0 plus a 16-bit halfword at +4; the BLE adv/random-address
 * commands expect big-endian (MSB first), so the 6 bytes are reversed in
 * place: swap [0]<->[5], [1]<->[4], [2]<->[3].
 *
 * OEM @ 0x00024AE0. Returns the caller's buffer pointer (r0 is preserved
 * across the swap loop). */
uint8_t *bdaddr_get_reversed(uint8_t *out)
{
    /* Pull the address as the OEM does: a word then a halfword, so the
     * 6 LE bytes land at out[0..5]. Direct loads (ldr/ldrh), not memcpy. */
    *(uint32_t *)out       = *(const volatile uint32_t *)(FCFG1_BLE_MAC);
    *(uint16_t *)(out + 4) = *(const volatile uint16_t *)(FCFG1_BLE_MAC + 4);

    /* In-place byte reverse over the 6 bytes. The OEM walks a low index
     * up and a high index down (both masked to a byte) until they cross. */
    unsigned lo = 0;
    unsigned hi = 5;
    while ((int)hi > (int)lo) {
        uint8_t t = out[hi];
        out[hi] = out[lo];
        out[lo] = t;
        lo = (lo + 1) & 0xFF;
        hi = (hi - 1) & 0xFF;
    }
    return out;
}

/* Apply legacy advertising set #1.
 *
 * If the set has not been created yet (handle == 0xFF) it is created and
 * its scan-response (+0x1C, 0x18 B) and advertising data (+0x51, 0x1F B)
 * are pushed; each push status is checked and a non-zero result aborts
 * via util_assert_fail. The set is then enabled in a retry loop: enable
 * is attempted, and on failure the set is disabled and re-enabled until
 * it succeeds. The "ready" flag at +2 reflects the final enable result.
 *
 * Returns 0 on success, -1 on failure (set still uncreated / 0xFF).
 *
 * OEM @ 0x00012EB0. The source-line numbers in the assert calls
 * (0x7F/0x84/0x89) are the OEM xs3_gap_adv.c line numbers — preserved. */
int gap_adv_apply_set1(void)
{
    uint8_t *cfg = g_gap_adv_cfg;
    uint8_t  handle = cfg[ADV_OFF_HANDLE1];

    if (handle == ADV_HANDLE_NONE) {
        uint32_t rc;

        rc = log_emit_v(0x10, GAP_ADV_CMD_CREATE_SET,
                        GAP_ADV_PARAMS_TEMPLATE, GAP_ADV_SET1_PARAM_BLK, cfg);
        if ((uint8_t)rc != 0) {
            util_assert_fail(rc, 0x7F, "source/xs3_gap_adv.c");
        }

        rc = log_emit_v(0x10, GAP_ADV_CMD_SET_DATA, cfg[ADV_OFF_HANDLE1],
                        0, 0x1F, cfg + ADV_OFF_SET1_DATA);
        if (rc != 0) {
            util_assert_fail(rc, 0x84, "source/xs3_gap_adv.c");
        }

        rc = log_emit_v(0x10, GAP_ADV_CMD_SET_DATA, cfg[ADV_OFF_HANDLE1],
                        1, 0x18, cfg + ADV_OFF_SET1_SCAN);
        if (rc != 0) {
            util_assert_fail(rc, 0x89, "source/xs3_gap_adv.c");
        }

        log_emit_v(0x10, GAP_ADV_CMD_LOAD_BUF, cfg[ADV_OFF_HANDLE1],
                   0x92, 0x18, cfg + ADV_OFF_SET1_SCAN);
        handle = cfg[ADV_OFF_HANDLE1];
    }

    int rc;
    if (handle == ADV_HANDLE_NONE) {
        rc = -1;
    } else {
        for (;;) {
            rc = (int)log_emit_v(0x10, GAP_ADV_CMD_ENABLE, cfg[ADV_OFF_HANDLE1],
                                 0, 0);
            if (rc == 0) {
                break;
            }
            /* Enable failed — disable and retry. */
            log_emit_v(0x10, GAP_ADV_CMD_DISABLE, g_gap_adv_cfg[ADV_OFF_HANDLE1]);
            handle = g_gap_adv_cfg[ADV_OFF_HANDLE1];
            (void)handle;
        }
    }

    cfg[ADV_OFF_READY1] = (uint8_t)(rc == 0);
    return (rc == 0) ? 0 : -1;
}

/* Apply secondary / extended advertising set #2.
 *
 * Mirror of gap_adv_apply_set1 but keyed on the set #2 handle (+1) and
 * data block (+0x34, 0x1D B). Unlike set #1 there is no enable-retry
 * loop — a single enable command is issued. The "ready" flag at +3
 * reflects the result.
 *
 * Returns 0 on success, -1 on failure.
 *
 * OEM @ 0x00016B04. Assert line numbers 0xA1/0xA6 preserved. */
int gap_adv_apply_set2(void)
{
    uint8_t *cfg = g_gap_adv_cfg;
    uint8_t  handle = cfg[ADV_OFF_HANDLE2];

    if (handle == ADV_HANDLE_NONE) {
        uint32_t rc;

        rc = log_emit_v(0x10, GAP_ADV_CMD_CREATE_SET,
                        GAP_ADV_PARAMS_TEMPLATE, GAP_ADV_SET2_PARAM_BLK,
                        cfg + ADV_OFF_HANDLE2);
        if (rc != 0) {
            util_assert_fail(rc, 0xA1, "source/xs3_gap_adv.c");
        }

        rc = log_emit_v(0x10, GAP_ADV_CMD_SET_DATA, cfg[ADV_OFF_HANDLE2],
                        0, 0x1D, cfg + ADV_OFF_SET2_DATA);
        if (rc != 0) {
            util_assert_fail(rc, 0xA6, "source/xs3_gap_adv.c");
        }

        log_emit_v(0x10, GAP_ADV_CMD_LOAD_BUF, cfg[ADV_OFF_HANDLE2],
                   0x92, 0x1D, cfg + ADV_OFF_SET2_DATA);
        handle = cfg[ADV_OFF_HANDLE2];
    }

    int rc;
    if (handle == ADV_HANDLE_NONE) {
        rc = -1;
    } else {
        rc = (int)log_emit_v(0x10, GAP_ADV_CMD_ENABLE, cfg[ADV_OFF_HANDLE2],
                             0, 0);
    }

    cfg[ADV_OFF_READY2] = (uint8_t)(rc == 0);
    return (rc == 0) ? 0 : -1;
}

/* Build the advertising device name "ES3-<MAC>" and stamp it into the
 * adv config, then clear both "ready" flags so the next apply re-pushes
 * the data.
 *
 * The name is formatted as "ES3-AABBCCDDEEFF" from the byte-reversed
 * BD_ADDR: the format string at flash 0x00018008 is
 * "%c%c%c%c%02X%02X%02X%02X%02X%02X" (the leading 'F' at 0x00018007 is
 * skipped by the +1 in the OEM), fed 'E','S','3','-' followed by the six
 * MAC bytes. The 8-byte result is stored at +0x56/+0x5A and the same
 * bytes are copied (0x10 B from the formatting scratch) into the two
 * device-name slots at +0x1E and +0x0C.
 *
 * OEM @ 0x00017F90. */
void gap_adv_set_device_name(void)
{
    uint8_t  mac[6];
    char     name[16];          /* formatting scratch (local_28..) */

    bdaddr_get_reversed(mac);
    bleware_snprintf(name, "%c%c%c%c%02X%02X%02X%02X%02X%02X",
                     'E', 'S', '3', '-',
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    uint8_t *cfg = g_gap_adv_cfg;
    /* The OEM stores the first 8 bytes of the formatted name as two
     * words at +0x56/+0x5A (the visible "ES3-<MAC>" prefix). */
    memcpy(cfg + ADV_OFF_NAME_STR, name, 8);

    /* And copies 16 bytes of the scratch into both name slots. */
    memcpy(cfg + ADV_OFF_NAME_B, name, 0x10);
    memcpy(cfg + ADV_OFF_NAME_A, name, 0x10);

    cfg[ADV_OFF_READY2] = 0;
    cfg[ADV_OFF_READY1] = 0;
}

/* First-time advertising bring-up. If the config has not been
 * initialised (the +1 handle is still zero), build the device name and
 * apply set #1. Sole caller is the bluetoothtask init path (FUN_000067C8).
 *
 * OEM @ 0x00027024. */
void gap_adv_init_apply(void)
{
    if (g_gap_adv_cfg[ADV_OFF_HANDLE2] == 0) {
        gap_adv_set_device_name();
        gap_adv_apply_set1();
    }
}

/* Load a new 6-byte BLE address into the random-address command and
 * re-apply whichever advertising sets were previously enabled.
 *
 * When `addr` is non-NULL it is byte-reversed into the staging area at
 * cfg+4 (word + halfword), mirrored into the random-address command
 * buffer, and pushed. The previous "ready" flags are captured before
 * the disable calls; after setting the address, each set that had been
 * ready is re-applied.
 *
 * OEM @ 0x0001CF14. */
void gap_adv_set_addr_and_restart(const uint32_t *addr)
{
    uint8_t *cfg = g_gap_adv_cfg;

    if (addr != NULL) {
        /* Stage the address at cfg+4: a word + halfword copy, then an
         * in-place 6-byte reverse — same shape as bdaddr_get_reversed. */
        uint8_t *stage = cfg + 4;
        *(uint32_t *)stage = addr[0];
        *(uint16_t *)(cfg + 8) = *(const uint16_t *)(addr + 1);

        unsigned lo = 0;
        unsigned hi = 5;
        while ((int)hi > (int)lo) {
            uint8_t t = stage[hi];
            stage[hi] = stage[lo];
            stage[lo] = t;
            lo = (lo + 1) & 0xFF;
            hi = (hi - 1) & 0xFF;
        }

        /* Mirror the staged address into the random-address command
         * buffer (RAM 0x20005AF8). */
        extern uint8_t g_gap_adv_rand_addr[];
        *(uint32_t *)g_gap_adv_rand_addr = *(uint32_t *)stage;
        *(uint16_t *)(g_gap_adv_rand_addr + 4) = *(uint16_t *)(cfg + 8);
    }

    /* Snapshot which sets were ready before tearing them down. */
    uint8_t was_ready1 = cfg[ADV_OFF_READY1];
    uint8_t was_ready2 = cfg[ADV_OFF_READY2];

    gap_adv_disable_set1();
    gap_adv_disable_set2();

    log_emit_v(0x10, GAP_ADV_CMD_SET_ADDR, cfg + 4);

    if (was_ready1 != 0) {
        gap_adv_apply_set1();
    }
    if (was_ready2 != 0) {
        gap_adv_apply_set2();
    }
}

/* Derive a randomised static BLE address from the device BD_ADDR and
 * apply it to advertising.
 *
 * The reversed BD_ADDR is fetched, byte +1 is bumped by 0x0A, and the
 * top two bits of the MSB are forced to 1 (0xC0 ⇒ the random-static
 * address type per the BLE spec). The resulting address is handed to
 * gap_adv_set_addr_and_restart.
 *
 * OEM @ 0x00025A04. The caller's r1..r3 spill into the stack frame the
 * staged address occupies; the OEM builds the 6-byte address on the
 * stack starting at sp. */
void gap_adv_set_random_static_addr(void)
{
    uint8_t addr[6];

    uint8_t *p = bdaddr_get_reversed(addr);
    addr[1] = (uint8_t)(addr[1] + 0x0A);   /* OEM: reversed[1] += 0x0A */
    *p |= 0xC0u;                            /* random-static type bits */

    gap_adv_set_addr_and_restart((const uint32_t *)addr);
}

/* Disable legacy advertising set #1.
 *
 * If the set has a valid handle (cfg[0] != 0xFF) a single disable command
 * is pushed via the ICall service message — note that, unlike set #1's
 * enable/load pushes, the disable descriptor carries no handle operand:
 * the OEM issues log_emit_v(0x10, GAP_ADV_CMD_DISABLE) with exactly two
 * arguments and the command reads the active handle from the GAP layer
 * itself. The "ready" flag at +2 is cleared unconditionally afterwards
 * (on both the disabled and the not-created paths).
 *
 * Returns 0 when a disable was issued, -1 when the set had no handle.
 *
 * OEM @ 0x00024FE8. */
int gap_adv_disable_set1(void)
{
    uint8_t *cfg = g_gap_adv_cfg;
    int rc;

    if (cfg[ADV_OFF_HANDLE1] == ADV_HANDLE_NONE) {
        rc = -1;
    } else {
        log_emit_v(0x10, GAP_ADV_CMD_DISABLE);
        rc = 0;
    }

    /* OEM clears READY1 on both paths (the store sits after the branch
     * join, not inside the else). */
    cfg[ADV_OFF_READY1] = 0;
    return rc;
}

/* Disable secondary / extended advertising set #2.
 *
 * Mirror of gap_adv_disable_set1 keyed on the set #2 handle (cfg+1), with
 * one deliberate asymmetry preserved from the OEM: the "ready" flag at +3
 * is cleared ONLY on the success (handle-valid) path — the store lives
 * inside the else branch, not after the join, so a not-created set #2
 * leaves READY2 untouched. (Set #1 clears READY1 unconditionally.)
 *
 * Returns 0 when a disable was issued, -1 when the set had no handle.
 *
 * OEM @ 0x00025010. */
int gap_adv_disable_set2(void)
{
    uint8_t *cfg = g_gap_adv_cfg;

    if (cfg[ADV_OFF_HANDLE2] == ADV_HANDLE_NONE) {
        return -1;
    }

    log_emit_v(0x10, GAP_ADV_CMD_DISABLE);
    cfg[ADV_OFF_READY2] = 0;
    return 0;
}
