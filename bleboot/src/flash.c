#include <stdint.h>

#include "bim.h"

/* Flash-session begin (`FUN_00056A88` in the OEM). Called as the
 * gate at every BIM entry point that touches flash:
 * `bim_full_scan_and_launch`, `bim_verify_and_launch_image`,
 * `bim_crc32_image` (when its `use_flash` arg is non-zero). Returns
 * 1 on success (caller may proceed), 0 on failure (caller bails).
 *
 * The complementary "release" routine is `FUN_000570AC`, called by
 * every flash-using path after the operation completes (and by
 * this function on the failure branch). Together they implement a
 * begin/end bracket around flash MMIO access.
 *
 * Five-stage setup:
 *
 *   1. Configure something via `FUN_000563C8` with the literal
 *      `0x003D0900` (= 4,000,000 decimal — likely a 4 MHz clock
 *      reference) and a count/divisor of 9.
 *
 *   2. Two ROM-API calls into the flash sub-table at ROM
 *      `0x100001B4` (4 bytes earlier than `bim_panic_prep`'s
 *      `0x100001B8`, so an adjacent sub-table in the standard
 *      `ROM_API_TABLE` array). Index 15 (byte offset 60) called
 *      with args `4` then `3` — looks like a two-step state
 *      machine (e.g., wake from low-power, then arm sense
 *      amplifier). Exact API identity not yet pinned.
 *
 *   3. Light DIO3 via `GPIO_DOUTSET31_0` — the flash-session
 *      busy LED. Distinct from DIO2 (`bim_panic_indicate`) and
 *      DIO4 (`FUN_00057138` / `FUN_00057188` pair).
 *
 *   4. Call `FUN_00057138` (8 B + 4 B literal) — known to write
 *      `16 = 1<<4` to `GPIO_DOUTSET31_0`, i.e. set DIO4 as well.
 *      So both DIO3 and DIO4 light during a flash session.
 *
 *   5. Two-stage readiness probe: `FUN_00056D6A()` must return
 *      non-zero, then `FUN_0005698C()` provides the actual
 *      success flag. If the first stage fails, we release the
 *      partially-acquired session via `FUN_000570AC` and report
 *      failure. */

#define GPIO_DOUTSET31_0      (*(volatile uint32_t *)0x40022090u)
#define GPIO_DOUTCLR31_0      (*(volatile uint32_t *)0x400220A0u)

/* DIO assignments on the BLE PCB:
 *
 *   DIO2 — panic LED (driven by `bim_panic_indicate`).
 *   DIO3 — "flash session active" indicator LED, lit by
 *          `bim_flash_prepare` and held across the whole session.
 *   DIO4 — **external SPI NOR flash /CS** (active low). Bit-banged
 *          manually rather than driven by SSI0's hardware FSS pin —
 *          every SPI bracket is `dio4_clear` (assert) → send/recv →
 *          `dio4_set` (release). The /CS interpretation is confirmed
 *          by the bracket pattern in `bim_spi_flash_read`,
 *          `bim_spi_read_rems_id`, `bim_spi_deep_power_down`, and
 *          `bim_spi_release_from_dpd`. The earlier "DIO_FLASH_OP_LED"
 *          name was a guess based on the LED-style write pattern;
 *          the function names are kept (`dio4_set` / `dio4_clear`)
 *          but the semantic is /CS, not an LED. */
#define DIO_FLASH_BUSY_LED    (1u << 3)
#define DIO_SPI_FLASH_CSn     (1u << 4)

#define BIM_FLASH_ROM_TABLE   (*(const uintptr_t *const *)0x100001B4u)

/* TI CC2642R1F ROM-API sub-table pointers. `ROM_API_TABLE` lives at
 * `0x10000180` and is an array of 32 `uint32_t` slots; each slot is
 * a *pointer* to a per-peripheral function-pointer table elsewhere
 * in ROM. The two used by the internal-flash program/blank-check
 * stack:
 *
 *   ROM_API_TABLE[10] (= 0x100001A8) → FLASH_TABLE
 *   ROM_API_TABLE[22] (= 0x100001D8) → VIMS_TABLE
 *
 * The slot indexes the BIM exercises:
 *
 *   FLASH_TABLE[5] — called with `(uint32_t addr)` returning a
 *                    blank-check status (signature matches TI's
 *                    `FlashEfuseReadRow`-style 1-arg dispatcher;
 *                    the BIM uses it as a "is this 8 KB page all
 *                    0xFF?" probe via `bim_iflash_rom_blank_check`).
 *   FLASH_TABLE[6] — called with `(uint8_t *src, uint32_t addr,
 *                    uint32_t count)` returning a program status —
 *                    TI's `FlashProgram` driverlib equivalent
 *                    (called via `bim_iflash_program_via_rom`).
 *   VIMS_TABLE[1]  — `VIMSModeSet(base, mode)` (no return).
 *   VIMS_TABLE[2]  — `VIMSModeGet(base)` returns current mode.
 *
 * VIMS (Versatile Instruction Memory System) sits at `0x40034000`
 * and gates the CPU's instruction cache against the FLASH module.
 * Programming or erasing internal flash while the cache is enabled
 * risks stale cache lines, so the BIM forces VIMS into MODE_OFF
 * (mode 0) before any flash op and restores it to MODE_ENABLED
 * (mode 1, cache-enabled) afterwards — `bim_iflash_session_begin`
 * / `bim_iflash_session_end` implement that bracket.
 *
 * The post-op write to MMIO `0x42600484` (bit-banded clear of bit 1
 * at FLASH+0x24) is a "FSM done" / "main bank select" latch the
 * ROM expects callers to acknowledge after each program / blank-
 * check. */
#define BIM_FLASH_TABLE_PTR    (*(const uintptr_t *const *)0x100001A8u)
#define BIM_VIMS_TABLE_PTR     (*(const uintptr_t *const *)0x100001D8u)
#define VIMS_BASE              0x40034000u
#define BIM_FLASH_ACK_BIT      (*(volatile uint32_t *)0x42600484u)

/* ROM dispatch slot at 0x100001B8 — same slot bim_panic_prep uses
 * for GPIO clock bring-up. The pointed-to function-pointer table
 * exposes PRCM operations: bim_panic_prep calls indices [5], [7],
 * [13]; bim_periph_power_off (below) calls [6], [8], [13];
 * bim_ssi_init calls [5], [7], [13]. Slot [13] is consistently
 * the read/status accessor; [5]/[6] are PowerDomainOn/Off;
 * [7]/[8] are PeripheralRunEnable/Disable. */
#define BIM_PRCM_ROM_TABLE_PTR ((const uintptr_t *const *)0x100001B8u)

/* SSI (Synchronous Serial Interface) ROM dispatch slot. Used by
 * bim_ssi_init: slot [0] = SSIConfigSetExpClk-equivalent (6-arg
 * config: base, refclk, protocol, mode, bit_rate, data_width);
 * slot [4] = data-fetch / FIFO drain (called in a loop until it
 * returns 0). Sole user in this build. */
#define BIM_SSI_ROM_TABLE_PTR  ((const uintptr_t *const *)0x100001C4u)

/* SSI0 peripheral at 0x40000000 — the BIM uses SSI0 as the SPI
 * master to talk to an external SPI NOR flash chip that stages
 * OAD update images. The internal CC2642 flash holds only the
 * BIM itself (this 8 KB page); candidate images live on the
 * external SPI flash and are read/written through SSI0. This is
 * the TI OAD "external flash" build configuration.
 *
 * Selected register offsets (subset that this file touches):
 *   0x04 SSI_CR1   — Control Register 1 (bit 1 SSE = enable)
 *   0x14 SSI_IM    — Interrupt Mask (low 4 bits = enable flags)
 *   0x20 SSI_ICR   — Interrupt Clear */
#define SSI0_BASE              0x40000000u
#define SSI0_CR1               (*(volatile uint32_t *)(SSI0_BASE + 0x04u))
#define SSI0_IM                (*(volatile uint32_t *)(SSI0_BASE + 0x14u))
#define SSI0_ICR               (*(volatile uint32_t *)(SSI0_BASE + 0x20u))
#define SSI0_CR1_SSE           (1u << 1)

