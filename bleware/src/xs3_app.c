/* xs3_app.c — top-level application glue (audio-playback dispatch).
 *
 * OEM source file: source/xs3_app.c (path string embedded at flash
 * 0x0001BAA4). The only function carved out of this TU so far is the
 * audio-playback entry that the monitor `audio_play` command and the
 * audiotask use to start a clip.
 *
 * Two OEM symbols share one body:
 *
 *   audio_player_play       @ 0x000275B8  (8-byte veneer)
 *   play_sound_repeatedly   @ 0x0001BA6C  (136-byte body)
 *
 * The veneer at 0x275B8 is `uxtb r0,r0; movs r1,#1; b.w 0x1ba6c`: it
 * narrows the index argument to a byte and hard-wires the repeat count
 * to 1, then tail-calls the real body. Ghidra folds the two into a
 * single function; we keep them split so the public entry matches the
 * caller-visible symbol (`audio_player_play`, referenced from
 * src/monitor/cmd_audio.c) while the body keeps the OEM `__func__`
 * name ("play_sound_repeatedly", stored at flash 0x0002A568 and passed
 * to the error log).
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* Location-aware monitor logger — same prototype used across the codebase
 * (see src/gatt_write.c). `fn` is the OEM `__func__` string; `level` is a
 * severity byte. OEM @ 0x00006D90 (FUN_00006D90). */
extern void monitor_log(const char *file, int line, const char *fn,
                        int level, const char *fmt, ...);

/* Audio-clip metadata helper (suspected audio_get_clip_duration).
 * Parses the WAV header for `index` and returns the clip duration in ms,
 * or 0xFFFFFFFF if the index has no valid clip. We only use it here as a
 * validity probe. OEM @ 0x00022FE8 (FUN_00022FE8). */
extern uint32_t audio_get_clip_duration(uint32_t index);  /* src/audiotask.c */

/* Audiotask trigger. Called with 0 to (re)kick playback of the clip whose
 * index/repeat fields were just written into the audio-player state block.
 * OEM @ 0x000237C8 (FUN_000237C8). */
extern void audiotask_kick(int action);

/* Audio-player state block at RAM 0x20005670 (DAT_0001BAE0). Only two
 * fields are touched on the play path:
 *   +0x04  uint8_t  clip index to play
 *   +0x14  uint32_t remaining repeat count
 * The audiotask consumes both; writing +0x14 = repeat and +0x04 = index
 * then kicking the task starts the clip. */
#define AUDIO_PLAYER_STATE_BASE   0x20005670u
#define AUDIO_PLAYER_OFF_INDEX    0x04u
#define AUDIO_PLAYER_OFF_REPEAT   0x14u

static const char k_src_file[] = "source/xs3_app.c";

/* Start playback of audio clip `index`, looping it `repeat` times.
 *
 * Probes the clip via audio_clip_duration_ms(): a 0xFFFFFFFF result means
 * there is no valid WAV at that index, in which case we log the failure
 * and return without touching the player state. On a hit we stash the
 * repeat count and index into the audio-player state block and kick the
 * audiotask.
 *
 * OEM @ 0x0001BA6C. The OEM `__func__` string "play_sound_repeatedly"
 * (flash 0x0002A568) is passed verbatim to monitor_log on the error path,
 * so we reproduce that name here. */
static void play_sound_repeatedly(uint8_t index, uint32_t repeat)
{
    if (audio_get_clip_duration(index) == 0xFFFFFFFFu) {
        monitor_log(k_src_file, 0x12d, "play_sound_repeatedly", 1,
                    "Couldn't find audio with index %d", index);
        return;
    }

    volatile uint32_t *state = (volatile uint32_t *)AUDIO_PLAYER_STATE_BASE;
    state[AUDIO_PLAYER_OFF_REPEAT / 4] = repeat;
    ((volatile uint8_t *)AUDIO_PLAYER_STATE_BASE)[AUDIO_PLAYER_OFF_INDEX] =
        index;

    audiotask_kick(0);
}

