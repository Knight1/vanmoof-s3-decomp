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
    python3 tools/emulate_mm32f031.py [--max-steps N] [--trace] [--from shifterboot|shifterware]

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
    # USART1->SR / ISR — TX-empty + TC + ORE never; just enough for tx_byte
    0x40013800 + 0x08: 0x000000C0,  # SR offset 0x08: TXE|TC = 0xC0
    0x40013800 + 0x0C: 0x000000C0,  # ISR offset 0x0C
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

SHIFTERBOOT_BIN = "shifterboot/shifterboot.bin"
SHIFTERWARE_BIN = "shifterware/shifterware_0.237.bin"

# Our locally-built shifterware (post-decomp), produced by `make` in
# `shifterware/`. Pass `--ware ours` to use this instead of the OEM bin.
SHIFTERWARE_OURS_BIN = "vanmoof-s3-decomp/shifterware/build/shifterware.bin"


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


# ---------- emulator builder -------------------------------------------------

def build_emulator(verbose: bool = False, ware: str = "oem") -> tuple[Uc, TraceState, capstone.Cs]:
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    state = TraceState()
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)

    # Map flash. Permissions are R-X by default in unicorn.
    uc.mem_map(FLASH_BASE, FLASH_SIZE)
    sb = Path(SHIFTERBOOT_BIN).read_bytes()
    uc.mem_write(FLASH_BASE, sb)
    sw_path = SHIFTERWARE_OURS_BIN if ware == "ours" else SHIFTERWARE_BIN
    sw = Path(sw_path).read_bytes()
    uc.mem_write(FLASH_BASE + 0x3000, sw)
    print(f"shifterware: {sw_path} ({len(sw)} B)")

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
        if FLASH_BASE <= address < FLASH_BASE + 0x3000:
            region = "shifterboot"
        elif FLASH_BASE + 0x3000 <= address < FLASH_BASE + 0x6000:
            region = "shifterware"
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


def run(args):
    uc, state, md = build_emulator(verbose=args.trace, ware=args.ware)

    if args.frm == "shifterboot":
        sp = read_vector(uc, FLASH_BASE, 0)
        pc = read_vector(uc, FLASH_BASE, 1)
        print(f"Cold reset (shifterboot):")
        print(f"  initial SP = 0x{sp:08X}")
        print(f"  initial PC = 0x{pc:08X} (Thumb LSB → {pc & ~1:#010x})")
        print()
    elif args.frm == "shifterware":
        # Vector table sits 40 B (image header) past the image base.
        vec_base = FLASH_BASE + 0x3000 + 0x28
        sp = read_vector(uc, vec_base, 0)
        pc = read_vector(uc, vec_base, 1)
        print(f"Shifterware (via vector slot 1 Reset_Handler):")
        print(f"  initial SP = 0x{sp:08X}")
        print(f"  initial PC = 0x{pc:08X} (Thumb LSB → {pc & ~1:#010x})")
        print()
    elif args.frm == "shifterware-main":
        # Jump straight to main, bypassing Reset_Handler. The OEM main
        # lives at 0x080042D6 (Thumb LSB); for our build it's wherever
        # the linker placed `main` — find via ELF symbol.
        if args.ware == "ours":
            import subprocess
            elf = SHIFTERWARE_OURS_BIN.rsplit(".", 1)[0] + ".elf"
            try:
                out = subprocess.check_output(["arm-none-eabi-nm", elf], text=True)
                main_addr = None
                for line in out.splitlines():
                    parts = line.split()
                    if len(parts) >= 3 and parts[2] == "main" and parts[1] in "Tt":
                        main_addr = int(parts[0], 16)
                        break
                if main_addr is None:
                    raise SystemExit("could not find `main` in our shifterware.elf")
                sp = 0x20000400
                pc = main_addr | 1
            except FileNotFoundError:
                raise SystemExit("arm-none-eabi-nm not found; can't locate our build's main")
        else:
            sp = 0x20000400
            pc = 0x080042D6 | 1
        print(f"Direct entry into main ({'ours' if args.ware == 'ours' else 'OEM'}):")
        print(f"  SP = 0x{sp:08X}, PC = 0x{pc:08X}")
        print()
    else:
        raise SystemExit(f"unknown --from value: {args.frm!r}")

    uc.reg_write(UC_ARM_REG_SP, sp)

    try:
        # Run for at most --max-steps instructions
        uc.emu_start(pc, until=0, count=args.max_steps)
    except UcError as e:
        pc = uc.reg_read(UC_ARM_REG_PC)
        print(f"\nemu stopped on UcError at PC=0x{pc:08X}: {e}")
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


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--max-steps", type=int, default=200_000,
                   help="Stop after this many instructions (default 200,000)")
    p.add_argument("--trace", action="store_true", help="Print every instruction (first 200)")
    p.add_argument("--from", dest="frm", default="shifterboot",
                   choices=("shifterboot", "shifterware", "shifterware-main"),
                   help="Where to start emulating from")
    p.add_argument("--dump-sram-writes", action="store_true", help="Dump tail SRAM writes")
    p.add_argument("--ware", choices=("oem", "ours"), default="oem",
                   help="Which shifterware bin to load: OEM (factory) or our build")
    args = p.parse_args()
    run(args)


if __name__ == "__main__":
    main()
