#!/usr/bin/env python3
"""Unicorn harness that emulates a CC2642R1F just well enough to boot
the bleboot image and trace it through Reset_Handler → SetupTrimDevice
→ ResetISR_body → _auto_init_table → main → bim_dispatch.

The CC2642R1F's peripherals aren't modeled by any public emulator, so
this harness stubs:
  - FCFG1 (factory config) — returns CC2642R1 PG2 IDs and zeros elsewhere
  - ROM_API_TABLE @ 0x100001xx — points each ROM slot at a tiny no-op
    function we inject into a small ROM region (`mov r0, #0; bx lr`)
  - FLASH_FSM_ACK / VIMS_BB_MODE_CHANGING bit-band aliases — returns 0
    so polling loops exit immediately
  - SSI0 + the SPI chip-probe path — returns the MX25L51245G JEDEC
    bytes (0xC2 / 0x19) so `bim_spi_probe_chip` succeeds and the BIM
    can walk its chip table
  - Everything else: reads → 0, writes → logged-and-dropped.

Run as:
  python3 tools/unicorn_boot_trace.py bleboot_1.0.0.bin
  python3 tools/unicorn_boot_trace.py build/bleboot.bin

Both runs trace the same code path, so you can compare behaviour
side-by-side.
"""
import sys
import struct
import subprocess
from pathlib import Path

from unicorn import (
    Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS,
    UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE,
    UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_READ_UNMAPPED, UC_HOOK_MEM_WRITE_UNMAPPED,
    UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC,
    UC_HOOK_INTR,
)
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_R7, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
)

# ---------- Memory map ----------
FLASH_BASE = 0x00000000
FLASH_SIZE = 0x00060000        # 384 KB (covers up to 0x60000)
SRAM_BASE  = 0x20000000
SRAM_SIZE  = 0x00014000        # 80 KB
ROM_BASE   = 0x10000000
ROM_SIZE   = 0x00010000        # 64 KB (covers ROM_API_TABLE + stub functions)
MMIO_BASE  = 0x40000000
MMIO_SIZE  = 0x20100000        # spans through bit-band region + secure alias at 0x6008xxxx
SCS_BASE   = 0xE0000000        # System Control Space (SCB/NVIC/etc.)
SCS_SIZE   = 0x00100000
BLEBOOT_LOAD_ADDR = 0x00056000

# ---------- ROM stub layout ----------
# ROM_API_TABLE @ 0x10000180 is an array of pointers to sub-tables.
# Each sub-table is an array of function pointers. We put the
# sub-tables and the stub functions inside ROM_BASE..ROM_BASE+SIZE.
ROM_API_TABLE_ADDR  = 0x10000180
ROM_SUBTABLE_BASE   = 0x10000300        # where we place sub-tables
ROM_STUB_FN_ADDR    = 0x10000400        # where the no-op stub function lives
ROM_STUB_FN_BYTES   = bytes([
    0x00, 0x20,                          # movs r0, #0
    0x70, 0x47,                          # bx lr
])
ROM_STUB_FN_RET1    = 0x10000408
ROM_STUB_FN_RET1_BYTES = bytes([
    0x01, 0x20,                          # movs r0, #1   (return 1 / "success")
    0x70, 0x47,
])
ROM_STUB_FN_RET0XFF = 0x10000410
ROM_STUB_FN_RET0XFF_BYTES = bytes([
    0xFF, 0x20,                          # movs r0, #0xFF
    0x70, 0x47,
])