/* PRCM_O_CLKLOADCTL at PRCM_BASE (0x40082000) + 0x28. Bit 0 = LOAD
 * (trigger), bit 1 = LOAD_DONE (ack). The write-through alias at
 * 0x60082028 is the canonical "kick" path; reads come back through
 * the normal alias at 0x40082028. */
#define PRCM_CLKLOADCTL_W      (*(volatile uint32_t *)0x60082028u)
#define PRCM_CLKLOADCTL_R      (*(volatile uint32_t *)0x40082028u)
#define PRCM_CLKLOADCTL_LOAD   (1u << 0)
#define PRCM_CLKLOADCTL_DONE   (1u << 1)

/* SRAM globals consumed by `bim_spi_probe_chip`:
 *
 *   `g_chip_id_bytes` (`0x20000404..0x20000406`) — the BIM
 *   places the JEDEC ID response bytes here after the SPI
 *   read-id command. The probe uses bytes [0] and [1] (at
 *   `0x20000404` and `0x20000405`) as the search key into the
 *   known-chip table. These are presumably the JEDEC
 *   "manufacturer_id" + "device_id_high" pair.
 *
 *   `g_chip_table_cursor` (`0x20000408`) — the table-walk
 *   cursor used by `bim_spi_probe_chip`. Reset to the table
 *   head (`0x000571A8`) at the start of every probe call. */
#define BIM_CHIP_TABLE_HEAD    ((const uint8_t *)0x000571A8u)
#define g_chip_table_cursor    (*(const uint8_t **)0x20000408u)
#define g_chip_id_byte1        (*(volatile uint8_t *)0x20000404u)
#define g_chip_id_byte2        (*(volatile uint8_t *)0x20000405u)

/* Two SPI-command literal blobs in flash, materialised once and
 * referenced by name by their respective callers:
 *
 *   `0x000571F0`: 4-byte REMS command word `90 FF FF 00` (opcode
 *                 0x90 + 24-bit dummy address `0xFFFF00`). Loaded
 *                 onto the stack by `bim_spi_read_rems_id` and
 *                 sent verbatim. Address LSB = 0 → REMS returns
 *                 mfr first, device second.
 *
 *   `0x000571F4`: 4-byte status-command blob `05 06 FF FF`.
 *                 Byte 0 = RDSR opcode `0x05`, loaded by
 *                 `bim_spi_wait_wip`. Byte 1 = WREN opcode
 *                 `0x06`, loaded by `bim_spi_write_enable` (via
 *                 the literal pointer at `0x000571F5` = base + 1).
 *                 Bytes 2/3 = `0xFF` filler. The two opcode
 *                 helpers share the same 4-byte `.rodata` slot
 *                 to keep flash use minimal — earlier guesses
 *                 that byte 1 was dead were wrong. */
#define BIM_REMS_CMD_WORD_PTR  ((const uint32_t *)0x000571F0u)
#define BIM_RDSR_OPCODE_PTR    ((const uint8_t  *)0x000571F4u)
#define BIM_WREN_OPCODE_PTR    ((const uint8_t  *)0x000571F5u)

/* DIO4 set / clear (`FUN_00057138` and `FUN_00057188` in the OEM).
 * Bracket every individual flash MMIO operation across the BIM —
 * `dio4_set` has ~13 call sites, `dio4_clear` ~8. DIO4 is the
 * fine-grained "flash op in flight" indicator, distinct from
 * DIO3 (the coarser "flash session active" LED that
 * `bim_flash_prepare` lights). The asymmetric caller count
 * suggests some helpers nest the bracket and let an outer caller
 * issue the matching clear.
 *
 * Both are leaf functions (no callees). The OEM's `dio4_set`
 * uses a literal-pool-sharing trick: load the DOUTCLR address
 * (`0x400220A0`, which is the canonical pool constant in this
 * image, shared with every DOUTCLR call site) and subtract 16 to
 * derive DOUTSET (`0x40022090`). GCC doesn't replicate that
 * trick (it loads `0x40022090` directly with its own literal),
 * so `dio4_set` is 4 instructions + literal (12 B total) in our
 * build vs 5 instructions (10 B) in the OEM, with the OEM's
 * 4-byte literal shared into surrounding pool space at flash
 * `0x57144`. `dio4_clear` matches more closely — same 4
 * instructions + 4-byte literal shape. */
__attribute__((noinline))
void dio4_set(void)
{
    GPIO_DOUTSET31_0 = (1u << 4);
}

__attribute__((noinline))
void dio4_clear(void)
{
    GPIO_DOUTCLR31_0 = (1u << 4);
}

/* SPI full-duplex transmit (`FUN_00056EA4` in the OEM, 44 B).
 * Sends `n` bytes from `src` over SSI0 and discards each
 * byte the slave shifts back in. Uses the SSI ROM table at
 * `0x100001C4`:
 *
 *   - slot [1] = `SSIDataPut(base, data)` (blocking) — pushes
 *     a byte into the SSI TX FIFO, blocking until the FIFO has
 *     space.
 *   - slot [3] = `SSIDataGet(base, *data)` (blocking) — pulls
 *     a byte from the SSI RX FIFO, blocking until one is
 *     available. The returned byte is stashed on the stack
 *     and ignored — for command-only transmissions like JEDEC
 *     opcodes, only the side effect matters.
 *
 * Returns 0 unconditionally. The OEM call sites preserve a
 * non-zero return path (see `bim_spi_release_from_dpd`'s
 * conditional below) suggesting earlier revisions returned a
 * status; the current implementation is success-only. */
int bim_spi_send_bytes(const void *src, uint32_t n)
{
    if (n == 0u) {
        return 0;
    }
    const uint8_t *p = (const uint8_t *)src;
    const uintptr_t *ssi = (const uintptr_t *)*BIM_SSI_ROM_TABLE_PTR;
    uint32_t scratch;
    do {
        ((void (*)(uint32_t, uint32_t))ssi[1])(SSI0_BASE, (uint32_t)*p);
        ((void (*)(uint32_t, uint32_t *))ssi[3])(SSI0_BASE, &scratch);
        p++;
        n--;
    } while (n != 0u);
    return 0;
}

/* SPI full-duplex receive (`FUN_00056C78` in the OEM, 58 B). For
 * each of `n` bytes: clock out a dummy `0x00` via slot [2]
 * (`SSIDataPutNonBlocking` — the *non-blocking* TX put), then
 * grab the byte the slave shifted back via slot [3] (`SSIDataGet`,
 * blocking) into a 1-byte stack scratch, then store it at
 * `dst[i++]`.
 *
 * Returns 0 on success. Returns -1 if any `SSIDataPutNonBlocking`
 * call returned 0 (TX FIFO full) — defensive, can't actually fire
 * with a 4-byte FIFO and the strictly serial put-then-get loop,
 * but the OEM emits the check.
 *
 * Sole in-source callers: `bim_spi_read_rems_id` (n=2),
 * `bim_spi_wait_wip` (n=1), `bim_spi_flash_read` (n=image-block-len). */
int bim_spi_recv_bytes(void *dst, uint32_t n)
{
    if (n == 0u) {
        return 0;
    }
    uint8_t *p = (uint8_t *)dst;
    do {
        const uintptr_t *ssi = (const uintptr_t *)*BIM_SSI_ROM_TABLE_PTR;
        uint32_t put_ok =
            ((uint32_t (*)(uint32_t, uint32_t))ssi[2])(SSI0_BASE, 0u);
        if (put_ok == 0u) {
            return -1;
        }
        ssi = (const uintptr_t *)*BIM_SSI_ROM_TABLE_PTR;
        uint32_t scratch = 0u;
        ((void (*)(uint32_t, uint32_t *))ssi[3])(SSI0_BASE, &scratch);
        *p++ = (uint8_t)scratch;
        n--;
    } while (n != 0u);
    return 0;
}

