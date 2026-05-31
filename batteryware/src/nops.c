#include "batteryware.h"

/* Compiler-generated empty thunks / ROP call sites.
 * In the OEM binary these are link-time-eliminated or
 * serve as trampolines for exception dispatch.
 */
void nop_e774(void) { }
void nop_e784(void) { }
void nop_a6e0(void) { }
void nop_2ba6(void)
{
    /* Calls veneer_11f08(1) — sub-loop dispatch */
    extern void veneer_11f08(int arg);
    veneer_11f08(1);
}
void nop_2bac(void) { }
void nop_4764(void) { }
void nop_537c(void) { }

/* Empty no-op (return thunk) — FUN_0800e290 */
void epilogue(void) { }

/* Thunk veneers — empty call stubs that forward to real implementations.
 * These are likely linker-generated veneers between ARM/Thumb code.
 */
void veneer_a6aa(void) { }
void veneer_a6ba(void) { }
void veneer_a6be(void) { }
void veneer_a6e0(void) { }   /* OEM dispatch-error log hook (command_parser) */

/* Thunk/protocol stubs */
void cmd_send_response_stub(void)  { }  /* FUN_0800d846 */
void cmd_send_response_stub2(void) { }  /* FUN_0800d84a */

/* Protocol reset — clears protocol state counter (FUN_0800d8c6) */
void protocol_reset(void)
{
    *(volatile uint8_t *)0x20002D8C = 0;
}

/* DMA get status — returns a register value passed in (FUN_0801053e) */
uint32_t dma_get_status(uint32_t reg_val)
{
    return reg_val;
}

/*
 * CRC-32/MPEG-2 (poly 0x04C11DB7, init 0xFFFFFFFF, no reflection, no final
 * XOR). The OEM drives the STM32 hardware CRC peripheral; this is the
 * software equivalent used by flash_verify_header's deferred image check.
 */
uint32_t crc32_mpeg2(uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
        }
    }
    return crc;
}

/* Epilogue and thunk stubs */
void epilogue_e1ca(void) { }
void thunk_e1bc(void) { }
void thunk_e1c4(void) { }
void epilogue_thunk(void) { }   /* alias for thunk_e1c4 (cmd.c response tail) */
void nop_e1c8(void) { }
void thunk_e1b4(void) { }

/* Null subroutine (trap redirect) — bx lr */
void null_trap(void) { }

/* Empty no-op stubs with push/pop frame — FUN_0800716c, FUN_0800721c.
 * OEM body is just `push {r7,lr}; add r7,sp,#0; nop; mov sp,r7; pop {r7,pc}`. */
void nop_716c(void) { }
void nop_721c(void) { }

/*
 * ARM EABI 64-bit division by zero handler (__aeabi_ldiv0).
 *
 * Called when the divisor in a 64-bit division is zero.
 * Returns 0xFFFFFFFFFFFFFFFF (max uint64) as the quotient.
 * This is a standard ARM runtime support function.
 *
 * At 0x08000248 in the OEM binary.
 */
uint64_t __aeabi_ldiv0(uint64_t dividend, uint64_t divisor)
{
    (void)dividend;
    (void)divisor;
    return 0xFFFFFFFFFFFFFFFFULL;
}

/* FUN_0800fdac was decompiled here as `bus_fault_reset` (stub). It is
 * actually HAL_RCC_OscConfig — 1864 B of multi-oscillator bring-up
 * (HSE/HSI/MSI/LSI/LSE/HSI48/PLL) with per-phase RDY polls. Moved to
 * rcc.c as `rcc_osc_config`. */

/*
 * Fuel gauge init — wraps bms_init() for boot sequence compatibility.
 */
void fg_init(void)
{
    bms_init();
}

/*
 * IRQ wait handler — wraps state_timer_10().
 * In the OEM binary this is a separate entry point at FUN_08006fbc
 * that handles the IRQ wait/state transition after clock setup.
 */
void irq_wait_handler(void)
{
    state_timer_10();
}

/*
 * Main clock setup — minimal RCC configuration.
 * Sets up system clock source and prescalers for the STM32L072.
 * In the OEM this configures MSI/HSE/PLL and flash wait states.
 */
void main_clock_setup(void)
{
    volatile uint32_t * const RCC = (volatile uint32_t *)0x40021000;

    /* Enable MSI oscillator (default after reset, ensure it's running) */
    RCC[0] &= ~1U;  /* clear MSION to trigger a re-enable cycle if needed */

    /* Configure flash prefetch + latency for up to 32 MHz */
    *(volatile uint32_t *)0x40022004 |= 0x07;  /* FLASH_ACR: PRFTEN + LATENCY */
}

/*
 * Standard C library `memcpy`. NOT the OEM `FUN_08009412` (which has a
 * non-standard (src, u16 count, dst) ABI). This is the libc-shaped routine
 * GCC emits calls to for aggregate initialisers (e.g. local pointer arrays
 * in `state_handlers.c::bms_set_state`). The OEM byte-copy at 0x08009412
 * is a different function and will need its own translation under a
 * different name when we go after byte-eq.
 */
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

