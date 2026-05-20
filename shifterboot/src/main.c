/* main.c — shifterboot's central orchestrator.
 *
 * OEM @ 0x080001D8, 852 bytes (the Ghidra-side "size = 802" is wrong
 * because of an unconditional branch from 0x08000332 forward to
 * 0x0800052C that interrupts the function's address range).
 *
 * Structure: two phases sharing the same 120-byte stack frame.
 *
 *   PHASE 1 — cold-boot validate-and-jump (~0x080001D8..0x08000332)
 *     Inspects both image slots, propagates a freshly-staged image
 *     from slot 1 to slot 2 if needed, listens for a "wipe slot 2"
 *     byte (`0x1B`) in the RX buffer for 250 ms, then either boots
 *     slot 2 directly, resets the chip, or falls through to phase 2.
 *
 *   PHASE 2 — Modbus RTU server loop (0x08000334..0x0800052C)
 *     Polls `MODBUS_RX_IDX`, dispatches on slave addr + func code +
 *     register-low byte (treated as a VanMoof-custom sub-id). The
 *     shifter's Modbus slave address is `0x20`.
 *
 *   sub-id (frame[3]) | semantics
 *   ------------------+---------------------------------------------
 *   0x01              | "ping" — reply with template B (data = 0x0200)
 *   0x81              | apply image — image_verify_crc, reply, latch
 *                       a reset that re-enters phase 1
 *   0x82 (func 0x10)  | OTA stream — 32 B image chunk at frame[11..42],
 *                       big-endian stream offset at frame[7..10]
 *   0x95              | erase slot 1 (the OTA staging slot)
 *   other (func 3/6)  | reply with template A (data = 0x0001)
 *
 * A complete OTA cycle, from the master's point of view:
 *
 *     master  -> 0x95 (erase staging slot)
 *     master  -> 0x82 × N (stream 32 B chunks)
 *     master  -> 0x81 (apply)
 *     loader  -> reply with image_verify_crc result, latch reset
 *     loader  -> NVIC_SystemReset
 *     ... cold-boot re-enters phase 1 ...
 *     phase 1 -> finds slot1.crc == slot2.crc, erases slot 1
 *     phase 1 -> slot 2 valid + slot 1 invalid → boot_app(slot 2)
 *
 * We preserve the OEM's single-function structure rather than splitting
 * into helpers, so the decomp can be diff'd against the disassembly
 * cleanly. The OEM's stack-frame layout (which we don't reproduce
 * exactly — gcc allocates differently) is documented next to each
 * local. */

#include "clock.h"
#include "crc.h"
#include "flash.h"
#include "image.h"
#include "modbus.h"
#include "ota.h"
#include "systick.h"
#include "uart.h"
#include "util.h"

#include <stdint.h>

/* MM32F031 image-slot bases (see `docs/hardware.md`). */
#define SLOT1_BASE     0x08001800u
#define SLOT2_BASE     0x08004800u
#define SLOT_PAGES     12u                    /* 12 × 1 KB = 12 KB per slot */

/* Slot 2 vector table = slot 2 base + image-header size (40 B). */
#define SLOT2_VEC_BASE (SLOT2_BASE + IMAGE_HDR_SIZE)

/* Modbus dispatch constants. */
#define MODBUS_SLAVE_ADDR        0x20u
#define MB_FUNC_READ_HOLDING     0x03u
#define MB_FUNC_WRITE_SINGLE     0x06u
#define MB_FUNC_WRITE_MULTIPLE   0x10u

/* VanMoof-custom sub-ids the dispatcher recognises (= `frame[3]`,
 * which is the Modbus register-address low byte on a 0x03/0x06 PDU
 * and the same offset on a 0x10 PDU). */
#define MB_SUB_PING              0x01u
#define MB_SUB_APPLY_IMAGE       0x81u
#define MB_SUB_OTA_STREAM        0x82u
#define MB_SUB_ERASE_SLOT1       0x95u

/* "Wipe slot 2" probe byte sampled from the RX buffer 250 ms after
 * boot. If the host parked this in `MODBUS_RX_BUF[0]`, the loader
 * erases slot 2 (the active image) before deciding what to boot.
 * Likely a mainware-driven recovery path. */