/* SSI0 RX-FIFO drain (`FUN_00056FE0` in the OEM, 28 B). Loops
 * calling `SSIDataGetNonBlocking` (slot [4]) until it returns 0
 * (FIFO empty). Standalone helper distinct from the inlined
 * drain at the tail of `bim_ssi_init`. Sole in-source caller:
 * `bim_spi_wait_wip` as a stale-RX-byte cleanup pulse. */
static void bim_ssi_rx_drain(void)
{
    uint32_t scratch;
    const uintptr_t *ssi = (const uintptr_t *)*BIM_SSI_ROM_TABLE_PTR;
    while (((int (*)(uint32_t, uint32_t *))ssi[4])(SSI0_BASE, &scratch) != 0) {
        ssi = (const uintptr_t *)*BIM_SSI_ROM_TABLE_PTR;
    }
}

/* SPI flash REMS (Read Electronic Manufacturer & Device ID)
 * read (`FUN_00056CF4` in the OEM, 52 B). Sends the 4-byte
 * REMS command from flash literal `0x000571F0` (= `90 FF FF 00`,
 * i.e. opcode `0x90` + 24-bit dummy address `0xFFFF00`), then
 * receives 2 bytes into the SRAM globals `g_chip_id_byte1` /
 * `g_chip_id_byte2` (= `0x20000404` / `0x20000405`).
 *
 * Returns 1 on success, 0 if either the send or receive
 * failed.
 *
 * REMS opcode `0x90` is the older 2-byte ID command (mfr +
 * device); the BIM uses it rather than the 3-byte JEDEC ID
 * (`0x9F`, mfr + memory-type + capacity) because the
 * chip-database table at flash `0x000571A8` is keyed on the
 * REMS pair. The alternate "JEDEC ID" name in earlier doc
 * passes was loose terminology; this is REMS specifically.
 *
 * Sole caller: `bim_spi_probe_chip`. */
int bim_spi_read_rems_id(void)
{
    uint32_t cmd_word = *BIM_REMS_CMD_WORD_PTR;
    dio4_clear();
    int send_ok = (bim_spi_send_bytes(&cmd_word, 4u) == 0);
    if (!send_ok) {
        dio4_set();
        return 0;
    }
    int recv_err = bim_spi_recv_bytes((void *)&g_chip_id_byte1, 2u);
    dio4_set();
    return (recv_err == 0) ? 1 : 0;
}

/* SPI chip-database lookup (`FUN_0005698C` in the OEM, 74 B).
 * Calls `bim_spi_read_rems_id` to fill `g_chip_id_byte1` /
 * `g_chip_id_byte2`, then walks the 8-byte-stride table at
 * `BIM_CHIP_TABLE_HEAD` (= `0x000571A8`) looking for an entry
 * whose bytes at offsets [4] and [5] match the readout. The
 * table terminates with an 8-byte entry whose first word is
 * `0`. Returns 1 if a matching non-NULL entry was found, 0 if
 * the read failed or the table was exhausted.
 *
 * Caller usage:
 *   - `bim_flash_prepare` tail-calls this and returns its
 *     value directly: prepare succeeds iff the connected
 *     external SPI flash is one of the BIM's known chips.
 *   - `bim_spi_wait_idle` loops on it up to 10 times, exiting
 *     when it returns 0. Used after `bim_spi_deep_power_down`
 *     to verify DPD took effect (chip stops responding to
 *     probes).
 *
 * The walk-cursor is held in SRAM (`g_chip_table_cursor`)
 * rather than on the stack — reset at the start of every
 * call. This is a TI CCS code-size tic; the cursor doesn't
 * need to persist across calls. */
int bim_spi_probe_chip(void)
{
    if (bim_spi_read_rems_id() == 0) {
        return 0;
    }

    g_chip_table_cursor = BIM_CHIP_TABLE_HEAD;

    for (;;) {
        const uint8_t *entry = g_chip_table_cursor;
        if (*(const uint32_t *)entry == 0u) {
            break;   /* end-of-table sentinel */
        }
        if (entry[4] == g_chip_id_byte1 && entry[5] == g_chip_id_byte2) {
            break;   /* match */
        }
        g_chip_table_cursor += 8u;
    }

    return (*(const uint32_t *)g_chip_table_cursor != 0u) ? 1 : 0;
}

/* SPI flash "wait until Write-In-Progress clears"
 * (`FUN_00056AD4` in the OEM, 68 B).
 *
 * Two phases:
 *
 *   1. **Prep pulse**: assert /CS, drain any stale RX bytes
 *      from the SSI0 RX FIFO via `bim_ssi_rx_drain`, release
 *      /CS. Defensive — clears any leftover state from a
 *      previously interrupted operation before the polling
 *      loop starts.
 *
 *   2. **Polling loop**: in each iteration, assert /CS, send
 *      the RDSR opcode (`0x05`, loaded once into a 1-byte
 *      stack scratch from the literal at flash `0x000571F4`),
 *      receive 1 status byte, release /CS. If the receive
 *      reported any error, return -2 immediately. Otherwise
 *      check bit 0 (WIP, Write-In-Progress per the standard
 *      SPI NOR status-register layout): if set, loop;
 *      if clear, return 0.
 *
 * Returns:
 *
 *   - `0`  — chip is ready (WIP cleared).
 *   - `-2` — receive error from `bim_spi_recv_bytes`.
 *
 * The loop is **unbounded**. In normal operation the chip
 * drops WIP within microseconds for reads / tens of ms for
 * page programs / hundreds of ms for sector erases — all
 * well within any reasonable wait. There is no timeout
 * guard; if the chip never responds with WIP=0 the BIM
 * hangs here. Used by:
 *
 *   - `bim_spi_release_from_dpd` — verify the chip woke up
 *     and is responsive.
 *   - `bim_spi_flash_read` — gate every read on the chip
 *     being idle (no in-progress program/erase blocking
 *     the read). */
int bim_spi_wait_wip(void)
{
    uint8_t opcode = *BIM_RDSR_OPCODE_PTR;

    dio4_clear();
    bim_ssi_rx_drain();
    dio4_set();

    for (;;) {
        dio4_clear();
        (void)bim_spi_send_bytes(&opcode, 1u);
        uint8_t status = 0u;
        int recv_err = bim_spi_recv_bytes(&status, 1u);
        dio4_set();

        if (recv_err != 0) {
            return -2;
        }
        if ((status & 0x1u) == 0u) {
            return 0;
        }
    }
}

/* SPI flash "Release from Deep Power Down" (`FUN_00056D6A` in
 * the OEM, 56 B). Three steps:
 *
 *   1. Send the JEDEC standard `0xAB` opcode (Release from DPD)
 *      via `bim_spi_send_bytes`, bracketed with `dio4_clear` /
 *      `dio4_set`. The 1-byte buffer lives on the stack in
 *      the `r2` slot of the prologue's `push {r2, r3, r4, lr}`
 *      — same TI CCS technique `bim_spi_deep_power_down` uses
 *      for its `0xB9` opcode.
 *
 *   2. tRES1 wake-up delay — JEDEC spec is 3–30 µs depending
 *      on chip family. The OEM uses a 200-iteration spin loop
 *      with a 16-bit counter (`mov r0, r1; subs r1, r0, #1;
 *      cmp r0, #0; uxth r1, r1; bne`) — ~12 µs at 48 MHz HF
 *      core clock. The post-decrement-with-compare-on-prev
 *      shape gives 201 actual iterations.
 *
 *   3. Verify the chip woke up by polling its status register
 *      via `bim_spi_wait_wip` (RDSR `0x05` until WIP clears).
 *      Returns 0 if the chip responds; otherwise the verify
 *      step times out via the recv error path.
 *
 * Returns 1 if both the send and verify succeeded, 0
 * otherwise. The "send failed → r4 = 0" branch is dead in this
 * build because `bim_spi_send_bytes` always returns 0, but the
 * OEM preserves the check — preserved here too.
 *
 * Sole caller: `bim_flash_prepare` (called as
 * `if (FUN_00056D6A() == 0) { bim_flash_release(); return 0; }`)
 * — the gate between SSI bring-up and the
 * `bim_spi_probe_chip` tail. */