# ---------- MMIO known reads ----------
def mmio_read_value(addr):
    """Return the canned value for an MMIO read."""
    # FCFG1 — ICEPICK_DEVICE_ID @ 0x50001318:
    #   PARTNO   bits[27:12] = 0xBB41 (CC2642R1)
    #   PG_REV   bits[31:28] = 0x3 (PG3 → bim_chip_hw_revision returns
    #            21 which is >= 20, passing bim_chip_assert_supported)
    if addr == 0x50001318:
        return (0x3 << 28) | (0xBB41 << 12)
    if addr == 0x500010A0:    # MINOR_HW_REV
        return 0
    if addr == 0x5000131C:    # FCFG1_REVISION
        return 0
    if addr == 0x5000140C:    # MISC_TRIM
        return 0

    # VIMS_BB_MODE_CHANGING (bit-band of VIMS_STAT bit 3) — stay clear
    if addr == 0x4268000C:
        return 0

    # FLASH controller bit-band aliases — return 0 (idle)
    if addr in (0x42600484, 0x42600494):
        return 0

    # AON gates referenced by SetupTrimDevice — return 0 ("disabled"
    # path skips bim_setup_after_cold_reset_cfg1)
    if addr in (0x43280180, 0x43200580):
        return 0

    # SSI0 status bit-band — most reads idle
    # SSI0 data — bim_spi_probe_chip reads JEDEC ID via REMS:
    #   bim_spi_read_rems_id sends 4-byte cmd [0x90, 0xFF, 0xFF, 0x00]
    #   then recvs 2 bytes which it stores at SRAM 0x20000404/05.
    # The SSI ROM slots dispatch to our no-op stubs, so the RX path
    # never actually shifts bytes. Instead we patch SRAM directly
    # after `bim_spi_probe_chip` is entered. See `hook_code` below.

    # AON_PMCTL random reads
    if addr == 0x40090028:
        return 0
    if addr == 0x4008218C:
        return 0
    if addr == 0x40032048:
        return 0

    # PRCM_CLKLOADCTL @ 0x40082028 — bit 1 = LOAD_DONE; bleboot polls
    # for it after every kick. Always report ready.
    if addr == 0x40082028:
        return 0x2

    # SSI0 SR (status) @ 0x4000000C — bit 0 = TFE (TX FIFO empty),
    # bit 2 = RNE (RX FIFO not empty). Return both so polling loops
    # take the "data ready" branch.
    if addr == 0x4000000C:
        return 0xFF

    # Default
    return 0


# ---------- Tracing globals ----------
INSTR_COUNT = 0
MAX_INSTR   = 200_000
TRACE       = []
PANIC_HIT   = False
CHIP_PROBE_PATCHED = False
SYMBOLS = {}   # addr -> name (for trace output)
PC_HITS    = {}   # addr -> count
SPIN_THRESHOLD = 500
SPI_DUMP   = None   # bytes of external SPI flash, if loaded
SPI_READS  = []     # (addr, len, dst) tuples

def symbol_for(addr):
    addr &= ~1
    return SYMBOLS.get(addr, '')