#define WIPE_SLOT2_PROBE         0x1Bu

/* RCC AHBENR bit for the hardware CRC peripheral (used by
 * `image_verify_crc` via `crc32_words`). */
#define RCC_AHBPeriph_CRC        (1u << 6)

/* Modbus dispatcher frame sizes. */
#define MB_SHORT_FRAME_LEN       8u   /* 6 B PDU + 2 B CRC */
#define MB_OTA_FRAME_LEN         45u  /* 11 B header + 32 B image + 2 B CRC */
#define MB_OTA_CHUNK_BYTES       32u

/* HAL externs we depend on directly from main. */
extern void NVIC_SystemReset(void);
extern void RCC_AHBPeriphClockCmd(uint32_t RCC_AHBPeriph, uint32_t NewState);
#define ENABLE  1u

/* Modbus RX accumulator (filled by `USART1_IRQHandler`). */
#define MODBUS_RX_BUF   ((volatile uint8_t *)0x200000C4u)
#define MODBUS_RX_IDX   (*(volatile uint16_t *)0x20000014u)

/* The OEM's `main` returns `void` and is reached via a 4-byte
 * `thunk_main` trampoline in the vector-table tail (see
 * `startup_mm32f031.S`). gcc warns about `void main` per C standard
 * — suppress with `__attribute__((noreturn))`, since the function
 * either boots an app, resets the chip, or spins in the Modbus
 * server forever. */