/* Public entry: play clip `index` exactly once.
 *
 * OEM @ 0x000275B8 — an 8-byte veneer (`uxtb r0,r0; movs r1,#1;
 * b.w 0x1ba6c`) that narrows the index to a byte and forwards to
 * play_sound_repeatedly with repeat count 1. The `(uint8_t)` cast below
 * reproduces the `uxtb`. Called from cmd_audio_play
 * (src/monitor/cmd_audio.c) and the audiotask.
 *
 * The parameter is `uint32_t` to match the bleware.h declaration (and the
 * weak stub in src/audio_stubs.c it supersedes); only the low byte is
 * used, exactly as the OEM veneer guarantees. */
void audio_player_play(uint32_t index)
{
    play_sound_repeatedly((uint8_t)index, 1u);
}

/* ======================================================================
 * State-machine application functions
 *
 * The four functions below are the action workers the BLE state machine
 * dispatches to (referenced from src/state_machine_handlers.c). They are
 * faithful control-flow reconstructions; their many leaf callees are reached
 * by their decoded symbol where one exists and by OEM `FUN_xxxx` address
 * otherwise (the current decode frontier one layer down). Three carry their
 * recovered OEM `__func__` names (app_boot_event_handler, platform_tick_handler,
 * handle_ssp_request_event); the dispatcher had no `__func__` to recover.
 * They were validated against the OEM decompile (2026-06-15); two bugs from the
 * first transcription — the boot firmware-updated flag offset and a scrambled
 * dispatcher arg list — were fixed in that pass.
 * ====================================================================== */

#define MON_FILE          "source/xs3_app.c"

/* ---- shared / already-decoded callees ------------------------------ */
extern int  module_forward_async(uint32_t cmd_id, uint8_t v);       /* 0x24508, ssp.c */
extern void ssp_signal_fetch(uint16_t cmd_id);                      /* 0x25B04, ssp.c */
extern void ble_activity_led_pulse(void);                          /* 0x27004, ssp.c */
extern void system_state_save(void);                               /* 0x26FF4, system.c */
extern int  ble_connection_is_active(uint32_t conn);               /* 0x23D30, ble_connection.c */
extern int  ble_connection_get_session_key(uint32_t conn);         /* 0x23DCC, ble_connection.c */
extern void ble_connection_addr(int index, uint8_t *dst);          /* 0x201E8, ble_connection.c */
extern void monitor_event_signal(uint32_t flags);                  /* 0x232B8, console.c */
extern void gap_adv_state_set(int flag);                           /* 0x23800, xs3_sm_actions.c */
extern void gap_adv_disable_set1(void);  extern int gap_adv_apply_set1(void);  /* xs3_gap_adv.c */
extern void gap_adv_disable_set2(void);  extern int gap_adv_apply_set2(void);  /* xs3_gap_adv.c */
extern void firmware_abort(void);                                  /* 0x1F7F8 */
extern int  ble_post_disconnect(uint16_t conn, uint8_t reason);    /* 0x21030 */
extern int  buttonpress_find_conn_slot(uint32_t cmd, uint16_t *out_slot);
extern void *memset(void *d, int c, unsigned int n);               /* 0x1B6B2 */
extern void *memcpy(void *d, const void *s, unsigned int n);       /* 0x18654 */
extern const char *reset_reason_string(void);
extern void module_publish_value(uint32_t cmd_id, uint32_t value); /* FUN_00021884 */
extern void FUN_00027478(void);   /* RTOS-yield / barrier hook (used by boot + dispatcher) */
extern void FUN_00026cc0(uint32_t v);

/* ---- SSP info-request handler — OEM @ 0x00014EE4
 * (__func__ "handle_ssp_request_event") ------------------------------ */
static const char MON_FN_INFO_QUERY[] = "handle_ssp_request_event";