int bim_spi_release_from_dpd(void)
{
    uint8_t opcode = 0xABu;
    int     ok     = 0;

    dio4_clear();
    if (bim_spi_send_bytes(&opcode, 1u) == 0) {
        ok = 1;
    }
    dio4_set();

    if (ok == 0) {
        return 0;
    }

    /* tRES1 spin delay, ~12 µs at 48 MHz. Volatile counter so
     * GCC doesn't optimise the loop away. */
    for (volatile uint16_t i = 200u; i != 0u; i--) {
        /* intentionally empty */
    }

    if (bim_spi_wait_wip() != 0) {
        ok = 0;
    }
    return ok;
}

/* SSI0 + PRCM bring-up (`FUN_000563C8` in the OEM, 172 B). Called
 * once by `bim_flash_prepare` with args `(4_000_000, 9)` —
 * configures SSI0 at 4 MHz SPI bit rate with config word 9
 * (likely an IOC/DMA pin-routing parameter handed to the third
 * ROM call). The TI BIM in external-flash OAD configuration uses
 * SSI0 as the SPI master to talk to an external SPI NOR flash
 * chip that stages OAD images.
 *
 * Sequence:
 *
 *   1. Power on the SERIAL + PERIPH PRCM domains: call
 *      `prcm_table[5](6)`, then poll `prcm_table[13](6) == 1`
 *      (= `PRCM_DOMAIN_POWER_ON`) until ready. The PRCM table
 *      is the same `0x100001B8` sub-table that
 *      `bim_periph_power_off` uses for the inverse teardown.
 *
 *   2. Two `prcm_table[7](mask)` calls (peripheral run-enable)
 *      with masks `0x500` and `0x100`, each followed by the
 *      canonical CLKLOADCTL kick-and-wait (`*0x60082028 = 1`,
 *      spin until `*0x40082028 & 2`). Mirrors `bim_periph_power_off`'s
 *      use of slot [8] for the inverse reconfigure.
 *
 *   3. Reset SSI0 interrupt state: clear the low 4 bits of
 *      SSI0_IM (`0x40000014`) and write 3 to SSI0_ICR
 *      (`0x40000020`).
 *
 *   4. Configure SSI0 via the SSI ROM table at `0x100001C4`,
 *      slot [0] — equivalent to TI driverlib's
 *      `SSIConfigSetExpClk(SSI0_BASE, ssi_clk=48_000_000,
 *      protocol=0, mode=0, bit_rate=arg0, data_width=8)`. The
 *      48 MHz constant matches CC2642R1F's HF XOSC.
 *
 *   5. Third ROM call via the table at `0x100001B4` (the same
 *      table `bim_flash_prepare` uses for slot [15]), slot
 *      [17], with args `(SSI0_BASE, 6, 5, -1, arg1)` — likely
 *      IOC pin routing or DMA setup for the SSI0 lines.
 *
 *   6. Enable SSI0: set the SSE bit (1<<1) of SSI0_CR1
 *      (`0x40000004`).
 *
 *   7. Drain any stale RX data: loop calling
 *      `ssi_table[4](SSI0_BASE, &scratch)` until it returns 0.
 *
 * 172 B in OEM; our reconstruction is behaviour-equivalent but
 * not byte-equivalent (GCC's register allocation and literal-
 * pool placement differ; OEM uses high regs r8/r9 to cache args
 * across the prologue, GCC chooses different scratch regs). */
void bim_ssi_init(uint32_t bit_rate, uint32_t cfg)
{
    const uintptr_t *prcm = (const uintptr_t *)*BIM_PRCM_ROM_TABLE_PTR;

    ((void (*)(uint32_t))prcm[5])(6u);
    while ((((uint32_t (*)(uint32_t))prcm[13])(6u)) != 1u) { }

    ((void (*)(uint32_t))prcm[7])(0x500u);
    PRCM_CLKLOADCTL_W = PRCM_CLKLOADCTL_LOAD;
    while ((PRCM_CLKLOADCTL_R & PRCM_CLKLOADCTL_DONE) == 0u) { }

    ((void (*)(uint32_t))prcm[7])(0x100u);
    PRCM_CLKLOADCTL_W = PRCM_CLKLOADCTL_LOAD;
    while ((PRCM_CLKLOADCTL_R & PRCM_CLKLOADCTL_DONE) == 0u) { }

    SSI0_IM &= ~0xFu;
    SSI0_ICR = 3u;

    const uintptr_t *ssi = (const uintptr_t *)*BIM_SSI_ROM_TABLE_PTR;
    ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))ssi[0])(
        SSI0_BASE, 48000000u, 0u, 0u, bit_rate, 8u);

    const uintptr_t *flash_t = (const uintptr_t *)*BIM_FLASH_ROM_TABLE;
    ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))flash_t[17])(
        SSI0_BASE, 6u, 5u, 0xFFFFFFFFu, cfg);

    SSI0_CR1 |= SSI0_CR1_SSE;

    uint32_t scratch;
    while (((int (*)(uint32_t, uint32_t *))ssi[4])(SSI0_BASE, &scratch) != 0) { }
}

/* SPI flash "Deep Power Down" command (`FUN_000570C8` in the
 * OEM, 26 B). Sole caller: `bim_flash_release`. Sends the JEDEC
 * standard `0xB9` opcode to the external SPI NOR flash, telling
 * it to enter low-power sleep. Standard across Winbond W25Q,
 * Micron N25Q, Macronix MX25, etc. — exit is via `0xAB`
 * (Release from Deep Power Down), which is presumably what
 * `bim_ssi_init`'s peripheral setup triggers on the next session.
 *
 * Brackets the 1-byte SSI write with `dio4_clear` / `dio4_set`
 * — i.e., the DIO4 op-indicator is *low* during the DPD command
 * transmission and *high* otherwise. That's the inverse of the
 * usual "on while busy" pattern; in this BIM, DIO4 is held high
 * across the whole session and only blips low for individual
 * sub-ops. The `0xB9` byte lives on the stack (in the r3 slot of
 * the prologue's push) for the duration of the 1-byte write. */
void bim_spi_deep_power_down(void)
{
    uint8_t opcode = 0xB9u;
    dio4_clear();
    bim_spi_send_bytes(&opcode, 1u);
    dio4_set();
}

/* SSI busy-wait, bounded to 10 polls (`FUN_000570E2` in the
 * OEM, 24 B). Called by `bim_flash_release` immediately after
 * `bim_spi_deep_power_down` to drain any in-flight SSI activity
 * before powering off the peripheral domains. Also called from
 * other flash-write helpers (e.g., `FUN_00056EA4` internally)
 * after each SSI transaction.
 *
 * Polls `FUN_0005698C` up to 10 times; the probe returns
 * non-zero while the SSI is still busy and 0 when idle. The
 * bound protects against hangs if the SSI never reports idle —
 * after 10 attempts the function returns regardless, letting
 * the caller proceed with teardown. */
void bim_spi_wait_idle(void)
{
    uint8_t tries = 0;
    while (tries < 10u) {
        if (bim_spi_probe_chip() == 0) {
            return;
        }
        tries = (uint8_t)(tries + 1u);
    }
}