/* `-Wmain` complains about `void main(void)` per ISO C — but the OEM
 * really does emit a void-returning `main` (the startup trampoline at
 * `0x080001D0` calls it and never expects a return). Suppress with a
 * pragma rather than renaming, so the symbol matches the OEM. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
__attribute__((noreturn))
void main(void)
{
    /* ---------------------------------------------------------------
     * Locals.  OEM offsets in `sp+N` comments are documentary —
     * gcc's allocator chooses its own layout.
     * --------------------------------------------------------------- */

    uint32_t image_status     = 0u;            /* sp+0  */
    uint32_t need_erase_slot1 = 0u;            /* sp+4  */
    uint32_t apply_pending    = 0u;            /* sp+8  — latches cmd-0x81 success */
    const uint32_t n_pages    = SLOT_PAGES;    /* sp+12 — OEM `movs #12` */
    uint32_t slot1_valid      = 0u;            /* sp+16 */
    uint32_t slot2_valid      = 0u;            /* sp+20 */
    uint32_t write_ptr        = SLOT1_BASE;    /* sp+24 — OTA dst cursor */
    uint32_t slot1_crc        = 0u;            /* sp+28 */
    uint32_t slot2_crc        = 0u;            /* sp+32 */
    uint32_t ota_chunk_size   = MB_OTA_CHUNK_BYTES;  /* sp+36 — active chunk size */
    uint32_t ota_remainder    = 0u;            /* sp+40 — image_length % 32 */
    uint32_t total_chunks     = 0u;            /* total chunks in current OTA — set on first chunk, persists across iterations (OEM stashes in r7) */
    uint32_t slot1_magic      = 0u;            /* sp+44 */
    uint32_t slot2_magic      = 0u;            /* sp+48 */

    /* sp+52..+99 — combined RX snapshot + TX scratch (the OEM aliases
     * the same 48 bytes between the 8-byte short-frame path and the
     * 45-byte OTA path). */
    uint8_t frame_buf[48];

    /* sp+100..+103 — halfword staging consumed by image_read_u32_le. */
    uint16_t staging[2];

    /* sp+104..+119 — two pre-built Modbus response templates the
     * dispatcher sends without modification. */
    const uint8_t response_a[8] = {
        MODBUS_SLAVE_ADDR, MB_FUNC_READ_HOLDING, 0x02u,
        0x00u, 0x01u, 0xC5u, 0x83u, 0x00u                /* data = 0x0001, CRC = 0x83C5 */
    };
    const uint8_t response_b[8] = {
        MODBUS_SLAVE_ADDR, MB_FUNC_READ_HOLDING, 0x02u,
        0x02u, 0x00u, 0x05u, 0x23u, 0x00u                /* data = 0x0200, CRC = 0x2305 */
    };

    /* ---------------------------------------------------------------
     * Init prelude.
     * --------------------------------------------------------------- */

    boot_init_systick();
    boot_init_usart1(9600u);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_CRC, ENABLE);

    /* ---------------------------------------------------------------
     * Phase 1 — cold-boot validate-and-jump.
     * --------------------------------------------------------------- */

    slot2_magic = image_read_u32_le((const uint16_t *)SLOT2_BASE, staging);
    slot1_magic = image_read_u32_le((const uint16_t *)SLOT1_BASE, staging);

    if (slot2_magic == IMAGE_MAGIC) {
        slot2_valid = 1u;
        slot2_crc = image_read_u32_le((const uint16_t *)(SLOT2_BASE + 8u), staging);
    }

    if (slot1_magic == IMAGE_MAGIC) {
        slot1_valid = 1u;

        /* Low byte of `version_word` at +4 is TYPE_ID. */
        uint16_t hw = image_read_halfword((const uint16_t *)(SLOT1_BASE + 4u));
        if ((uint8_t)hw != IMAGE_TYPE_SHIFTERWARE) {
            need_erase_slot1 = 1u;
        } else {
            slot1_crc = image_read_u32_le((const uint16_t *)(SLOT1_BASE + 8u), staging);
            if (slot1_crc == slot2_crc) {
                if (slot2_valid && (slot1_crc == slot2_crc)) {
                    need_erase_slot1 = 1u;       /* already mirrored — consume */
                }
            } else {
                image_status = (uint32_t)image_verify_crc();
                if (image_status == IMAGE_OK) {
                    flash_erase_pages(SLOT2_BASE, n_pages);
                    flash_copy_region(SLOT1_BASE, SLOT2_BASE);
                } else {
                    need_erase_slot1 = 1u;
                }
            }
        }
    }

    if (need_erase_slot1) {
        flash_erase_pages(SLOT1_BASE, n_pages);
        slot1_magic = image_read_u32_le((const uint16_t *)SLOT1_BASE, staging);
        if (slot1_magic != IMAGE_MAGIC) {
            slot1_valid = 0u;
        }
    }

    mdelay(250u);

    if (MODBUS_RX_BUF[0] == WIPE_SLOT2_PROBE) {
        flash_erase_pages(SLOT2_BASE, n_pages);
        slot2_magic = image_read_u32_le((const uint16_t *)SLOT2_BASE, staging);
        if (slot2_magic != IMAGE_MAGIC) {
            slot2_valid = 0u;
        }
    }

    /*
     *   slot 1 | slot 2 | action
     *   -------+--------+------------------------------------------
     *   inv    | inv    | enter Modbus loop (recovery mode)
     *   inv    | valid  | boot_app(slot 2)         — normal cold boot
     *   valid  | inv    | NVIC_SystemReset()       — re-evaluate
     *   valid  | valid  | NVIC_SystemReset()       — consume mirrored slot 1
     */
    if (slot2_valid && !slot1_valid) {
        boot_app((const uint32_t *)SLOT2_VEC_BASE);
    } else if (slot2_valid || slot1_valid) {
        NVIC_SystemReset();
    }

    /* ---------------------------------------------------------------
     * Phase 2 — Modbus RTU server loop.
     * --------------------------------------------------------------- */

    for (;;) {
        uint16_t idx = MODBUS_RX_IDX;
        if ((int16_t)idx <= 0) {
            goto end_iter;
        }

        if (MODBUS_RX_BUF[0] != MODBUS_SLAVE_ADDR) {
            MODBUS_RX_IDX = 0u;
            goto end_iter;
        }

        if (idx < MB_SHORT_FRAME_LEN) {
            goto end_iter;       /* short PDU still arriving */
        }

        uint8_t func = MODBUS_RX_BUF[1];

        if (func == MB_FUNC_READ_HOLDING || func == MB_FUNC_WRITE_SINGLE) {
            MODBUS_RX_IDX = 0u;
            memcpy(frame_buf, (const void *)MODBUS_RX_BUF, MB_SHORT_FRAME_LEN);

            uint16_t crc = modbus_crc16(frame_buf, 6u);
            if (frame_buf[6] != (uint8_t)crc)        goto end_iter;
            if (frame_buf[7] != (uint8_t)(crc >> 8)) goto end_iter;

            uint8_t sub = frame_buf[3];
            if (sub == MB_SUB_APPLY_IMAGE) {
                image_status = (uint32_t)image_verify_crc();

                uint16_t be16 = (uint16_t)((frame_buf[4] << 8) | frame_buf[5]);
                frame_buf[3] = (uint8_t)((be16 & 0x7Fu) << 1);
                frame_buf[4] = 0u;
                frame_buf[5] = (uint8_t)image_status;

                uint16_t reply_crc = modbus_crc16(frame_buf, 5u);
                frame_buf[5] = (uint8_t)reply_crc;       /* OEM writes CRC into the byte that just held image_status — but */
                frame_buf[6] = (uint8_t)(reply_crc >> 8);/* image_status is also captured separately on the stack at sp+0 */
                uart1_send_buf(frame_buf, 7u);

                if (image_status == IMAGE_OK) {
                    apply_pending = 1u;
                }
            } else if (sub == MB_SUB_PING) {
                uart1_send_buf(response_b, 7u);
            } else if (sub == MB_SUB_ERASE_SLOT1) {
                flash_erase_pages(SLOT1_BASE, n_pages);
                uart1_send_buf(frame_buf, 8u);
            } else {
                uart1_send_buf(response_a, 7u);
            }
            goto end_iter;
        }

        if (func == MB_FUNC_WRITE_MULTIPLE && MODBUS_RX_BUF[3] == MB_SUB_OTA_STREAM) {
            if (MODBUS_RX_IDX < MB_OTA_FRAME_LEN) {
                goto end_iter;   /* OTA frame still arriving */
            }

            MODBUS_RX_IDX = 0u;
            memcpy(frame_buf, (const void *)MODBUS_RX_BUF, MB_OTA_FRAME_LEN);

            uint16_t crc = modbus_crc16(frame_buf, 43u);
            if (frame_buf[43] != (uint8_t)crc)        goto end_iter;
            if (frame_buf[44] != (uint8_t)(crc >> 8)) goto end_iter;

            uint32_t stream_pos =
                ((uint32_t)frame_buf[7]  << 24) |
                ((uint32_t)frame_buf[8]  << 16) |
                ((uint32_t)frame_buf[9]  <<  8) |
                ((uint32_t)frame_buf[10]      );

            if (stream_pos == 0u) {
                write_ptr = SLOT1_BASE;

                /* The first OTA chunk delivers the image header at
                 * frame[11..50]. The `length` field is at header
                 * offset +12 → frame[23..26], stored little-endian
                 * (matches the image header on flash). */
                uint32_t image_length =
                    ((uint32_t)frame_buf[26] << 24) |
                    ((uint32_t)frame_buf[25] << 16) |
                    ((uint32_t)frame_buf[24] <<  8) |
                    ((uint32_t)frame_buf[23]      );

                total_chunks  = image_length >> 5;
                ota_remainder = image_length - (total_chunks << 5);
                ota_chunk_size = MB_OTA_CHUNK_BYTES;
            }

            /* The "is this the final chunk?" test runs every chunk —
             * gcc keeps `total_chunks` in a callee-saved register
             * across loop iterations, mirroring the OEM's use of r7. */
            if ((stream_pos >> 5) == total_chunks) {
                ota_chunk_size = ota_remainder;
            }

            ota_program_chunk(write_ptr, frame_buf, ota_chunk_size);
            write_ptr += MB_OTA_CHUNK_BYTES;

            uint16_t ack_crc = modbus_crc16(frame_buf, 6u);
            frame_buf[6] = (uint8_t)ack_crc;
            frame_buf[7] = (uint8_t)(ack_crc >> 8);
            uart1_send_buf(frame_buf, MB_SHORT_FRAME_LEN);
            goto end_iter;
        }

        /* func 0x10 with wrong sub-id, or any other func code: drop. */
        MODBUS_RX_IDX = 0u;

    end_iter:
        if (apply_pending) {
            NVIC_SystemReset();
        }
    }
}
#pragma GCC diagnostic pop
