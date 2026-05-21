#!/usr/bin/env python3
"""Cortex-M0 boot-flow emulator for the MM32F031F6U6 (eShifter).

Loads shifterboot.bin at 0x08000000 and shifterware.bin at 0x08003000,
maps 4 KB of SRAM, stubs out the peripheral regions with hook-based
handlers, and traces execution from the chip's reset vector.

What it answers (and what it can't):
  - WHO writes SRAM 0x20000148 (G_HCLK_HZ)? — tracked via a write hook
  - Does shifterboot ever branch into shifterware's 0x08003000+
    region? — tracked via a code-region transition hook
  - Does SYSCFG.MEM_MODE get set to 3 by anyone before shifterware? —
    tracked via a write to 0x40010000
  - What's the call graph during boot? — every BL / function entry is
    logged

  - It cannot simulate live UART / Modbus protocol traffic. When
    shifterboot's main loop blocks on a UART RX poll waiting for an
    inbound packet, the emulator's UART model returns "no data" and
    the loop spins. We detect that case and stop.

Usage:
    python3 tools/emulate_mm32f031.py SHIFTERBOOT.bin [SHIFTERWARE.bin] \
        [--max-steps N] [--trace] [--from shifterboot|shifterware|shifterware-main]

Positional arguments select which .bin files to load. The shifterboot bin
is required; the shifterware bin is optional (only needed for `--from
shifterware*`). If you omit either, the script tries the historical
defaults:

  shifterboot.bin     ./shifterboot/shifterboot.bin
  shifterware.bin     ./shifterware/shifterware_0.237.bin

Default target: shifterboot (cold-reset entry).
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from unicorn import (
    UC_ARCH_ARM,
    UC_HOOK_BLOCK,
    UC_HOOK_CODE,
    UC_HOOK_INTR,
    UC_HOOK_MEM_INVALID,
    UC_HOOK_MEM_READ,
    UC_HOOK_MEM_WRITE,
    UC_MODE_MCLASS,
    UC_MODE_THUMB,
    Uc,
    UcError,
)
from unicorn.arm_const import (
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_SP,
)

import capstone

# ---------- chip layout ------------------------------------------------------

FLASH_BASE   = 0x08000000
FLASH_SIZE   = 0x00008000   # 32 KB
SRAM_BASE    = 0x20000000
SRAM_SIZE    = 0x00001000   # 4 KB

# Where shifterware actually lives in flash when it's running.
#
# The MM32F031 has two image slots: slot 1 (0x08001800, OTA staging)
# and slot 2 (0x08004800, the boot slot). Shifterboot writes inbound
# OTA chunks into slot 1, validates them, copies the image to slot 2,
# then `boot_app(slot 2 + 0x28)` jumps to slot 2's vector table.
#
# So a shifterware.bin must be loaded at slot 2 to execute correctly —
# its Reset vector (e.g. `0x08005E79` for shifterware 0.237) only
# resolves to real code when the image is mapped from 0x08004800.
SHIFTERWARE_RUN_BASE  = 0x08004800
# The slot-1 staging address is exposed too (currently unused — it's
# where shifterboot would write inbound OTA chunks before validating).
SHIFTERWARE_STAGE_BASE = 0x08001800

# Peripheral regions we stub out. Reads return zero by default, except the
# specific addresses listed in PERIPH_READ_OVERRIDES below.
PERIPH_REGIONS = [
    (0x40000000, 0x00010000, "APB1"),    # TIM2/3, USART2, I2C, ...
    (0x40010000, 0x00010000, "APB2"),    # SYSCFG, USART1, TIM1, GPIO-AFIO/SPI1
    (0x40020000, 0x00010000, "AHB1"),    # FLASH (0x40022000), RCC (0x40021000), CRC
    (0x48000000, 0x00002000, "AHB2"),    # GPIOA (0x48000000), GPIOB (0x48000400)
    (0xE0000000, 0x00100000, "ARM_PPB"), # SysTick / NVIC / SCB
]

# Reads at these addresses return the listed value instead of 0. Lets
# polling-loops make progress (HSI/HSE ready flags, USART TX-empty, ...).
PERIPH_READ_OVERRIDES = {
    # RCC->CR — HSI always ready + HSE always ready (we don't actually have
    # an oscillator; just want to satisfy any "while !ready" polls).
    0x40021000: 0x00038003,  # HSEON|HSERDY|HSEBYP set, HSION|HSIRDY set
    # RCC->CFGR — SWS = SW after one read (mirror back whatever SW is set to)
    # handled below by a live hook because it depends on what main wrote
    # FLASH->SR — BSY clear, EOP set (operations always complete instantly)
    0x4002200C: 0x00000020,
    # USART1->SR — MM32F031 has bit 0 = TX-ready/complete (different
    # from F1's bit 0 = PE). The MindMotion HAL's `USART_GetFlagStatus`
    # body just `ands r3, r1` against r1 (the flag mask the caller
    # passes), and `uart1_send_byte` polls with mask = 1 → expects
    # bit 0 set when ready. Also keep F1-compat bits 6+7 (TXE|TC) so
    # callers that pass those masks see "ready" too.
    0x40013800 + 0x08: 0x000000C1,  # SR offset 0x08: bit 0 set + TXE|TC
    0x40013800 + 0x0C: 0x000000C0,  # ISR offset 0x0C — F0-style fallback
}

# Specific named addresses for logging.
NAMED_ADDRS = {
    0x20000148: "G_HCLK_HZ",
    0x40010000: "SYSCFG_CFGR1",
    0x40021000: "RCC->CR",
    0x40021004: "RCC->CFGR",
    0x40022000: "FLASH->ACR",
    0x40022004: "FLASH->KEYR",
    0x4002200C: "FLASH->SR",
    0x40022010: "FLASH->CR",
    0xE000ED0C: "AIRCR (SYSRESETREQ)",
    0xE000E010: "SysTick->CTRL",
    0xE000E014: "SysTick->LOAD",
}

DEFAULT_SHIFTERBOOT_BIN = "shifterboot/shifterboot.bin"
DEFAULT_SHIFTERWARE_BIN = "shifterware/shifterware_0.237.bin"


# ---------- state tracking ---------------------------------------------------

@dataclass
class TraceState:
    write_log: list = field(default_factory=list)   # (pc, addr, size, value, label)
    region_entries: Counter = field(default_factory=Counter)
    func_calls: list = field(default_factory=list)  # (pc, target)
    instr_count: int = 0
    last_pc: int = 0
    syscfg_mem_mode: int = 0                        # tracks SYSCFG_CFGR1 low 2 bits
    sysreset_seen: bool = False
    interesting_pc: list = field(default_factory=list)
    # TX-capture: bytes the firmware sent out via USART1->DR. Cleared
    # between Modbus injections so each response can be decoded separately.
    tx_capture: bytearray = field(default_factory=bytearray)
    tx_total: bytearray = field(default_factory=bytearray)   # not cleared


# ---------- emulator builder -------------------------------------------------

def build_emulator(
    shifterboot_path: Path,
    shifterware_path: Optional[Path] = None,
    verbose: bool = False,
) -> tuple[Uc, TraceState, capstone.Cs]:
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    state = TraceState()
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)

    # Map flash. Permissions are R-X by default in unicorn.
    uc.mem_map(FLASH_BASE, FLASH_SIZE)
    sb = shifterboot_path.read_bytes()
    uc.mem_write(FLASH_BASE, sb)
    print(f"shifterboot: {shifterboot_path} ({len(sb)} B)")

    if shifterware_path is not None:
        sw = shifterware_path.read_bytes()
        uc.mem_write(SHIFTERWARE_RUN_BASE, sw)
        print(f"shifterware: {shifterware_path} ({len(sw)} B) loaded at "
              f"0x{SHIFTERWARE_RUN_BASE:08X} (slot 2 = boot slot)")
    else:
        print("shifterware: (not loaded — only --from shifterboot works)")

    # Boot-mode alias region at 0x00000000. Per MM32F031 UM § 1.5, with
    # BOOT0=0 (default, eShifter PCB) the main flash is aliased here.
    # We pre-load it with shifterboot's bytes (= contents of 0x08000000).
    # If MEM_MODE is later changed to 3 (SRAM at 0x00000000), the write
    # hook re-maps this region from SRAM contents.
    uc.mem_map(0x00000000, FLASH_SIZE)
    uc.mem_write(0x00000000, sb)

    # SRAM.
    uc.mem_map(SRAM_BASE, SRAM_SIZE)

    # Peripheral regions. We need them mapped so reads/writes don't fault.
    # Use uc.mem_map and let the hooks observe accesses.
    for base, size, name in PERIPH_REGIONS:
        uc.mem_map(base, size)

    def label(addr: int) -> str:
        return NAMED_ADDRS.get(addr, f"0x{addr:08X}")

    # --- hooks ---------------------------------------------------------------

    # Short-circuit shifterboot's mdelay() spin at 0x080014CA. The function
    # stores its arg into g_systick_countdown @ SRAM 0x20000010 then spins
    # until SysTick_Handler decrements it to 0. Our emulator doesn't fire
    # the SysTick IRQ, so without intervention the loop runs forever. Just
    # zero the counter on entry so the spin exits on the first iteration.
    DELAY_FUNC_ENTRY = 0x080014CA
    SYSTICK_COUNTDOWN = 0x20000010

    def code_hook(uc, address, size, _user):
        state.instr_count += 1
        state.last_pc = address

        # Detect entry to the mdelay-spin and short-circuit it. The
        # function writes its arg into g_systick_countdown at offset
        # 0x080014D0; we zero the counter at 0x080014D2 (the nop right
        # after the store) so the immediately-following spin exits.
        if address == 0x080014D2:
            uc.mem_write(SYSTICK_COUNTDOWN, struct.pack("<I", 0))
        if verbose and state.instr_count <= 200:
            try:
                bs = uc.mem_read(address, size)
                ins = next(md.disasm(bytes(bs), address))
                print(f"  PC=0x{address:08X}: {ins.mnemonic:6s} {ins.op_str}")
            except StopIteration:
                pass

        # Region transition tracking
        if FLASH_BASE <= address < SHIFTERWARE_STAGE_BASE:
            region = "shifterboot"
        elif SHIFTERWARE_RUN_BASE <= address < SHIFTERWARE_RUN_BASE + 0x3800:
            region = "shifterware"
        elif SHIFTERWARE_STAGE_BASE <= address < SHIFTERWARE_RUN_BASE:
            region = "ota-staging"   # slot 1 — only live during OTA cycles
        elif SRAM_BASE <= address < SRAM_BASE + SRAM_SIZE:
            region = "SRAM"
        else:
            region = "other"
        state.region_entries[region] += 1

        # Note shifterware entry the first time we see it
        if region == "shifterware" and state.region_entries[region] == 1:
            print(f"\n*** FIRST ENTRY INTO SHIFTERWARE REGION at PC=0x{address:08X} "
                  f"after {state.instr_count} instructions ***")
            state.interesting_pc.append((state.instr_count, address, "shifterware-first"))

    def block_hook(uc, address, size, _user):
        # Detect BL targets — block-level start of new basic block
        lr = uc.reg_read(UC_ARM_REG_LR)
        # If the previous block ended in BL, the current LR points just past it
        # We log all entries into recognized function start addresses
        pass

    def mem_write_hook(uc, access, address, size, value, _user):
        pc = uc.reg_read(UC_ARM_REG_PC)
        lbl = label(address)

        # SYSCFG_CFGR1 changes
        if address == 0x40010000:
            mm_old = state.syscfg_mem_mode
            mm_new = value & 0x3
            state.syscfg_mem_mode = mm_new
            print(f"  [PC=0x{pc:08X}] SYSCFG_CFGR1 ← 0x{value:08X}  "
                  f"MEM_MODE: {mm_old} → {mm_new}"
                  f"  {'[SRAM aliased at 0x00000000]' if mm_new == 3 else ''}")
            state.interesting_pc.append((state.instr_count, pc, f"syscfg_mem_mode={mm_new}"))

        # G_HCLK_HZ write (only the NON-zero ones — BSS init writes 0 here)
        if 0x20000148 <= address < 0x2000014C and value != 0:
            print(f"  [PC=0x{pc:08X}] *** NON-ZERO G_HCLK_HZ WRITE *** "
                  f"size={size} value=0x{value:08X} ({value} dec) at 0x{address:08X}")
            state.interesting_pc.append((state.instr_count, pc, f"G_HCLK_HZ=0x{value:08X}"))

        # SYSRESETREQ
        if address == 0xE000ED0C and (value >> 16) == 0x05FA and (value & 0x4):
            print(f"  [PC=0x{pc:08X}] *** NVIC_SystemReset (AIRCR.SYSRESETREQ) ***")
            state.sysreset_seen = True
            state.interesting_pc.append((state.instr_count, pc, "SYSRESETREQ"))
            uc.emu_stop()

        # USART1 TX: write to USART1->TDR at offset 0 of the 0x40013800
        # base. MM32F031 actually has a split TDR/RDR layout (TDR @ 0,
        # RDR @ 4) — verified by the OEM `USART_SendData` body
        # (`str r2, [r0, #0]`) and `USART_ReceiveData` body
        # (`ldr r0, [r1, #4]`). Don't capture reads — the peripheral
        # read overrides in PERIPH_READ_OVERRIDES would otherwise show
        # up as "TX" if we also captured 0x40013804.
        if address == 0x40013800:
            byte = value & 0xFF
            state.tx_capture.append(byte)
            state.tx_total.append(byte)

        state.write_log.append((pc, address, size, value, lbl))

    def mem_read_hook(uc, access, address, size, value, _user):
        # Provide override values for specific peripheral reads
        if address in PERIPH_READ_OVERRIDES:
            ov = PERIPH_READ_OVERRIDES[address]
            # Re-write the override into memory so subsequent reads see it
            # (Unicorn doesn't let us return a value from the hook, so we
            # stamp the address with the override on each access.)
            uc.mem_write(address, struct.pack("<I", ov))
        # Special: RCC->CFGR — mirror SWS field to SW field so the
        # `while (SWS != PLL)` poll in set_sysclock_to_48m terminates.
        if address == 0x40021004:
            cur = struct.unpack("<I", uc.mem_read(0x40021004, 4))[0]
            sw  = cur & 0x3
            sws = (cur >> 2) & 0x3
            if sw != sws:
                new = (cur & ~0xC) | (sw << 2)
                uc.mem_write(0x40021004, struct.pack("<I", new))

    def intr_hook(uc, intno, _user):
        pc = uc.reg_read(UC_ARM_REG_PC)
        print(f"  [PC=0x{pc:08X}] interrupt {intno}")
        if intno == 7:  # SVC / hardfault on CM0
            uc.emu_stop()

    def mem_invalid_hook(uc, access, address, size, value, _user):
        pc = uc.reg_read(UC_ARM_REG_PC)
        print(f"  [PC=0x{pc:08X}] *** INVALID MEM ACCESS {access} "
              f"addr=0x{address:08X} size={size} value=0x{value:08X} ***")
        return False  # don't try to handle

    uc.hook_add(UC_HOOK_CODE, code_hook)
    uc.hook_add(UC_HOOK_BLOCK, block_hook)
    uc.hook_add(UC_HOOK_MEM_WRITE, mem_write_hook)
    uc.hook_add(UC_HOOK_MEM_READ, mem_read_hook)
    uc.hook_add(UC_HOOK_INTR, intr_hook)
    uc.hook_add(UC_HOOK_MEM_INVALID, mem_invalid_hook)

    return uc, state, md


# ---------- entry helpers ----------------------------------------------------

def read_vector(uc: Uc, base: int, slot: int) -> int:
    return struct.unpack("<I", uc.mem_read(base + slot * 4, 4))[0]


# ---------- Modbus injection -------------------------------------------------
#
# Pre-built command scenarios that map to the wire-format frames in
# `tools/modbus.py`. Two vocabularies are supported:
#
#   - shifter-tool's three runtime commands (target: shifterware)
#   - shifterboot's OTA-server commands (target: shifterboot)
#
# `--scenario` accepts a colon-suffix arg (e.g. `shifter-write-gear:3`)
# for the few commands that need one.

def _modbus_module():
    """Late-import tools/modbus.py without polluting the global namespace
    (so the emulator still imports when modbus.py isn't on sys.path)."""
    import importlib
    import sys as _sys
    here = Path(__file__).resolve().parent
    if str(here) not in _sys.path:
        _sys.path.insert(0, str(here))
    return importlib.import_module("modbus")


def build_injection_plan(args) -> list[tuple[str, bytes]]:
    """Resolve --scenario and --inject-frame into a list of
    (label, frame_bytes) tuples in injection order."""
    out: list[tuple[str, bytes]] = []
    mb = None

    if args.scenario:
        mb = _modbus_module()
        for spec in args.scenario:
            name, _, arg = spec.partition(":")
            if name == "shifter-read-gear":
                out.append((name, mb.shifter_read_gear()))
            elif name == "shifter-read-shifts":
                out.append((name, mb.shifter_read_shifts()))
            elif name == "shifter-write-gear":
                if not arg:
                    raise SystemExit("shifter-write-gear needs a gear value: shifter-write-gear:N")
                out.append((f"{name}:{arg}", mb.shifter_write_gear(int(arg))))
            elif name == "shifterboot-ping":
                out.append((name, mb.shifterboot_ping()))
            elif name == "shifterboot-apply":
                out.append((name, mb.shifterboot_apply_image()))
            elif name == "shifterboot-erase":
                out.append((name, mb.shifterboot_erase_slot1()))
            elif name == "shifterboot-ota":
                pos = int(arg or "0", 0)
                out.append((f"{name}:pos={pos}", mb.shifterboot_ota_chunk(pos, b"\xAA" * 32)))
            elif name == "shifterboot-full-cycle":
                # Erase slot 1, push 3 dummy chunks, apply.
                out.append(("shifterboot-erase", mb.shifterboot_erase_slot1()))
                for chunk_idx in range(3):
                    out.append((
                        f"shifterboot-ota:pos={chunk_idx * 32}",
                        mb.shifterboot_ota_chunk(chunk_idx * 32, bytes([chunk_idx + 1] * 32)),
                    ))
                out.append(("shifterboot-apply", mb.shifterboot_apply_image()))
            elif name == "shifter-tool":
                # Default sequence the shifter-tool runs on connect+switch:
                # read-gear, read-shifts, write-gear:3, read-gear.
                gear = int(arg or "3", 0)
                out.append(("shifter-read-gear", mb.shifter_read_gear()))
                out.append(("shifter-read-shifts", mb.shifter_read_shifts()))
                out.append((f"shifter-write-gear:{gear}", mb.shifter_write_gear(gear)))
                out.append(("shifter-read-gear", mb.shifter_read_gear()))
            else:
                raise SystemExit(f"unknown --scenario: {name}")

    if args.inject_frame:
        for spec in args.inject_frame:
            label, _, hex_bytes = spec.partition("=") if "=" in spec else ("raw", "", spec)
            data = bytes.fromhex(hex_bytes.replace(" ", "")) if hex_bytes else bytes.fromhex(spec.replace(" ", ""))
            out.append((label or "raw", data))

    return out


def run_modbus_injection(uc, state, n: int, label: str, frame: bytes, args) -> None:
    """Write `frame` into the RX buffer, bump the RX index/head, resume the
    CPU for `args.steps_after_inject` steps, then dump captured TX.

    Two injection profiles, selected by `--target`:

      target=shifterboot — write frame bytes to MODBUS_RX_BUF (default
        0x200000C4) and set MODBUS_RX_IDX (0x20000014) = len(frame). The
        polling loop in shifterboot's `main` reads from there directly.

      target=shifterware — write frame bytes to G_RX_SCRATCH (default
        0x200001B2), set G_RX_HEAD (0x200000E4) = len(frame), and set
        G_RX_FRAME_MODE (0x200000D9) to 0 (short, 8 B) or 1 (long, 45 B).
        Also zero G_RX_WAIT_CTR (0x200000DC) so the FSM enters the
        validate-and-dispatch branch on its next poll instead of ticking
        the EOF timeout. `modbus_rx_poll` then validates the CRC and
        hands off to `modbus_dispatch_pdu`.
    """
    import sys as _sys
    here = Path(__file__).resolve().parent
    if str(here) not in _sys.path:
        _sys.path.insert(0, str(here))
    import modbus as mb

    target = args.target

    if target == "shifterboot":
        rx_addr = args.rx_buf_addr
        print(f"\n[phase {n + 1}] inject `{label}` ({len(frame)} B) → "
              f"shifterboot RX buf 0x{rx_addr:08X}")
        print(f"  >> {mb.hex_frame(frame)}")
        state.tx_capture.clear()
        uc.mem_write(rx_addr, frame)
        uc.mem_write(args.rx_idx_addr, struct.pack("<H", len(frame)))
    elif target == "shifterware":
        # shifterware RX layout (see modbus_rx_poll @ 0x08003EDA)
        scratch  = args.sw_rx_scratch
        head     = args.sw_rx_head
        mode_adr = args.sw_rx_frame_mode
        wait_ctr = args.sw_rx_wait_ctr
        frame_mode = 1 if len(frame) >= 0x2D else 0
        print(f"\n[phase {n + 1}] inject `{label}` ({len(frame)} B) → "
              f"shifterware scratch 0x{scratch:08X}  mode={frame_mode}")
        print(f"  >> {mb.hex_frame(frame)}")
        state.tx_capture.clear()
        uc.mem_write(scratch, frame)
        uc.mem_write(head, struct.pack("<I", len(frame)))
        uc.mem_write(mode_adr, struct.pack("<B", frame_mode))
        uc.mem_write(wait_ctr, struct.pack("<I", 0))
    else:
        raise SystemExit(f"unknown --target: {target!r}")

    # Resume.
    pc_before = uc.reg_read(UC_ARM_REG_PC)
    try:
        uc.emu_start(pc_before | 1, until=0, count=args.steps_after_inject)
    except UcError as e:
        pc_now = uc.reg_read(UC_ARM_REG_PC)
        print(f"  emu stopped on UcError at PC=0x{pc_now:08X}: {e}")

    # Inspect what came back.
    tx = bytes(state.tx_capture)
    if not tx:
        print(f"  << (no TX captured in {args.steps_after_inject} steps)")
        return

    print(f"  << {mb.hex_frame(tx)}")
    # Best-effort decode by function code.
    func = tx[1] if len(tx) >= 2 else None
    if func == mb.FUNC_READ_HOLDING:
        d = mb.decode_read_holding_response(tx)
        if "error" in d:
            print(f"  decode: {d['error']}")
        else:
            print(f"  decode: slave=0x{d['slave']:02X} read-holding "
                  f"byte_count={d['byte_count']} values={d['values']} "
                  f"crc_ok={d['crc_ok']}")
    elif func == mb.FUNC_WRITE_SINGLE:
        d = mb.decode_write_single_response(tx)
        if "error" in d:
            print(f"  decode: {d['error']}")
        else:
            print(f"  decode: slave=0x{d['slave']:02X} write-single "
                  f"addr=0x{d['addr']:04X} value=0x{d['value']:04X} "
                  f"crc_ok={d['crc_ok']}")
    elif func is None:
        print(f"  decode: (only {len(tx)} byte(s) captured — likely stuck in uart1_send_byte's TX-empty spin; try a larger --steps-after-inject)")
    else:
        print(f"  decode: (function 0x{func:02X} — no decoder)")


def run(args):
    sb_path = Path(args.shifterboot)
    if not sb_path.exists():
        raise SystemExit(f"shifterboot bin not found: {sb_path}")

    sw_path: Optional[Path] = Path(args.shifterware) if args.shifterware else None
    if sw_path is not None and not sw_path.exists():
        raise SystemExit(f"shifterware bin not found: {sw_path}")

    if args.frm != "shifterboot" and sw_path is None:
        raise SystemExit(
            f"--from {args.frm} requires a shifterware bin "
            f"(pass it as the second positional argument or via --shifterware)"
        )

    uc, state, md = build_emulator(sb_path, sw_path, verbose=args.trace)

    if args.frm == "shifterboot":
        sp = read_vector(uc, FLASH_BASE, 0)
        pc = read_vector(uc, FLASH_BASE, 1)
        print(f"Cold reset (shifterboot):")
        print(f"  initial SP = 0x{sp:08X}")
        print(f"  initial PC = 0x{pc:08X} (Thumb LSB → {pc & ~1:#010x})")
        print()
    elif args.frm == "shifterware":
        # Vector table sits 40 B (image header) past the image base.
        vec_base = SHIFTERWARE_RUN_BASE + 0x28
        sp = read_vector(uc, vec_base, 0)
        pc = read_vector(uc, vec_base, 1)
        print(f"Shifterware (via vector slot 1 Reset_Handler):")
        print(f"  initial SP = 0x{sp:08X}")
        print(f"  initial PC = 0x{pc:08X} (Thumb LSB → {pc & ~1:#010x})")
        print()
    elif args.frm == "shifterware-main":
        # Jump straight to main, bypassing Reset_Handler.
        if args.main_addr is not None:
            main_addr = args.main_addr
            sp = 0x20000400
            pc = main_addr | 1
            print(f"Direct entry into main (--main-addr): PC=0x{pc:08X}")
        else:
            # Try to derive from an .elf next to the shifterware bin.
            import subprocess
            assert sw_path is not None
            elf = sw_path.with_suffix(".elf")
            if not elf.exists():
                raise SystemExit(
                    f"--from shifterware-main without --main-addr expects an "
                    f"ELF next to the bin (looked for {elf}). Pass --main-addr "
                    f"<hex> to override (OEM = 0x080042D6)."
                )
            try:
                out = subprocess.check_output(["arm-none-eabi-nm", str(elf)], text=True)
            except FileNotFoundError:
                raise SystemExit("arm-none-eabi-nm not found")
            main_addr = None
            for line in out.splitlines():
                parts = line.split()
                if len(parts) >= 3 and parts[2] == "main" and parts[1] in "Tt":
                    main_addr = int(parts[0], 16)
                    break
            if main_addr is None:
                raise SystemExit(f"could not find `main` in {elf}")
            sp = 0x20000400
            pc = main_addr | 1
            print(f"Direct entry into main (from {elf}): PC=0x{pc:08X}")
        print()
    else:
        raise SystemExit(f"unknown --from value: {args.frm!r}")

    uc.reg_write(UC_ARM_REG_SP, sp)

    # Build the injection plan from --scenario / --inject-frame.
    plan = build_injection_plan(args)

    try:
        if not plan:
            # No injections: run flat to max_steps as before.
            uc.emu_start(pc, until=0, count=args.max_steps)
        else:
            # Phase 1: cold-boot to a Modbus-idle state.
            print(f"\n[phase 1] cold boot for {args.inject_after_steps} steps "
                  f"(let main reach the Modbus polling loop)...")
            try:
                uc.emu_start(pc, until=0, count=args.inject_after_steps)
            except UcError as e:
                pc_now = uc.reg_read(UC_ARM_REG_PC)
                print(f"  emu stopped on UcError at PC=0x{pc_now:08X}: {e}")
            print(f"  cold boot complete, PC=0x{uc.reg_read(UC_ARM_REG_PC):08X}, "
                  f"steps={state.instr_count}")

            # Phase 2..: per-injection.
            for n, (label, frame) in enumerate(plan, start=1):
                run_modbus_injection(uc, state, n, label, frame, args)
    except KeyboardInterrupt:
        pc = uc.reg_read(UC_ARM_REG_PC)
        print(f"\ninterrupted at PC=0x{pc:08X}")

    # --- summary -------------------------------------------------------------
    print()
    print("=" * 60)
    print(f"Final PC = 0x{state.last_pc:08X}")
    print(f"Instructions executed: {state.instr_count}")
    print()
    print("Region entry counts:")
    for r, c in state.region_entries.most_common():
        print(f"  {r:15s}  {c}")
    print()
    print(f"SYSCFG MEM_MODE final: {state.syscfg_mem_mode}")
    print(f"SYSRESETREQ seen:      {state.sysreset_seen}")
    print()

    # G_HCLK_HZ final state
    hclk = struct.unpack("<I", uc.mem_read(0x20000148, 4))[0]
    print(f"SRAM 0x20000148 (G_HCLK_HZ) final value: 0x{hclk:08X} ({hclk} dec)")
    print()

    if state.interesting_pc:
        print("Interesting events:")
        for n, pc, what in state.interesting_pc[:50]:
            print(f"  step {n:6d}  PC=0x{pc:08X}  {what}")
    else:
        print("(no flagged events)")

    print()
    print(f"Total memory writes recorded: {len(state.write_log)}")
    # Show writes to interesting regions
    sram_writes = [w for w in state.write_log if SRAM_BASE <= w[1] < SRAM_BASE + SRAM_SIZE]
    print(f"  SRAM writes: {len(sram_writes)}")
    if sram_writes and args.dump_sram_writes:
        for pc, addr, sz, val, lbl in sram_writes[-30:]:
            print(f"    [PC=0x{pc:08X}] *0x{addr:08X} ← 0x{val:08X} ({sz}B)")

    if state.tx_total:
        print()
        print(f"Cumulative USART1 TX ({len(state.tx_total)} bytes):")
        # 16 bytes per line for readability
        for i in range(0, len(state.tx_total), 16):
            row = state.tx_total[i:i + 16]
            print("  " + " ".join(f"{b:02X}" for b in row))


def _hex_int(s: str) -> int:
    """argparse type that accepts 0x-prefixed or decimal integers."""
    return int(s, 0)


def main():
    p = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=(
            "Cortex-M0 boot-flow emulator for the MM32F031F6U6 (eShifter).\n\n"
            "Pass the two firmware .bin files as positional arguments. The "
            "shifterboot bin is loaded at flash 0x08000000; the shifterware "
            "bin (optional) is loaded at flash 0x08003000 (the image-header "
            "offset within slot 1)."
        ),
    )
    p.add_argument(
        "shifterboot",
        nargs="?",
        default=DEFAULT_SHIFTERBOOT_BIN,
        help=(
            f"Path to shifterboot.bin (loaded at flash 0x08000000). "
            f"Default: {DEFAULT_SHIFTERBOOT_BIN}"
        ),
    )
    p.add_argument(
        "shifterware",
        nargs="?",
        default=None,
        help=(
            f"Path to a shifterware .bin (loaded at flash 0x08003000). "
            f"Optional — only needed for --from shifterware / "
            f"shifterware-main. Pass an explicit path, or omit to skip the "
            f"second-image load entirely. "
            f"Historical OEM default: {DEFAULT_SHIFTERWARE_BIN}"
        ),
    )
    p.add_argument(
        "--max-steps",
        type=int,
        default=200_000,
        help="Stop after this many instructions (default 200,000)",
    )
    p.add_argument(
        "--trace",
        action="store_true",
        help="Print every instruction (first 200)",
    )
    p.add_argument(
        "--from",
        dest="frm",
        default="shifterboot",
        choices=("shifterboot", "shifterware", "shifterware-main"),
        help="Where to start emulating from (default: shifterboot)",
    )
    p.add_argument(
        "--main-addr",
        type=_hex_int,
        default=None,
        help=(
            "For --from shifterware-main, the absolute address of `main` "
            "(Thumb bit cleared). If omitted, the script looks for an "
            "ELF named like the shifterware bin (`.elf` instead of `.bin`) "
            "and reads `main` via arm-none-eabi-nm. OEM build: 0x080042D6."
        ),
    )
    p.add_argument(
        "--dump-sram-writes",
        action="store_true",
        help="Dump tail SRAM writes",
    )

    # --- Modbus injection / scenarios ---
    p.add_argument(
        "--scenario",
        action="append",
        default=[],
        metavar="NAME[:ARG]",
        help=(
            "Pre-built Modbus injection scenarios. Repeatable. Options:\n"
            "  shifter-read-gear        — shifter-tool's readRegister(32,2,1)\n"
            "  shifter-read-shifts      — shifter-tool's readRegister(32,15,2)\n"
            "  shifter-write-gear:N     — shifter-tool's writeRegister(32,2,N), N=1..4\n"
            "  shifter-tool[:gear]      — full read-gear/read-shifts/write/read sequence\n"
            "  shifterboot-ping         — sub-id 0x01 (template B reply)\n"
            "  shifterboot-apply        — sub-id 0x81 (image_verify_crc + reset latch)\n"
            "  shifterboot-erase        — sub-id 0x95 (erase slot 1)\n"
            "  shifterboot-ota[:pos]    — sub-id 0x82 (OTA 32-byte chunk)\n"
            "  shifterboot-full-cycle   — erase, 3 OTA chunks, apply (full OTA flow)"
        ),
    )
    p.add_argument(
        "--inject-frame",
        action="append",
        default=[],
        metavar="HEX",
        help=(
            "Raw hex bytes to inject into the RX buffer as one Modbus frame. "
            "Repeatable. Whitespace within HEX is allowed (e.g. "
            "'20 03 00 02 00 01 23 7B')."
        ),
    )
    p.add_argument(
        "--target",
        choices=("shifterboot", "shifterware"),
        default=None,
        help=(
            "Which dispatcher we're injecting into — picks the right SRAM "
            "addresses for the RX accumulator state. shifterboot uses "
            "RX_BUF/IDX; shifterware uses SCRATCH/HEAD/MODE/WAIT_CTR. "
            "Auto-defaults from --from (shifterboot → shifterboot, "
            "shifterware* → shifterware)."
        ),
    )
    p.add_argument(
        "--rx-buf-addr",
        type=_hex_int,
        default=0x200000C4,
        help=(
            "[target=shifterboot] RX buffer base (default 0x200000C4 = "
            "shifterboot's MODBUS_RX_BUF)."
        ),
    )
    p.add_argument(
        "--rx-idx-addr",
        type=_hex_int,
        default=0x20000014,
        help=(
            "[target=shifterboot] RX-index halfword address (default "
            "0x20000014 = shifterboot's MODBUS_RX_IDX). Each injection "
            "writes `len(frame)` here so the next dispatcher poll picks "
            "it up."
        ),
    )
    p.add_argument(
        "--sw-rx-scratch",
        type=_hex_int,
        default=0x200001B2,
        help=(
            "[target=shifterware] IRQ-filled inbound scratch (default "
            "0x200001B2 = G_RX_SCRATCH). The FSM at `modbus_rx_poll` "
            "copies validated bytes from here into G_RX_BUF (short) or "
            "G_LONG_BUF (long)."
        ),
    )
    p.add_argument(
        "--sw-rx-head",
        type=_hex_int,
        default=0x200000E4,
        help="[target=shifterware] G_RX_HEAD (default 0x200000E4) — frame bytes received.",
    )
    p.add_argument(
        "--sw-rx-frame-mode",
        type=_hex_int,
        default=0x200000D9,
        help="[target=shifterware] G_RX_FRAME_MODE (default 0x200000D9) — 0=short, 1=long.",
    )
    p.add_argument(
        "--sw-rx-wait-ctr",
        type=_hex_int,
        default=0x200000DC,
        help="[target=shifterware] G_RX_WAIT_CTR (default 0x200000DC) — zeroed on inject.",
    )
    p.add_argument(
        "--inject-after-steps",
        type=int,
        default=10_000,
        help="Cold-boot steps before the first injection (default 10,000).",
    )
    p.add_argument(
        "--steps-after-inject",
        type=int,
        default=5_000,
        help="Steps to run after each injection (default 5,000).",
    )

    args = p.parse_args()

    # Auto-default --target from --from.
    if args.target is None:
        args.target = "shifterware" if args.frm.startswith("shifterware") else "shifterboot"

    run(args)


if __name__ == "__main__":
    main()