/* SPI flash sequential read (`FUN_000569E4` in the OEM, 84 B).
 * The leaf primitive consumed by every BIM caller that pulls
 * bytes off the external SPI flash — the slot iterator's
 * 8-byte sniff (`bim_slot_iterator`), the verify-and-launch
 * 56-byte header read, the full-scan path's metadata reads,
 * and (in this build, dead) `bim_crc32_image`'s flash-source
 * path.
 *
 * Steps:
 *
 *   1. `bim_spi_wait_wip` — gate on the chip being idle. If
 *      the wait returns non-zero (recv error) return 0 to
 *      signal failure.
 *
 *   2. Build the 4-byte SPI READ command on the stack:
 *      `[0x03, addr_hi, addr_mid, addr_lo]` — opcode `0x03`
 *      (standard SPI NOR Read, slow mode, no dummy cycles)
 *      plus the 24-bit address sent **big-endian** (high
 *      byte first). 24-bit addressing limits this primitive
 *      to the first 16 MB of the chip; the installed
 *      MX25L51245G is 64 MB, so anything above 16 MB needs
 *      the 4-byte-address opcode (`0x13`) instead — which
 *      the BIM doesn't appear to use, suggesting the OAD
 *      slots are confined to the first 16 MB of the chip.
 *
 *   3. Assert /CS, send the 4-byte command, on send error
 *      release /CS and return 0.
 *
 *   4. Receive `len` bytes into `dst` via
 *      `bim_spi_recv_bytes` (which clocks out dummy zeros to
 *      shift each byte in).
 *
 *   5. Release /CS; return 1 on recv success, 0 on recv error.
 *
 * Returns 1 on success, 0 on any error path. The OEM
 * preserves both intermediate failure paths (`wait_wip` /
 * send) as separate `cbz` branches, plus a third for recv;
 * we mirror that. */
int bim_spi_flash_read(uint32_t addr, uint32_t len, void *dst)
{
    if (bim_spi_wait_wip() != 0) {
        return 0;
    }

    uint8_t cmd[4];
    cmd[0] = 0x03u;                           /* READ opcode */
    cmd[1] = (uint8_t)((addr >> 16) & 0xFFu);
    cmd[2] = (uint8_t)((addr >>  8) & 0xFFu);
    cmd[3] = (uint8_t)( addr        & 0xFFu);

    dio4_clear();
    if (bim_spi_send_bytes(cmd, 4u) != 0) {
        dio4_set();
        return 0;
    }

    int recv_err = bim_spi_recv_bytes(dst, len);
    dio4_set();
    return (recv_err == 0) ? 1 : 0;
}

/* SPI flash "Write Enable" (`FUN_00056ED4` in the OEM, 24 B +
 * 4 B literal). Sends the JEDEC standard `0x06` opcode (loaded
 * from the second byte of the shared opcode literal at flash
 * `0x000571F4`, i.e. address `0x000571F5`), bracketed with
 * `dio4_clear` / `dio4_set`. Returns 0 on success, -3 on send
 * error.
 *
 * SPI NOR flash chips require a Write Enable command before
 * every program / erase / write-status — the chip clears its
 * internal "WEL" (Write Enable Latch) after each
 * program/erase, so each operation must re-arm via WREN.
 * Sole in-source caller: `bim_spi_flash_program` (issues
 * before every page-program command). */
int bim_spi_write_enable(void)
{
    uint8_t opcode = *BIM_WREN_OPCODE_PTR;

    dio4_clear();
    int recv_err = bim_spi_send_bytes(&opcode, 1u);
    dio4_set();
    if (recv_err != 0) {
        return -3;
    }
    return 0;
}

/* SPI flash page-program (`FUN_000567A0` in the OEM, 132 B).
 * Writes `len` bytes from `src` to the external SPI NOR flash
 * starting at `addr`. Handles the chip's 256-byte page boundary
 * automatically by splitting writes that cross it.
 *
 * Per-iteration sequence:
 *
 *   1. `bim_spi_wait_wip` — gate on the chip being idle. On
 *      error, abort and return 0.
 *
 *   2. `bim_spi_write_enable` — arm the WEL bit. On error,
 *      abort and return 0.
 *
 *   3. Compute bytes-this-chunk = `min(256 - (addr & 0xFF),
 *      remaining)` — the bytes available before the next
 *      256-byte page boundary, capped at the bytes still
 *      needed.
 *
 *   4. Build the 4-byte SPI PAGE PROGRAM command on the stack:
 *      `[0x02, addr_hi, addr_mid, addr_lo]` — opcode `0x02`
 *      plus the 24-bit address sent **big-endian**. Same
 *      24-bit-address limitation as `bim_spi_flash_read`:
 *      writes above 16 MB would need the 4-byte-address
 *      PP4B opcode (`0x12`), not present in this BIM.
 *
 *   5. Assert /CS, send the 4-byte command, then send the
 *      data bytes for this page. Release /CS regardless of
 *      success.
 *
 *   6. Advance `addr`, `src`, decrement `remaining`. Loop
 *      until `remaining == 0`.
 *
 * Returns 1 on success, 0 on any error (wait-WIP fail, WREN
 * fail, send fail). The function does NOT wait for the program
 * to complete before returning — the next caller's
 * `bim_spi_wait_wip` (or any subsequent flash op) handles
 * that. Also does NOT erase first; the caller must ensure the
 * target bytes are pre-erased (`0xFF`), which is the case for
 * status-marker writes that flip individual bits from `1` to
 * `0` (e.g. `0xFF` → `0xFE` for "verified", `0xFF` → `0xFC`
 * for "rejected").
 *
 * Sole caller: `bim_full_scan_and_launch`'s slot-marker writes
 * (4 sites — transient `0xFC` and final `0xFE` markers on
 * external SPI flash). */
int bim_spi_flash_program(uint32_t addr, uint32_t len, const void *src)
{
    if (len == 0u) {
        return 1;
    }

    const uint8_t *p         = (const uint8_t *)src;
    uint32_t       remaining = len;
    uint32_t       cur_addr  = addr;
    uint8_t        opcode    = 0x02u;          /* PAGE PROGRAM */

    while (remaining != 0u) {
        if (bim_spi_wait_wip() != 0) {
            return 0;
        }
        if (bim_spi_write_enable() != 0) {
            return 0;
        }

        uint32_t page_room  = 256u - (cur_addr & 0xFFu);
        uint32_t this_chunk = (page_room <= remaining) ? page_room : remaining;

        uint8_t cmd[4];
        cmd[0] = opcode;
        cmd[1] = (uint8_t)((cur_addr >> 16) & 0xFFu);
        cmd[2] = (uint8_t)((cur_addr >>  8) & 0xFFu);
        cmd[3] = (uint8_t)( cur_addr        & 0xFFu);

        cur_addr  += this_chunk;
        remaining -= this_chunk;

        dio4_clear();
        if (bim_spi_send_bytes(cmd, 4u) != 0) {
            dio4_set();
            return 0;
        }
        if (bim_spi_send_bytes(p, this_chunk) != 0) {
            dio4_set();
            return 0;
        }
        p += this_chunk;
        dio4_set();
    }
    return 1;
}

/* Internal CC2642 flash session begin (`FUN_00056E0C` in the OEM,
 * 44 B + 8 B literal pool). Brackets every internal-flash op with a
 * VIMS shutdown: read current VIMS mode via `VIMS_TABLE[2]`
 * (`VIMSModeGet`); if non-zero (cache or split mode), call
 * `VIMS_TABLE[1]` (`VIMSModeSet`) with mode 0 (MODE_OFF) and busy-
 * wait on `VIMSModeGet` returning 0 (mode-change settle). Returns
 * the *original* mode as a uint8_t — `bim_iflash_session_end` uses
 * it as the "did we change anything?" predicate. The OEM reloads
 * `*BIM_VIMS_TABLE_PTR` on every call site rather than caching it
 * in a register across the loop; preserved verbatim so each
 * function-pointer fetch is re-dispatched (matches OEM's three
 * separate ldr sequences). */
uint32_t bim_iflash_session_begin(void)
{
    uint32_t orig;
    {
        const uintptr_t *vims = (const uintptr_t *)BIM_VIMS_TABLE_PTR;
        orig = ((uint32_t (*)(uint32_t))vims[2])(VIMS_BASE) & 0xFFu;
    }
    if (orig == 0u) {
        return 0u;
    }
    {
        const uintptr_t *vims = (const uintptr_t *)BIM_VIMS_TABLE_PTR;
        ((void (*)(uint32_t, uint32_t))vims[1])(VIMS_BASE, 0u);
    }
    for (;;) {
        const uintptr_t *vims = (const uintptr_t *)BIM_VIMS_TABLE_PTR;
        if (((uint32_t (*)(uint32_t))vims[2])(VIMS_BASE) == 0u) {
            break;
        }
    }
    return orig;
}