def hook_code(uc, address, size, user_data):
    global INSTR_COUNT, PANIC_HIT, CHIP_PROBE_PATCHED
    INSTR_COUNT += 1
    if INSTR_COUNT > MAX_INSTR:
        print(f'[STOP] hit instruction limit ({MAX_INSTR})')
        uc.emu_stop()
        return

    user_data['last_pc'] = address

    # Any branch into the ROM region: this is a TI ROM-API call.
    if ROM_BASE <= address < (ROM_BASE + ROM_SIZE):
        lr = uc.reg_read(UC_ARM_REG_LR)
        retval = user_data.get('rom_retvals', {}).get(address, 0)
        # PRCM state machine: PRCM[5] = power-on request → next
        # PRCM[13] returns 1. PRCM[6] = power-off request → next
        # returns 2. PRCM[8] (clock) leaves state untouched.
        prcm_on  = user_data.get('prcm_on_addr')
        prcm_off = user_data.get('prcm_off_addr')
        prcm_status = user_data.get('prcm_status_addr')
        if address == prcm_on:
            user_data['rom_retvals'][prcm_status] = 1
        elif address == prcm_off:
            user_data['rom_retvals'][prcm_status] = 2
        # Special case: SSIDataGet (SSI ROM slot [3]) — signature is
        # `SSIDataGet(uint32_t base, uint32_t *data)`. The byte the
        # slave shifted in is stored at *r1; the function returns a
        # status. We pop the next byte from `jedec_queue` (set when
        # `bim_spi_recv_bytes` is entered with dst=0x20000404) and
        # write it to the pointer in r1.
        if address == user_data.get('ssi_dataget_addr'):
            r1 = uc.reg_read(UC_ARM_REG_R1)
            q = user_data.get('jedec_queue', [])
            byte_val = q.pop(0) if q else 0
            if r1:
                uc.mem_write(r1, bytes([byte_val]))
            retval = 1   # SSI status: byte available / success
        uc.reg_write(UC_ARM_REG_R0, retval)
        user_data['resume_at'] = lr | 1
        if address not in user_data.setdefault('rom_pcs_seen', set()):
            user_data['rom_pcs_seen'].add(address)
        uc.emu_stop()
        return

    name = symbol_for(address)
    # Log every function entry (not just unique transitions)
    if name:
        sp = uc.reg_read(UC_ARM_REG_SP)
        r0 = uc.reg_read(UC_ARM_REG_R0)
        r1 = uc.reg_read(UC_ARM_REG_R1)
        TRACE.append((INSTR_COUNT, address, name, sp, r0, r1))

        # When bim_spi_recv_bytes is called with dst=0x20000404
        # (the chip-id-byte globals), queue MX25L51245G JEDEC bytes
        # so the next two SSIDataGet ROM calls return 0xC2 / 0x19.
        # (Match `.part.0` GCC-split variants too.)
        if name.startswith('bim_spi_recv_bytes') and r0 == 0x20000404:
            user_data['jedec_queue'] = [0xC2, 0x19]

        # Intercept bim_spi_flash_read(addr, len, dst): synthesize the
        # read from SPI_DUMP if loaded, otherwise let it run (and read
        # zeros via the ROM stubs). Skipping the function body bypasses
        # the SSI plumbing entirely, which is faster and more accurate
        # than feeding bytes through the SSIDataGet queue.
        if name.startswith('bim_spi_flash_read') and SPI_DUMP is not None:
            r2 = uc.reg_read(UC_ARM_REG_R2)
            addr_, len_, dst_ = r0, r1, r2
            SPI_READS.append((addr_, len_, dst_))
            if dst_ and len_:
                end = addr_ + len_
                if end <= len(SPI_DUMP):
                    uc.mem_write(dst_, SPI_DUMP[addr_:end])
                else:
                    # Out-of-range read — fill with 0xFF (erased flash)
                    uc.mem_write(dst_, b'\xff' * len_)
            uc.reg_write(UC_ARM_REG_R0, 1)   # success
            lr = uc.reg_read(UC_ARM_REG_LR)
            user_data['resume_at'] = lr | 1
            uc.emu_stop()
            return

    PC_HITS[address] = PC_HITS.get(address, 0) + 1
    if PC_HITS[address] == SPIN_THRESHOLD:
        # Likely an infinite loop — report and stop
        lr = uc.reg_read(UC_ARM_REG_LR)
        print(f'[SPIN] pc=0x{address:08x} hit {SPIN_THRESHOLD} times. lr=0x{lr:08x}')
        print('  Recent unique function entries (last 15):')
        seen = set()
        recent = []
        for entry in reversed(TRACE):
            if entry[1] not in seen:
                seen.add(entry[1])
                recent.append(entry)
                if len(recent) >= 15:
                    break
        for entry in reversed(recent):
            n, addr, nm, sp, r0, r1 = entry
            print(f'    [{n:7d}]  0x{addr:08x}  {nm:30s}  r0=0x{r0:08x}')
        uc.emu_stop()

    # Detect the b . panic loop (handler @ HardFault_Handler/Default_Handler)
    # which has shape `e7 fe` (b .)
    insn_bytes = bytes(uc.mem_read(address, 2))
    if insn_bytes == b'\xfe\xe7':
        PANIC_HIT = True
        # one extra cycle then stop
        TRACE.append((INSTR_COUNT, address, '[TRAP LOOP `b .`]', 0, 0, 0))
        uc.emu_stop()

def hook_mem_read(uc, access, address, size, value, user_data):
    if MMIO_BASE <= address < (MMIO_BASE + MMIO_SIZE):
        val = mmio_read_value(address)
        uc.mem_write(address, val.to_bytes(4, 'little'))

def hook_unmapped(uc, access, address, size, value, user_data):
    pc = uc.reg_read(UC_ARM_REG_PC)
    lr = uc.reg_read(UC_ARM_REG_LR)
    print(f'[UNMAPPED] @ pc=0x{pc:08x} lr=0x{lr:08x}  access={access} addr=0x{address:08x} size={size}')
    # Print the last few function entries before the crash
    print('  Last 10 trace entries:')
    for entry in TRACE[-10:]:
        n, addr, name, sp, r0, r1 = entry
        print(f'    [{n:7d}]  0x{addr:08x}  {name:30s}  sp=0x{sp:08x}  r0=0x{r0:08x}  r1=0x{r1:08x}')
    uc.emu_stop()
    return False