void handle_ssp_request_event(uint16_t cmd)
{
    extern void module_publish_bytes(uint32_t cmd_id, const void *buf, uint32_t n);  /* FUN_000244D8 */
    extern int  ble_conn_state_byte(uint32_t conn, uint8_t *out);                    /* 0x228B0 */
    extern int  pack_bleware_missing(void);                                          /* FUN_00020A78 */
    extern const uint32_t g_info_word_118;   /* DAT_00014FB4 */
    extern const uint8_t  g_device_mac[6];   /* DAT_00014FB0 */

    if (cmd == 0x118) {
        module_publish_value(0x118, g_info_word_118);
    } else if (cmd == 0x119) {
        uint8_t state = 0;
        ble_conn_state_byte(0, &state);
        module_forward_async(0x119, state);
    } else if (cmd == 0x11a) {
        uint8_t rev[6];                       /* MAC, byte-reversed for the wire */
        rev[0] = g_device_mac[5]; rev[1] = g_device_mac[4]; rev[2] = g_device_mac[3];
        rev[3] = g_device_mac[2]; rev[4] = g_device_mac[1]; rev[5] = g_device_mac[0];
        module_publish_bytes(0x11a, rev, 6);
    } else if (cmd == 0x11b && pack_bleware_missing() != 0) {
        monitor_log(MON_FILE, 0x1eb, MON_FN_INFO_QUERY, 2,
                    "Could not retrieve BLEware from pakfile");
    }
}

/* ---- periodic tick handler / connection supervisor — OEM @ 0x0000A81C
 * (__func__ "platform_tick_handler") --------------------------------- */
static const char MON_FN_SUPERVISE[] = "platform_tick_handler";