/* Internal CC2642 flash program-via-ROM (`FUN_0005703C` in the
 * OEM, 16 B + 8 B literal pool). Calls `FLASH_TABLE[6]` — TI's
 * `FlashProgram(src, addr, count)` driverlib equivalent — and
 * passes the program status straight back to the caller. After
 * the ROM call, clears the FLASH+0x24 bit-1 latch via the bit-
 * banded alias at `0x42600484` (the "FSM done" acknowledge the
 * ROM expects callers to issue between operations). Sole caller:
 * `bim_iflash_program` / `bim_iflash_program_flat`. */
int bim_iflash_program_via_rom(const void *src, uint32_t addr, uint32_t count)
{
    const uintptr_t *flash = (const uintptr_t *)BIM_FLASH_TABLE_PTR;
    int status =
        ((int (*)(const void *, uint32_t, uint32_t))flash[6])(src, addr, count);
    BIM_FLASH_ACK_BIT = 0u;
    return status;
}

/* Internal CC2642 flash session end (`FUN_00057090` in the OEM,
 * 14 B + 8 B literal pool). Restores VIMS to MODE_ENABLED (cache
 * on, mode 1) iff the matching `bim_iflash_session_begin` returned
 * non-zero (i.e. VIMS was originally in some non-OFF mode and we
 * forced it to OFF for the duration of the flash op).
 *
 * Quirk: always restores to mode 1 regardless of what mode the
 * original VIMSModeGet returned — even if the prior mode was
 * MODE_SPLIT (2) or anything else, this routine forces MODE_ENABLED.
 * In practice the BIM only runs at boot with no app GPRAM use, so
 * mode 1 is always the right restore target. */
void bim_iflash_session_end(uint32_t prev_state)
{
    if (prev_state == 0u) {
        return;
    }
    const uintptr_t *vims = (const uintptr_t *)BIM_VIMS_TABLE_PTR;
    ((void (*)(uint32_t, uint32_t))vims[1])(VIMS_BASE, 1u);
}

/* Internal CC2642 flash program (`FUN_00056E72` in the OEM,
 * 50 B). Writes `count` bytes from `src` to internal flash at
 * address `(slot << 13) + offset`. The 8 KB stride per slot
 * matches the CC2642R1F flash erase-page size, so each "slot"
 * is exactly one erasable page in the bleware region of
 * internal flash (`0x00000000..0x00055FFF`, 344 KB = 43 × 8 KB).
 *
 * Three steps, all delegated to ROM-API helpers:
 *
 *   1. `bim_iflash_session_begin` (`FUN_00056E0C`) — bring up
 *      the internal flash controller. Returns an opaque
 *      "previous state" handle for the matching tear-down.
 *
 *   2. `bim_iflash_program_via_rom` (`FUN_0005703C`) — calls
 *      the ROM API table at `0x100001A8` slot [6] (TI's
 *      `FlashProgram(src, addr, count)` driverlib equivalent),
 *      then writes 0 to MMIO `0x42600484` (some "operation
 *      complete" status clear).
 *
 *   3. `bim_iflash_session_end` (`FUN_00057090`) — tears down
 *      the flash session iff the bring-up changed state.
 *      Takes the bring-up's return value as the "should I
 *      tear down?" predicate.
 *
 * Returns 0 on success, `0xFF` on program failure. The
 * `0xFF` return code is suggestive — that's the pre-erased
 * value of NOR flash, so "byte didn't take" reads back as
 * `0xFF`. Used as a **complement to `bim_spi_flash_program`**:
 * SPI flash holds the OAD staging slots, internal flash
 * holds the executable bleware images. The full-scan path
 * promotes by writing markers to BOTH (status byte `0xFE`
 * to the external SPI slot, then to the corresponding
 * internal flash page).
 *
 * Callers: `bim_full_scan_and_launch` (1 site, internal
 * marker write at `slot+17`), `bim_verify_and_launch_image`
 * (1 site, marker write to the BIM's own header). */
int bim_iflash_program(uint32_t slot, uint32_t offset,
                       const void *src, uint32_t count)
{
    uint32_t prev_state = bim_iflash_session_begin();
    int      prog_err   = bim_iflash_program_via_rom(
                              src, (slot << 13) + offset, count);
    bim_iflash_session_end(prev_state);
    return (prog_err == 0) ? 0 : 0xFF;
}

/* Internal-flash flat-address program (`FUN_00056F00` in the OEM,
 * 42 B). Sibling of `bim_iflash_program` — same three-step
 * begin/ROM-program/end sequence, but with a flat destination
 * address instead of `(slot, offset)`. Used by
 * `bim_iflash_copy_from_spi` to drop 256-byte chunks at
 * arbitrary internal-flash addresses inside the bleware region.
 *
 * Returns 0 on success, `0xFF` on program failure — matches
 * `bim_iflash_program`'s convention. The slot/offset wrapper
 * exists for marker-write call sites that think in slot-relative
 * terms (`bim_iflash_program(page, 17, ...)`); this flat
 * variant is for the bulk-copy call site. */
static int bim_iflash_program_flat(uint32_t addr, const void *src, uint32_t count)
{
    uint32_t prev_state = bim_iflash_session_begin();
    int      prog_err   = bim_iflash_program_via_rom(src, addr, count);
    bim_iflash_session_end(prev_state);
    return (prog_err == 0) ? 0 : 0xFF;
}

/* TI ROM-API blank-check primitive (`FUN_00057058` in the OEM,
 * 16 B + 8 B literal pool). Calls `FLASH_TABLE[5]` (the 1-arg
 * ROM helper at offset 0x14 of the `ROM_API_FLASH_TABLE`) with
 * the page address; the ROM returns non-zero if the 8 KB page
 * is entirely `0xFF` (blank), 0 if any byte differs. Same
 * post-op latch acknowledge (`BIM_FLASH_ACK_BIT = 0`) as
 * `bim_iflash_program_via_rom`. Sole in-source caller:
 * `bim_iflash_check_slot_blank`. */
uint32_t bim_iflash_rom_blank_check(uint32_t addr)
{
    const uintptr_t *flash = (const uintptr_t *)BIM_FLASH_TABLE_PTR;
    uint32_t status = ((uint32_t (*)(uint32_t))flash[5])(addr);
    BIM_FLASH_ACK_BIT = 0u;
    return status;
}

/* Internal-flash blank-page check, slot-indexed (`FUN_00056FBC`
 * in the OEM, 32 B + 4 B literal). Wraps the TI ROM-API
 * blank-check primitive in a session begin/check/end bracket.
 * Computes the page's flash address as `slot << 13` (8 KB stride)
 * and asks the ROM whether the entire page is `0xFF`. Returns
 * `0xFF` if the page is blank (caller continues), `0` if it
 * isn't (caller bails). Used only by `bim_iflash_check_range_blank`
 * (4 B per call's loop body — the slot iteration). */
static int bim_iflash_check_slot_blank(uint8_t slot_idx)
{
    uint32_t prev_state = bim_iflash_session_begin();
    uint32_t blank      = bim_iflash_rom_blank_check((uint32_t)slot_idx << 13);
    bim_iflash_session_end(prev_state);
    return ((uint8_t)blank != 0u) ? 0xFF : 0;
}

/* Internal-flash blank-range verification (`FUN_00056C34` in the
 * OEM, 66 B). Iterates every internal-flash page that the byte
 * range `[addr_low, addr_low + length)` would touch and confirms
 * each one is blank (pre-erased to `0xFF`). Returns `0` if every
 * page is blank, `0xFF` on the first non-blank page.
 *
 * Quirk: the OEM passes only the LOW 8 BITS of the destination
 * address (`uxtb r0` at the call site in
 * `bim_iflash_copy_from_spi`), so the start-page index it
 * computes is `(addr & 0xFF) / chunk_size` — which collapses to
 * `0` for any reasonable chunk size (1024, 2048, 8192). The
 * end-page index is `ceil(length / chunk_size)`, so the check
 * effectively verifies pages `0..ceil(length/chunk_size)` are
 * blank — not the pages this write will actually touch. Looks
 * like a holdover from an earlier `slot_base`-relative variant
 * where the low byte WAS the slot index; preserved verbatim
 * because changing it would diverge from the OEM behaviour
 * (which is: the check is effectively a "are slots 0..N blank?"
 * sanity pass before a bulk write, regardless of where the
 * write lands). */