def hook_intr(uc, intno, user_data):
    pc = uc.reg_read(UC_ARM_REG_PC)
    lr = uc.reg_read(UC_ARM_REG_LR)
    print(f'[INTR] intno={intno} pc=0x{pc:08x} lr=0x{lr:08x}')
    print('  Last 8 trace entries:')
    for entry in TRACE[-8:]:
        n, addr, name, sp, r0, r1 = entry
        print(f'    [{n:7d}]  0x{addr:08x}  {name:30s}  r0=0x{r0:08x}')
    uc.emu_stop()


# ---------- Symbol-table extraction ----------
def load_symbols_from_elf(elf_path):
    """Parse `nm` output for function addresses."""
    symbols = {}
    try:
        out = subprocess.check_output(
            ['arm-none-eabi-nm', elf_path], text=True, stderr=subprocess.DEVNULL)
    except (FileNotFoundError, subprocess.CalledProcessError):
        return symbols
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ('T', 't', 'W', 'w'):
            try:
                addr = int(parts[0], 16) & ~1
            except ValueError:
                continue
            symbols[addr] = parts[2]
    return symbols

# Hardcoded OEM symbol table (from bleboot/docs/progress.md).
OEM_SYMBOLS = {
    0x000567a0: 'bim_spi_flash_program',
    0x000565e0: 'bim_memcpy_aligned',
    0x000563c8: 'bim_ssi_init',
    0x00056254: 'bim_full_scan_and_launch',
    0x00056490: 'bim_setup_after_cold_reset_cfg1',
    0x0005653c: 'bim_crc32_buffer',
    0x0005667c: 'SetupTrimDevice',
    0x00056714: 'bim_iflash_copy_from_spi',
    0x000567a0: 'bim_spi_flash_program',
    0x00056824: 'bim_quick_scan_and_launch',
    0x000568a6: 'HardFault_Handler',
    0x000568a8: 'bim_verify_and_launch_image',
    0x00056924: 'cinit_byte_stream_copy',
    0x0005698c: 'bim_spi_probe_chip',
    0x000569e4: 'bim_spi_flash_read',
    0x00056a38: 'bim_periph_power_off',
    0x00056a88: 'bim_flash_prepare',
    0x00056ad4: 'bim_spi_wait_wip',
    0x00056b1c: 'bim_slot_iterator',
    0x00056b64: 'bim_panic_prep',
    0x00056bac: 'bim_chip_hw_revision',
    0x00056bf0: '_auto_init_table',
    0x00056c34: 'bim_iflash_check_range_blank',
    0x00056c76: 'Default_Handler',
    0x00056c78: 'bim_spi_recv_bytes',
    0x00056cb8: 'bim_oad_find_image_addr',
    0x00056cf4: 'bim_spi_read_rems_id',
    0x00056d30: 'bim_iflash_read_paged',
    0x00056d6a: 'bim_spi_release_from_dpd',
    0x00056da2: 'NMI_Handler',
    0x00056da4: 'bim_setup_adi_step',
    0x00056dd8: 'ResetISR_body',
    0x00056e0c: 'bim_iflash_session_begin',
    0x00056e40: 'bim_iflash_read',
    0x00056e72: 'bim_iflash_program',
    0x00056ea4: 'bim_spi_send_bytes',
    0x00056ed4: 'bim_spi_write_enable',
    0x00056f00: 'bim_iflash_program_flat',
    0x00056f2a: 'bim_dispatch',
    0x00056f50: 'crc32_ieee_byte_step',
    0x00056f74: 'oad_magic_match',
    0x00056f98: 'oad_magic_match2',
    0x00056fbc: 'bim_iflash_check_slot_blank',
    0x00056fe0: 'bim_ssi_rx_drain',
    0x00057000: 'main',
    0x00057020: 'bim_chip_family',
    0x0005703c: 'bim_iflash_program_via_rom',
    0x00057058: 'bim_iflash_rom_blank_check',
    0x00057074: 'auto_init_zero_fill',
    0x00057090: 'bim_iflash_session_end',
    0x000570ac: 'bim_flash_release',
    0x000570c8: 'bim_spi_deep_power_down',
    0x000570e2: 'bim_spi_wait_idle',
    0x000570fa: 'bim_memcpy_safe',
    0x00057110: 'bim_chip_assert_supported',
    0x00057126: 'Reset_Handler',
    0x00057138: 'dio4_set',
    0x00057148: 'cinit_generic_copy',
    0x00057156: 'bim_launch_image',
    0x00057164: 'bim_irq_disable_save',
    0x00057170: 'bim_irq_enable_restore',
    0x0005717c: 'bim_get_chip_entry',
    0x00057188: 'dio4_clear',
    0x00057194: 'bim_panic_indicate',
    0x000571a0: '_system_pre_init',
    0x000571a4: '_exit',
}