/*
 * memcpy_oem — the OEM byte-copy at FUN_08009412 (88 B).
 *
 * Non-standard ABI: `(const u8 *src, u16 count, u8 *dst)` — the source
 * pointer is in r0, the count (16-bit, stored as halfword) in r1, the
 * destination pointer in r2. Used heavily by `config_init` and `bms_init`
 * to pull EEPROM bytes into stack locals before validation.
 *
 * For decomp-c correctness we expose the same semantics. Byte-divergent
 * from OEM (which is a hand-rolled loop with specific stack layout) but
 * functionally identical.
 */
void memcpy_oem(const uint8_t *src, uint16_t count, uint8_t *dst)
{
    for (uint16_t i = 0; i < count; i++) {
        dst[i] = src[i];
    }
}

/*
 * veneer_1557c — used as `__aeabi_uidivmod(a, b)` returning the quotient.
 *
 * The OEM veneer at 0x0801557c saves r0, loads `0x08005131` into ip and
 * `bx`-jumps to it. Address 0x08005130 sits inside an unrelated state-
 * machine function whose first instruction happens to be `bl
 * __aeabi_uidivmod`, then the function tail runs onward and eventually
 * branches to its epilogue. For callers like `calculate_rsoc` and
 * `bms_init` only the divmod quotient is observable through r0; the rest
 * of the tail touches caller's [r7+7] and a bit-field byte that belong to
 * the original state machine's stack/state.
 *
 * For decomp-c correctness we expose just the divmod semantics.
 */
uint32_t veneer_1557c(uint32_t numerator, uint32_t divisor)
{
    if (divisor == 0) {
        return 0xFFFFFFFFu;
    }
    return numerator / divisor;
}

/*
 * veneer_11f48 — OEM jumps to an SRAM-resident routine at 0x20000750
 * that is installed at runtime (the .bin doesn't contain its body).
 * Used by `bms_init` after each cell-voltage scan as a post-update
 * notify hook. No observable return value at the call site. Stub.
 */
uint32_t veneer_11f48(void)
{
    return 0;
}

/*
 * veneer_1556c — sibling of veneer_1557c at OEM 0x0801556C (10 B).
 * Used by `coulomb_counter` as a notification hook on each discharge
 * tick. Like its sibling, it bx-jumps into the middle of a state-
 * machine routine that does flag bookkeeping; for decomp-c we expose
 * a no-op return-value stub.
 */
/* veneer_11ef8 — referenced from bms_setup as a 1-arg helper invoked
 * after RSOC bookkeeping ticks the stored counter. Body pending decomp;
 * empty stub keeps the call site intact. */
void veneer_11ef8(uint32_t arg)
{
    (void)arg;
}

uint32_t veneer_1556c(uint32_t arg)
{
    (void)arg;
    return 0;
}

/*
 * veneer_11ee8 / 11f08 / 11f18 / 11f58 / 11f68 / 11f88 — sibling trampolines
 * of veneer_11f48. Each is `push {r0}; ldr r0,[pc,#8]; mov ip,r0; pop {r0};
 * bx ip`, jumping to a flash-held pointer whose value is an SRAM address with
 * the Thumb bit set (e.g. 11f68 -> 0x20000d01, 11f88 -> 0x20000dd9, 11f08 ->
 * 0x2000185d, 11f18 -> 0x20001935, 11ee8 -> 0x20001089, 11f58 -> 0x200000cd).
 * Those routines are installed in SRAM at runtime and are not part of this
 * image, so for decomp-c they are no-op stubs that keep the call sites intact.
 */
void veneer_11ee8(void)          { }
void veneer_11f08(int arg)       { (void)arg; }
void veneer_11f18(void)          { }
void veneer_11f58(uint32_t arg)  { (void)arg; }
void veneer_11f68(void)          { }
void veneer_11f88(void)          { }

/*
 * The two leaves that surfaced when the main super-loop was rewired have now
 * been resolved (no longer stubs):
 *   - fg_read_done was not a function at all: 0x08004764 is the tail-merged
 *     function epilogue (`mov sp,r7; add sp,#0x44; pop {r4-r7,pc}`) that
 *     several BMS routines branch to. Every fg_read_loop call site was simply
 *     a return; the calls have been dropped in fuel_gauge.c.
 *   - gpio_check_and_config was FUN_0800AB7C — USART1 service-UART bring-up
 *     gated on PA10; decompiled in state_handlers.c as service_uart_init().
 */


/* FUN_0800eebc was decompiled here as `nop_eebc`. Re-identified as
 * HAL_CRC_MspInit (called from HAL_CRC_Init / FUN_0800edf0). Moved to
 * crc.c as `crc_msp_init`. */