static int bim_iflash_check_range_blank(uint8_t addr_low, uint32_t length, uint32_t chunk_size)
{
    uint8_t start = (uint8_t)((uint32_t)addr_low / chunk_size);
    uint8_t count = (uint8_t)(length / chunk_size);

    if ((length % chunk_size) != 0u) {
        count = (uint8_t)(count + 1u);
    }

    uint8_t end = (uint8_t)(start + count);
    uint8_t slot = start;
    uint8_t fail = 0;

    while (slot < end) {
        if (bim_iflash_check_slot_blank(slot) != 0xFF) {
            fail = 0xFF;
            break;
        }
        slot = (uint8_t)(slot + 1u);
    }

    return fail;
}

/* External-to-internal flash copy (`FUN_00056714` in the OEM,
 * 140 B). The OAD promote primitive — pulls a contiguous range
 * from external SPI flash and writes it to internal CC2642
 * flash, 256 bytes per chunk. Used after CRC verification in
 * `bim_full_scan_and_launch` to copy the verified candidate from
 * its OAD staging slot to its executable destination in internal
 * flash, and also by `bim_verify_and_launch_image` for the same
 * staging-to-executable transfer.
 *
 * Preconditions, all enforced up front (any failure returns -1
 * without touching flash):
 *
 *   - `iflash_dst` must be 4-byte aligned (the underlying
 *     ROM-API flash programmer requires word alignment).
 *   - `(iflash_dst + length - 1) / chunk_size` (end page index)
 *     must be ≥ `iflash_dst / chunk_size` (start page index) —
 *     guards against overflow.
 *   - end page index must be ≤ 44 (the BIM's executable region
 *     spans 44 × 8 KB pages = 352 KB; chunk_size is
 *     `g_oad_chunk_size` from `BIM_CHUNK_SIZE_REG`).
 *   - target slots must be pre-erased (see
 *     `bim_iflash_check_range_blank` — note that quirk's
 *     low-byte truncation effectively makes this a "slots
 *     0..N blank" check rather than a check on the actual
 *     target slots).
 *
 * Main loop: pulls 256-byte chunks from SPI flash via
 * `bim_spi_flash_read` into a 256-byte stack buffer, then writes
 * each chunk to internal flash via `bim_iflash_program_flat`.
 * Both source and destination advance lockstep by the chunk
 * size; the final iteration may write less than 256 bytes.
 *
 * Returns 0 on success, -1 on any failure (precondition trip,
 * SPI read failure, or internal-flash program failure). The
 * caller compares against 0 to decide whether to promote (write
 * 0xFE status markers) or reject (write 0xFC). */
int bim_iflash_copy_from_spi(uint32_t spi_src, uint32_t length, uint32_t iflash_dst)
{
    uint32_t chunk_size = g_oad_chunk_size;

    uint32_t start_page = iflash_dst / chunk_size;
    uint32_t end_page   = (iflash_dst + length - 1u) / chunk_size;

    if ((iflash_dst & 0x3u) != 0u) {
        return -1;
    }
    if (end_page < start_page || end_page > 44u) {
        return -1;
    }

    if (bim_iflash_check_range_blank((uint8_t)iflash_dst, length, chunk_size) != 0) {
        return -1;
    }

    uint8_t  buf[256];
    uint32_t remaining   = length;
    uint32_t cur_spi     = spi_src;
    uint32_t cur_iflash  = iflash_dst;

    while (remaining != 0u) {
        uint16_t chunk = (remaining < 256u) ? (uint16_t)remaining : 256u;

        if (bim_spi_flash_read(cur_spi, chunk, buf) == 0) {
            return -1;
        }
        if (bim_iflash_program_flat(cur_iflash, buf, chunk) != 0) {
            return -1;
        }

        cur_spi    += chunk;
        cur_iflash += chunk;
        remaining  -= chunk;
    }

    return 0;
}

/* IRQ enable/disable around a flash read. PRIMASK save/restore
 * primitives — both return prior PRIMASK as `uint32_t`.
 *
 *   `bim_irq_disable_save` (`FUN_00057164`, 8 B) — `mrs r0,
 *   PRIMASK; cpsid i; bx lr`. Disables IRQs, returns the prior
 *   bit (0 = were enabled, 1 = were already disabled).
 *
 *   `bim_irq_enable_restore` (`FUN_00057170`, 8 B) — `mrs r0,
 *   PRIMASK; cpsie i; bx lr`. Enables IRQs, returns the prior
 *   bit.
 *
 * Used by `bim_iflash_read` and `bim_iflash_read_paged` to
 * bracket the memcpy with a critical section *only when*
 * interrupts were enabled going in — nested calls (BIM already
 * inside a critical section) skip the re-enable so the outer
 * caller's IRQ state survives.
 *
 * Naked + inline-asm because GCC would otherwise emit a 4-byte
 * push/pop frame the OEM doesn't have, breaking the 8-byte
 * size. */
__attribute__((naked))
uint32_t bim_irq_disable_save(void)
{
    __asm volatile (
        "mrs r0, PRIMASK\n\t"
        "cpsid i\n\t"
        "bx lr"
    );
}

__attribute__((naked))
uint32_t bim_irq_enable_restore(void)
{
    __asm volatile (
        "mrs r0, PRIMASK\n\t"
        "cpsie i\n\t"
        "bx lr"
    );
}

/* Internal-flash read primitive (`FUN_00056E40` in the OEM,
 * 50 B). Reads `len` bytes from internal flash at `src` (which
 * lives at the CC2642's memory-mapped internal flash region
 * `0x00000000..0x00057FFF`) into RAM at `dst`, wrapped in an
 * IRQ-disabled critical section.
 *
 * Why disable IRQs around a flash read: the CC2642's internal
 * flash controller is single-ported — if a higher-priority ISR
 * issues a flash program/erase while a read is in-flight (e.g.
 * `bim_iflash_program` running from a hypothetical preemption
 * path), the read returns garbage. The BIM is essentially
 * single-threaded so this is defensive, but the OEM emits the
 * bracket anyway.
 *
 * Nested-safe: only re-enables IRQs at the end if the prior
 * PRIMASK said they were enabled going in. If the caller was
 * already in a critical section, this routine respects that
 * across the memcpy.
 *
 * Used by `bim_quick_scan_and_launch` for the 8-byte sniff and
 * 44-byte short-header reads at each slot's anchor — both reads
 * land on internal flash (not external SPI) because the quick
 * scan operates on already-promoted images that live in
 * internal flash. */
int bim_iflash_read(const void *src, void *dst, uint32_t len)
{
    uint32_t prev_primask = bim_irq_disable_save();
    int      need_restore = (prev_primask == 0u) ? 1 : 0;

    const uint8_t *s = (const uint8_t *)src;
    uint8_t       *d = (uint8_t *)dst;

    while (len != 0u) {
        *d++ = *s++;
        len--;
    }

    if (need_restore) {
        (void)bim_irq_enable_restore();
    }
    return 0;
}

/* Internal-flash read with paged addressing (`FUN_00056D30` in the
 * OEM, 58 B). Sibling of `bim_iflash_read`: same IRQ-safe memcpy
 * body, same nested-safe PRIMASK bracket, but addresses the
 * source as `(base + page * 8KB) + offset` instead of a flat
 * pointer. The 8 KB stride matches the CC2642R1F internal-flash
 * erase-page size, so `(page, offset)` reads naturally one byte at
 * a time within a slot.
 *
 * Sole caller: `bim_crc32_image`'s `use_flash != 0` path — the
 * dead-in-this-build alternate that CRC32s an image residing in
 * internal flash (the live path CRC32s from external SPI staging
 * via `bim_spi_flash_read`). Three call sites in that routine:
 * the first-block load, the mid-loop block refill, and a one-off
 * paired call right before a `bim_spi_flash_read` that overwrites
 * its result (a vestigial OEM pattern preserved verbatim).
 *
 * Quirk: the inner loop uses a 16-bit counter (`uxth` on each
 * decrement), so for `count > 0xFFFF` the routine loops 0x10000
 * times rather than `count`. In practice the BIM always passes
 * `count = 256` (BIM_BUF_BYTES) so this is harmless. The
 * `uint16_t` cast on `remaining` below preserves the OEM
 * truncation. */