def run(binary_path, symbols, label, spi_dump_path=None):
    global INSTR_COUNT, TRACE, PANIC_HIT, CHIP_PROBE_PATCHED, SYMBOLS, SPI_DUMP, SPI_READS
    INSTR_COUNT = 0
    TRACE = []
    PANIC_HIT = False
    CHIP_PROBE_PATCHED = False
    SYMBOLS = symbols
    SPI_READS = []
    if spi_dump_path and Path(spi_dump_path).exists():
        SPI_DUMP = Path(spi_dump_path).read_bytes()
        print(f'  Loaded SPI dump: {spi_dump_path} ({len(SPI_DUMP)} B = {len(SPI_DUMP)/1024/1024:.1f} MB)')
    else:
        SPI_DUMP = None

    bin_bytes = Path(binary_path).read_bytes()

    print(f'\n=== {label}: {binary_path} ({len(bin_bytes)} B) ===')

    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    uc.mem_map(FLASH_BASE, FLASH_SIZE, UC_PROT_READ | UC_PROT_EXEC)
    uc.mem_map(SRAM_BASE,  SRAM_SIZE,  UC_PROT_READ | UC_PROT_WRITE)
    uc.mem_map(ROM_BASE,   ROM_SIZE,   UC_PROT_READ | UC_PROT_EXEC)
    uc.mem_map(MMIO_BASE,  MMIO_SIZE,  UC_PROT_READ | UC_PROT_WRITE)
    uc.mem_map(SCS_BASE,   SCS_SIZE,   UC_PROT_READ | UC_PROT_WRITE)

    # Load binary at the bleboot flash address.
    uc.mem_write(BLEBOOT_LOAD_ADDR, bin_bytes)

    # Build a fake ROM_API_TABLE. Most CC2642R1F ROM tables are flat
    # (subtable[k] = function pointer), but FLASH (top_idx 13) has an
    # extra indirection level: subtable[13][0] = pointer to a FUNC
    # table, and FUNC[k] = the actual function pointer.
    NUM_TOP   = 32
    NUM_SLOTS = 32
    PLACEHOLDER_BASE = ROM_BASE + 0x1000   # 0x10001000
    FLASH_FUNC_TABLE_BASE = ROM_BASE + 0x2000  # 0x10002000
    rom_retvals = {}
    subtables_addr = []
    for top_idx in range(NUM_TOP):
        subt_addr = ROM_SUBTABLE_BASE + top_idx * NUM_SLOTS * 4
        for slot in range(NUM_SLOTS):
            placeholder = PLACEHOLDER_BASE + (top_idx * NUM_SLOTS + slot) * 4
            uc.mem_write(subt_addr + slot * 4,
                         (placeholder | 1).to_bytes(4, 'little'))
            rom_retvals[placeholder] = 0
        subtables_addr.append(subt_addr)

    # FLASH table override: subtable[13][0] = pointer to FUNC table.
    # FUNC[k] = unique placeholder addresses (different namespace from
    # the main subtable[13] slots so we can configure them separately).
    flash_func_table = FLASH_FUNC_TABLE_BASE
    uc.mem_write(subtables_addr[13], flash_func_table.to_bytes(4, 'little'))
    for slot in range(NUM_SLOTS):
        placeholder = flash_func_table + 0x100 + slot * 4   # offset to avoid collision
        uc.mem_write(flash_func_table + slot * 4,
                     (placeholder | 1).to_bytes(4, 'little'))
        rom_retvals[placeholder] = 0

    # Patch the top-level table.
    for top_idx in range(NUM_TOP):
        uc.mem_write(ROM_API_TABLE_ADDR + top_idx * 4,
                     subtables_addr[top_idx].to_bytes(4, 'little'))

    # Per-slot return-value overrides — only what the BIM cares about:
    def slot_addr(top_idx, slot):
        return PLACEHOLDER_BASE + (top_idx * NUM_SLOTS + slot) * 4

    # PRCM (ROM_API_TABLE[14] = 0x100001B8): slot[13] reports power
    # status; bim_ssi_init waits for ==1 (PD on), bim_periph_power_off
    # waits for ==2 (PD off). Return 1 so the boot-time bring-up path
    # exits its retry loop. (bim_periph_power_off only runs on flash
    # tear-down, not on the happy boot path we're tracing.)
    rom_retvals[slot_addr(14, 13)] = 1

    # VIMS (ROM_API_TABLE[22] = 0x100001D8): slot[2] is VIMSModeGet;
    # return 0 so bim_iflash_session_begin's idle-check skips.
    # (Default 0 already, listed for clarity.)
    rom_retvals[slot_addr(22, 2)] = 0

    # SSI ROM helpers (ROM_API_TABLE[17] = 0x100001C4):
    # Slot [1] = SSIDataPut (no return value used)
    # Slot [2] = SSIDataPutNonBlocking → return 1 (success)
    # Slot [3] = SSIDataGet → returns 0 (the dummy byte from SPI flash
    #           probing — bim_spi_probe_chip is separately patched to
    #           inject JEDEC c2/19 into SRAM directly)
    rom_retvals[slot_addr(17, 1)] = 0
    rom_retvals[slot_addr(17, 2)] = 1   # non-blocking put: success
    rom_retvals[slot_addr(17, 3)] = 0
    rom_retvals[slot_addr(17, 4)] = 0   # non-blocking get: empty (drain done)
    ssi_dataget_addr = slot_addr(17, 3)

    # FLASH ROM helpers (ROM_API_TABLE[10] = 0x100001A8):
    # Slot [5] = FlashCheckBlank (used by bim_iflash_rom_blank_check)
    #            → return non-zero ("blank") so the OAD promote path
    #            can proceed.
    rom_retvals[slot_addr(10, 5)] = 1
    rom_retvals[slot_addr(10, 6)] = 0   # FlashProgram: 0 = success

    user_data_ref = {'rom_retvals': rom_retvals,
                     'ssi_dataget_addr': ssi_dataget_addr,
                     'prcm_on_addr':     slot_addr(14, 5),
                     'prcm_off_addr':    slot_addr(14, 6),
                     'prcm_status_addr': slot_addr(14, 13)}

    # Install hooks.
    user_data = {'last_name': None}
    user_data.update(user_data_ref)
    uc.hook_add(UC_HOOK_CODE, hook_code, user_data)
    uc.hook_add(UC_HOOK_MEM_READ, hook_mem_read, None,
                MMIO_BASE, MMIO_BASE + MMIO_SIZE)
    uc.hook_add(
        UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED |
        UC_HOOK_MEM_UNMAPPED, hook_unmapped)
    uc.hook_add(UC_HOOK_INTR, hook_intr)

    # Debug: verify ROM table layout
    rom_28 = struct.unpack('<I', bytes(uc.mem_read(0x100001F0, 4)))[0]
    print(f'  ROM_API_TABLE[28] @ 0x100001F0 = 0x{rom_28:08x}')
    if rom_28 != 0:
        hapi_18 = struct.unpack('<I', bytes(uc.mem_read(rom_28 + 18*4, 4)))[0]
        print(f'  hapi[18] @ 0x{rom_28 + 18*4:08x} = 0x{hapi_18:08x}')
    rom_13 = struct.unpack('<I', bytes(uc.mem_read(0x100001B4, 4)))[0]
    print(f'  ROM_API_TABLE[13] (FLASH) @ 0x100001B4 = 0x{rom_13:08x}')
    if rom_13 != 0:
        flash_self = struct.unpack('<I', bytes(uc.mem_read(rom_13, 4)))[0]
        flash_17 = struct.unpack('<I', bytes(uc.mem_read(rom_13 + 17*4, 4)))[0]
        print(f'  FLASH_table[0] (= self) @ 0x{rom_13:08x} = 0x{flash_self:08x}')
        print(f'  FLASH_table[17] @ 0x{rom_13 + 17*4:08x} = 0x{flash_17:08x}')

    # Read initial SP and Reset_Handler from the vector table.
    init_sp = struct.unpack('<I', bytes(uc.mem_read(BLEBOOT_LOAD_ADDR, 4)))[0]
    reset_handler = struct.unpack('<I',
        bytes(uc.mem_read(BLEBOOT_LOAD_ADDR + 4, 4)))[0]
    print(f'  init_sp = 0x{init_sp:08x}')
    print(f'  reset_handler = 0x{reset_handler:08x}'
          f'  ({symbol_for(reset_handler)})')

    uc.reg_write(UC_ARM_REG_SP, init_sp)
    next_pc = reset_handler
    while True:
        try:
            uc.emu_start(next_pc, 0, count=MAX_INSTR - INSTR_COUNT)
        except Exception as e:
            pc = uc.reg_read(UC_ARM_REG_PC)
            print(f'  [EXCEPTION] {e}  pc=0x{pc:08x}  insn#={INSTR_COUNT}')
            break
        # Did we stop because of a ROM-call intercept? If so, resume
        # at the saved LR.
        if user_data.get('resume_at') is not None:
            next_pc = user_data.pop('resume_at')
            continue
        # Normal stop (panic loop, instruction limit, etc.) — done.
        break

    # Trace summary
    print(f'\n  instructions executed: {INSTR_COUNT}')
    print(f'  panic loop hit: {PANIC_HIT}')
    print(f'  function call sequence (first 50):')
    for entry in TRACE[:50]:
        n, addr, name, sp, r0, r1 = entry
        print(f'    [{n:7d}]  0x{addr:08x}  {name:30s}  sp=0x{sp:08x}  r0=0x{r0:08x}  r1=0x{r1:08x}')

    # SRAM state of the cinit-touched region
    sram_300 = bytes(uc.mem_read(0x20000300, 0x110))
    nonzero = [(i, b) for i, b in enumerate(sram_300) if b != 0]
    print(f'\n  SRAM[0x20000300..0x20000410] non-zero bytes:'
          f' {len(nonzero)} of {len(sram_300)}')
    if nonzero and len(nonzero) < 32:
        for i, b in nonzero:
            print(f'    0x{0x20000300+i:08x}: 0x{b:02x}')

    return TRACE