void platform_tick_handler(void)
{
    extern int   ble_connection_age_ms(uint32_t conn);      /* FUN_000208E8 */
    extern void  conn_cleanup_closed(uint32_t conn);        /* FUN_0002107C */
    extern void  FUN_00026fd4(uint32_t conn);               /* per-conn keepalive (undecoded) */
    extern uint8_t *g_xs3_app_state;                        /* DAT_0000A9F8 */
    extern const char g_supervise_query_desc[];             /* DAT_0000A9F0 (ICall descriptor) */

    monitor_event_signal(1);
    *(uint32_t *)(g_xs3_app_state + 0xc) += 1;

    for (uint32_t i = 0; i < 3; i++) {
        if (ble_connection_is_active(i) == 0) {
            continue;
        }
        if (log_emit_v(0x10, g_supervise_query_desc, i, 1) == 0) {
            monitor_log(MON_FILE, 0x2c5, MON_FN_SUPERVISE, 1,
                        "Connectionhandle was closed by cleanup action");
            conn_cleanup_closed(i);
            return;
        }
        if (ble_connection_get_session_key(i) == 0 && ble_connection_age_ms(i) > 30000) {
            if (ble_connection_age_ms(i) > 39999) {
                uint8_t mac[6];
                ble_connection_addr((int)i, mac);
                monitor_log(MON_FILE, 0x2dd, MON_FN_SUPERVISE, 2,
                            "Potential stack error -> Unable to disconnect connection "
                            "with host %02x:%02x:%02x:%02x:%02x:%02x",
                            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                firmware_abort();
            }
            monitor_log(MON_FILE, 0x2e4, MON_FN_SUPERVISE, 1,
                        "Unauthenticated connection -> trying to disconnect");
            ble_post_disconnect((uint16_t)i, 3);
        } else {
            FUN_00026fd4(i);
        }
    }
}

/* ---- boot / cold-start — OEM @ 0x0000A458 (__func__ "app_boot_event_handler") */
static const char MON_FN_BOOT[] = "app_boot_event_handler";

void app_boot_event_handler(void)
{
    extern int   FUN_0001733c(uint32_t a, uint32_t b);
    extern void  FUN_00021914(void);
    extern void  FUN_000270e4(int phase);
    extern void  FUN_0001c534(void);
    extern int   FUN_000152fc(void);
    extern void  FUN_0001c5a4(int a, int b, void *buf);
    extern void  FUN_00016a50(int a, int b);
    extern void  FUN_0002758e(void);
    extern void  FUN_0001cf80(void);
    extern void  FUN_00017b24(void);
    extern void  FUN_0002777c(void);
    extern void  FUN_00018030(void);
    extern void  FUN_0001ee3c(void);
    extern void  FUN_0001ede0(void);
    extern void  FUN_00026acc(void);
    extern void  FUN_00026f74(void);
    extern void  FUN_00026a2c(void);
    extern void  FUN_00024388(uint32_t a, int b, uint32_t c);
    extern uint8_t *g_boot_state_a;   /* DAT_0000A61C */
    extern uint8_t *g_boot_state_b;   /* DAT_0000A624 */
    extern uint32_t g_boot_arg_620;   /* DAT_0000A620 */
    extern uint32_t g_boot_arg_62c;   /* DAT_0000A62C */
    extern uint32_t g_boot_arg_630;   /* DAT_0000A630 */
    extern uint8_t *g_boot_blob_634;  /* DAT_0000A634 */
    extern const char g_boot_cmd_10a_desc[];   /* DAT_0000A638 */
    extern volatile uint8_t *g_cap_workaround_flag;  /* DAT_0000A628 */

    uint8_t scratch[0x8d];   /* FUN_0001c5a4 fills 0x8D bytes (via FUN_00024418) */

    int init = FUN_0001733c((uint32_t)(g_boot_state_a + 0x28), (uint32_t)(g_boot_state_b + 0x10));
    *(int *)(g_boot_state_a + 8) = init;
    while (init == 0) { /* halt: core init failed */ }

    FUN_00021914();
    FUN_000270e4(1);
    FUN_00027478();
    FUN_0001c534();

    int updated = 0;
    if (FUN_000152fc() == 1) {
        FUN_0001c5a4(0, 0x8d, scratch);
        updated = (scratch[0x10] == 0xFC);   /* OEM: byte at +0x10 == -4 → "firmware updated" */
        if (updated) {
            FUN_00016a50(0, 0x2000);
        }
        FUN_0002758e();
    }

    FUN_00027478();
    FUN_0001cf80();
    FUN_00026cc0(g_boot_arg_620);
    FUN_00017b24();
    FUN_0002777c();
    FUN_00027478();

    monitor_log(MON_FILE, 0xb0, MON_FN_BOOT, 0, "boot-up, resetcause <%s>", reset_reason_string());

    FUN_00027478();
    FUN_00018030();
    FUN_000270e4(8);
    FUN_0001ee3c();
    FUN_000270e4(4);
    FUN_0001ede0();
    FUN_000270e4(2);

    if (updated) {
        monitor_log(MON_FILE, 0xbe, MON_FN_BOOT, 0, "Firmware updated");
    }
    if (*g_cap_workaround_flag != 0) {
        monitor_log(MON_FILE, 0xc3, MON_FN_BOOT, 1, "Applied capacitor array workaround, reset now");
        firmware_abort();
    }

    FUN_00027478();
    FUN_00026acc();
    FUN_00026f74();
    FUN_00026a2c();
    FUN_00024388((uint32_t)(g_boot_state_a + 0x10), 0, g_boot_arg_62c);

    uint32_t info118 = g_boot_arg_630;
    if (updated) {
        info118 = ((uint32_t)g_boot_blob_634[0x21] << 16) |
                  ((uint32_t)g_boot_blob_634[0x22] << 8)  |
                   (uint32_t)g_boot_blob_634[0x23];
    }
    module_publish_value(0x118, info118);
    ssp_signal_fetch(0x5567);

    g_boot_state_a[3] = 0;
    log_emit_v(0x10, g_boot_cmd_10a_desc, 0x10a, 1, &g_boot_state_a[3]);
    FUN_00027478();
}

/* ---- GATT/SSP command dispatcher — OEM @ 0x00009274 --------------
 * NOTE: control-flow reconstruction pending line-by-line validation. */
void xs3_app_dispatch_command(uint16_t *msg)
{
    extern void  FUN_000264ec(void);
    extern void  FUN_0001d058(uint32_t v);
    extern void  FUN_00013288(void *p);
    extern int   FUN_00021644(uint16_t slot, void *out);
    extern void  FUN_00018c4c(uint16_t a, uint16_t b, uint8_t c, uint16_t d, uint8_t e,
                              void *payload, uint16_t len);
    extern void *FUN_000244a8(uint16_t cmd);
    extern uint32_t FUN_00026018(uint16_t cmd);
    extern void  FUN_0001508a(short a, uint8_t b, void *buf, uint16_t len, int f);
    extern void  FUN_000202e4(void);
    extern int   FUN_00016fdc(void);
    extern int   system_power_down(int a, int b);   /* 0x1D404 */
    extern void *monitor_alloc(unsigned int n);     /* 0x13470 (bleware.h) */
    extern void  heap_free(void *p);                /* 0x21B88 */
    extern uint8_t *g_dispatch_state;               /* DAT_0000947C */

    uint8_t *st  = g_dispatch_state;
    uint16_t cmd = msg[0];

    if (cmd == 0x5521) {
        st[0] = (uint8_t)msg[3];
    }

    if (cmd < 0x112) {
        if (cmd == 0x111) {
            if ((char)msg[3] == 1) {
                ble_post_disconnect(0xfffd, 4);
            }
            goto done;
        }
        if (cmd == 200) {                 /* 0xC8 */
            if (*((uint8_t *)msg + 7) > 1 || (char)msg[3] != (char)st[4]) {
                st[4] = (uint8_t)msg[3];
                *(uint32_t *)(st + 0x14) = (uint32_t)(*((uint8_t *)msg + 7) - 1);
            }
            FUN_000264ec();
            goto done;
        }
        if (cmd == 0x104) {
            FUN_0001d058(*(uint32_t *)(msg + 3));
            goto done;
        }
        if (cmd == 0x106) {
            FUN_00027478();
            if (msg[2] != 0) {
                FUN_00013288(msg + 3);
            }
            goto done;
        }
        if (cmd == 0x10f) {
            if ((char)msg[3] == 1) {
                ble_activity_led_pulse();
            } else {
                system_state_save();
            }
            goto done;
        }
        if (cmd == 0x110) {
            if ((char)msg[3] == 0) {
                gap_adv_disable_set1();
            } else if ((char)msg[3] == 1) {
                gap_adv_apply_set1();
            }
            goto done;
        }
        goto relay;          /* LAB_000092D8 */
    } else {
        if (cmd == 0x112) {
            system_state_save();
            FUN_000202e4();
            for (;;) { system_power_down(0, 0); }
        }
        if (cmd == 0x114) {
            gap_adv_state_set((uint8_t)msg[3] & 1);
            if ((msg[3] & 1) == 0) {
                gap_adv_disable_set2();
            } else {
                gap_adv_apply_set2();
            }
            goto done;
        }
        if (cmd == 0x11c) {
            FUN_00027478();
            if (FUN_00016fdc() != 0) {
                goto done;
            }
        } else if (cmd != 0x11d) {
            goto relay;
        }
        firmware_abort();
    }

relay:                       /* LAB_000092D8 — per-connection / module relay */
    if (cmd == 0x5541) {
        st[2] = (uint8_t)msg[3];
    }
    if (cmd == 0x5567) {
        *(uint32_t *)(st + 0xc) = *(uint32_t *)(msg + 3);
        FUN_00026cc0(*(uint32_t *)(msg + 3));
    }
    {
        uint16_t slot[2];
        if (buttonpress_find_conn_slot(cmd, slot) == 0) {
            uint8_t info[8];   /* FUN_00021644 fills: u16@0, u16@2, u16@4, u8@6, u8@7 */
            FUN_00021644(slot[0], info);
            FUN_00018c4c(*(uint16_t *)info, *(uint16_t *)(info + 2), info[7],
                         *(uint16_t *)(info + 4), info[6], msg + 3, msg[2]);
        } else if (FUN_000244a8(cmd) != NULL) {
            uint32_t n = msg[2] & 0xfffffff0u;
            if ((msg[2] & 0xf) != 0) {
                n += 0x10;
            }
            void *buf = monitor_alloc((unsigned int)(n & 0xffff));
            if (buf == NULL) {
                return;
            }
            memset(buf, 0, n);
            memcpy(buf, msg + 3, msg[2]);
            short        tag = *(short *)FUN_000244a8(cmd);
            uint32_t     sel = FUN_00026018(cmd);
            if (tag != 0 && (int)sel >= 0) {
                FUN_0001508a(tag, (uint8_t)sel, buf, (uint16_t)n, 1);
            }
            heap_free(buf);
        }
    }

done:                        /* LAB_00009474 */
    FUN_00027478();
}