int bim_iflash_read_paged(uint32_t page, uint32_t offset,
                          void *dst, uint32_t count)
{
    const uint8_t *src = (const uint8_t *)((page << 13) + offset);
    uint8_t       *d   = (uint8_t *)dst;

    uint32_t prev_primask = bim_irq_disable_save();
    int      need_restore = (prev_primask == 0u) ? 1 : 0;

    uint16_t remaining = (uint16_t)count;
    while (remaining != 0u) {
        *d++ = *src++;
        remaining--;
    }

    if (need_restore) {
        (void)bim_irq_enable_restore();
    }
    return 0;
}

/* Defensive memcpy (`FUN_000570FA` in the OEM, 22 B). Standard
 * memcpy with a null-destination guard: if `dst` is NULL, returns
 * 0 without touching memory; otherwise copies `count` bytes from
 * `src` to `dst` (iterating index-down-from-`count` over the
 * range) and returns `dst`. The body is pure C `memcpy` semantics
 * — no IRQ bracketing, no flash bracketing, no SPI dispatch.
 *
 * Sole callers: the `use_spi == 0` / `use_flash == 0` alt-source
 * paths in `bim_crc32_buffer` and `bim_crc32_image`. Both paths
 * are dead in this build (every in-source caller passes the SPI
 * path) but the OEM preserves them — presumably the alt source
 * is a RAM-resident OAD reception buffer that's only used when
 * the image being CRC'd hasn't been committed to flash yet.
 * Preserved verbatim so the alt paths continue to compile and
 * link against a real symbol. */
void *bim_memcpy_safe(void *dst, const void *src, uint32_t count)
{
    if (dst == (void *)0) {
        return (void *)0;
    }
    const uint8_t *s = (const uint8_t *)src;
    uint8_t       *d = (uint8_t *)dst;
    while (count != 0u) {
        count--;
        d[count] = s[count];
    }
    return dst;
}

int bim_flash_prepare(void)
{
    bim_ssi_init(0x003D0900u, 9u);   /* 4 MHz SPI bit rate, cfg = 9 */

    ((void (*)(uint32_t))BIM_FLASH_ROM_TABLE[15])(4u);
    ((void (*)(uint32_t))BIM_FLASH_ROM_TABLE[15])(3u);

    GPIO_DOUTSET31_0 = DIO_FLASH_BUSY_LED;

    dio4_set();

    if (bim_spi_release_from_dpd() == 0) {
        bim_flash_release();
        return 0;
    }

    return bim_spi_probe_chip();
}

/* Flash-session end (`FUN_000570AC` in the OEM). Complement to
 * `bim_flash_prepare`: called by every BIM path that touches flash
 * once the operation is done, and also recursively from
 * `bim_flash_prepare` itself when the readiness probe trips. Four
 * teardown steps:
 *
 *   1. `FUN_000570C8` (26 B) — writes a one-byte status word
 *      (`0xB9`) somewhere via a flash-write helper. Looks like a
 *      "session closing" marker write rather than peripheral
 *      teardown; pin once decoded.
 *
 *   2. `FUN_000570E2` (24 B) — a tight 10-iteration loop. Likely the
 *      flash-controller drain / status-poll loop that pairs with the
 *      probe in `FUN_00056D6A`.
 *
 *   3. Clear DIO4 directly via `GPIO_DOUTCLR31_0` — the flash-op
 *      indicator that `FUN_00057138` lit during prepare. Note: DIO3
 *      (the flash-busy LED that prepare lit inline) is NOT cleared
 *      here; presumably either left lit through image launch, or
 *      cleared by one of the sub-helpers' inner work.
 *
 *   4. `FUN_00056A38` (68 B) — clock/PRCM teardown sequencer.
 *      Issues several calls through the ROM dispatch slot at
 *      `0x100001B8` (same slot used by `bim_panic_prep`) with
 *      modified-immediate args `0x100` and `0x500`, brackets each
 *      with a busy-wait on a flash-controller status word, and
 *      finishes with `cmp #2; bne -14` retry on the final return
 *      value. This is the inverse of the PRCM bring-up that prepare
 *      delegates to `FUN_000563C8`.
 *
 * No return value: every flash path treats release as fire-and-forget. */
/* PRCM peripheral + power-domain teardown (`FUN_00056A38` in the
 * OEM). Final step of `bim_flash_release`, called once per flash
 * transaction. Three operations, all via the PRCM ROM dispatch
 * slot at `0x100001B8` (same slot `bim_panic_prep` uses for the
 * inverse "bring up GPIO" sequence):
 *
 *   1. Two `prcm_table[8](mask)` calls (masks `0x100`, then
 *      `0x500`), each followed by the canonical CLKLOADCTL "kick
 *      and wait for LOAD_DONE" idiom: write 1 to the write-through
 *      alias `0x60082028`, then spin-read `0x40082028` until bit 1
 *      sets. Slot [8] takes a peripheral bitmask and reconfigures
 *      run/sleep clock state for that mask — the two masks together
 *      cover the set of peripherals the flash session brought up.
 *
 *   2. A retry loop pairing `prcm_table[6](6)` with
 *      `prcm_table[13](6)`. Slot [6] takes a power-domain mask (6 =
 *      SERIAL | PERIPH in PRCM_DOMAIN_* encoding) and initiates a
 *      power-off. Slot [13] reads the resulting domain status; loop
 *      exits when the status equals `2`. The retry covers the case
 *      where the off-request raced with an in-flight access — the
 *      OEM re-issues both calls until the domain reports ready.
 *
 * Slot [13] is the same accessor `bim_panic_prep` uses (with arg
 * `4`); confirms [13] is consistently the PRCM read/status entry
 * across both callers.
 *
 * Asymmetry with `bim_flash_prepare`: prepare doesn't directly
 * touch PRCM — it delegates clock setup to `FUN_000563C8` (still
 * pending). So this teardown likely undoes work that
 * `FUN_000563C8` did transitively, plus reasserts a sleep-friendly
 * power-domain state between flash transactions to save energy
 * during BIM execution. */
void bim_periph_power_off(void)
{
    const uintptr_t *prcm = (const uintptr_t *)*BIM_PRCM_ROM_TABLE_PTR;

    ((void (*)(uint32_t))prcm[8])(0x100u);
    PRCM_CLKLOADCTL_W = PRCM_CLKLOADCTL_LOAD;
    while ((PRCM_CLKLOADCTL_R & PRCM_CLKLOADCTL_DONE) == 0u) { }

    prcm = (const uintptr_t *)*BIM_PRCM_ROM_TABLE_PTR;
    ((void (*)(uint32_t))prcm[8])(0x500u);
    PRCM_CLKLOADCTL_W = PRCM_CLKLOADCTL_LOAD;
    while ((PRCM_CLKLOADCTL_R & PRCM_CLKLOADCTL_DONE) == 0u) { }

    uint32_t status;
    do {
        prcm = (const uintptr_t *)*BIM_PRCM_ROM_TABLE_PTR;
        ((void (*)(uint32_t))prcm[6])(6u);

        prcm = (const uintptr_t *)*BIM_PRCM_ROM_TABLE_PTR;
        status = ((uint32_t (*)(uint32_t))prcm[13])(6u);
    } while (status != 2u);
}

void bim_flash_release(void)
{
    bim_spi_deep_power_down();
    bim_spi_wait_idle();
    GPIO_DOUTCLR31_0 = DIO_SPI_FLASH_CSn;   /* drop /CS as the chip is now in DPD */
    bim_periph_power_off();
}