def summarize(label, trace, oem_path, ours_path, oem_trace=None):
    """Print a concise milestone summary for a run."""
    milestones = [
        'Reset_Handler', 'SetupTrimDevice', 'ResetISR_body',
        '_system_pre_init', '_auto_init_table', 'auto_init_zero_fill',
        'cinit_byte_stream_copy', 'main', 'bim_dispatch',
        'bim_full_scan_and_launch', 'bim_flash_prepare', 'bim_ssi_init',
        'bim_spi_release_from_dpd', 'bim_spi_probe_chip',
        'bim_verify_and_launch_image', 'bim_quick_scan_and_launch',
    ]
    hits = {m: None for m in milestones}
    for n, addr, name, sp, r0, r1 in trace:
        base = name.split('.')[0]
        if base in hits and hits[base] is None:
            hits[base] = n
    print(f'\n  Milestone reached (instruction #):')
    for m in milestones:
        ok = '✓' if hits[m] is not None else '✗'
        n_str = f'#{hits[m]:5d}' if hits[m] is not None else '   --'
        print(f'    {ok} {m:30s} {n_str}')


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    binary = sys.argv[1]
    if 'compare' in binary:
        # Run both, side-by-side
        dump = 'SPI.rom'
        oem_t = run('bleboot_1.0.0.bin', OEM_SYMBOLS, 'OEM', spi_dump_path=dump)
        summarize('OEM', oem_t, None, None)
        elf = 'build/bleboot.elf'
        ours_t = run('build/bleboot.bin', load_symbols_from_elf(elf), 'OURS', spi_dump_path=dump)
        summarize('OURS', ours_t, None, None, oem_t)
        return 0
    if 'build/bleboot' in binary:
        elf = binary.replace('.bin', '.elf')
        symbols = load_symbols_from_elf(elf)
        label = 'OURS'
    else:
        symbols = OEM_SYMBOLS
        label = 'OEM'
    t = run(binary, symbols, label, spi_dump_path='SPI-Flash_F88A5E4F9ECB.rom')
    summarize(label, t, None, None)
    return 0

if __name__ == '__main__':
    sys.exit(main())
