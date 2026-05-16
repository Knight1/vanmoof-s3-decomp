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
 *   `0x000571F4`: 4-byte status-command blob `05 06 FF FF`. Only
 *                 the first byte is used (RDSR opcode `0x05`); the
 *                 next byte (`0x06` = WREN) is dead in this
 *                 build. `bim_spi_wait_wip` loads byte 0 onto the
 *                 stack each call. */
#define BIM_REMS_CMD_WORD_PTR  ((const uint32_t *)0x000571F0u)
#define BIM_RDSR_OPCODE_PTR    ((const uint8_t  *)0x000571F4u)

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
int bim_spi_flash_read(uint32_t addr, void *dst, uint32_t len)
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
