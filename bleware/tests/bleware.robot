*** Settings ***
Documentation     Renode smoke tests for the VanMoof bleware image — the BLE-MCU
...               application on the CC2642R1F (ARM Cortex-M4F).
...               Build the test image first:  make -C bleware test
...               then run:  renode-test bleware/tests/bleware.robot
...               `make test` relinks the objects retaining the self-contained leaf
...               routines (crc32_le, crc16_modbus, lcg_random_u15) that the minimal
...               reconstruction's production link drops via --gc-sections.
...
...               The image's vector table sits at flash 0x00000090 (just past the
...               0x90-byte OAD header). The CRC + PRNG primitives are the functional
...               surface exercised here.
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}

*** Variables ***
${ELF}            @${CURDIR}/../build/bleware_test.elf
${REPL}           @${CURDIR}/bleware.repl
${VTOR}           0x00000090      # app vector table base (SP @ +0, reset @ +4)
${SCRATCH}        0x20007000      # leaf-test data buffer, high in the 80 KB SRAM
${STACKTOP}       0x20014000      # top of SRAM
# LCG state lives at the very base of SRAM (data/bss): the two lock-callback
# pointers and the 32-bit state.
${TK_STATE}          0x20007100      # scratch timekeeper state array (5 u32) for the leaf tests

*** Keywords ***
Create Boot Machine
    [Documentation]    Build the machine, load the app image and point the vector
    ...                table at 0x00000090. Register a reset macro so a soft reset
    ...                restores the offset.
    Execute Command           mach create "bleware"
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus LoadELF ${ELF}
    Execute Command           cpu VectorTableOffset ${VTOR}
    Execute Command           macro reset "cpu VectorTableOffset ${VTOR}"

Create Leaf Machine
    [Documentation]    Minimal machine for direct-call (leaf) tests: load the platform
    ...                and ELF, nothing else. The CRC/PRNG routines touch no trim/clock
    ...                status registers, so none of the boot hooks are needed here.
    Execute Command           mach create
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus LoadELF ${ELF}

Resolve Symbol
    [Documentation]    Look up a symbol's address in the loaded ELF, stripped.
    [Arguments]    ${name}
    ${addr}=    Execute Command    sysbus GetSymbolAddress "${name}"
    ${addr}=    Strip String       ${addr}
    [Return]    ${addr}

Hook Return
    [Documentation]    Hook a function (by symbol) to return immediately — cpu.PC =
    ...                cpu.LR before its prologue, so callers proceed as if it ran.
    [Arguments]    ${symbol}
    ${a}=    Resolve Symbol    ${symbol}
    Execute Command    cpu AddHook ${a} "cpu.PC = cpu.LR"

Return Trap
    [Documentation]    Address of _exit (a `b .` self-loop) with the Thumb bit set,
    ...                used as LR so a called function parks the CPU harmlessly on
    ...                return (BX LR) instead of running off into undefined code.
    ...                (bleware has no Default_Handler symbol — its default vectors point
    ...                into ROM — but _exit is the same `b .` shape.)
    ${trap}=    Resolve Symbol     _exit
    ${trap}=    Evaluate           ${trap} | 1
    [Return]    ${trap}

Call Leaf Function
    [Documentation]    Invoke a Thumb function by symbol name and return R0 (its return
    ...                value), stripped. Arguments R0..R3 must already be set. Sets up a
    ...                fresh stack, points LR at the Return Trap self-loop, runs a brief
    ...                window so the call completes, then reads R0.
    [Arguments]    ${symbol}
    ${addr}=    Resolve Symbol    ${symbol}
    ${trap}=    Return Trap
    Execute Command           cpu SetRegister 13 ${STACKTOP}
    Execute Command           cpu SetRegister 14 ${trap}
    Execute Command           cpu PC ${addr}
    Execute Command           emulation RunFor "0.002"
    ${r0}=    Execute Command    cpu GetRegister 0
    ${r0}=    Strip String      ${r0}
    [Return]    ${r0}

Load Scratch Bytes
    [Documentation]    Write a list of byte literals into the SRAM scratch buffer
    ...                (${SCRATCH}), one byte per ascending address.
    [Arguments]    @{bytes}
    ${i}=    Set Variable    ${0}
    FOR    ${b}    IN    @{bytes}
        ${addr}=    Evaluate    ${SCRATCH} + ${i}
        Execute Command    sysbus WriteByte ${addr} ${b}
        ${i}=    Evaluate    ${i} + 1
    END

*** Test Cases ***
Vector Table Is Well Formed
    [Documentation]    Static check (no execution): vector slot 0 (initial SP) points
    ...                into SRAM and slot 1 (reset PC) into app flash with the Thumb bit
    ...                set — the CPU resets cleanly into this image past the OAD header.
    Create Boot Machine
    ${sp}=    Execute Command    sysbus ReadDoubleWord 0x00000090
    ${pc}=    Execute Command    sysbus ReadDoubleWord 0x00000094
    # Initial SP in SRAM (0x2001_3A00).
    Should Match Regexp    ${sp}    (?i)0x0*2001[0-9a-f]+
    # Reset vector in app flash (0x0000_0xxx) with the Thumb bit (odd) set.
    Should Match Regexp    ${pc}    (?i)0x0*[0-9a-f]*[13579bdf]

Reset Chain Reaches Main
    [Documentation]    The startup chain — Reset_Handler (FPU enable, silicon trim,
    ...                cinit C-runtime init) — must hand control to main(). SetupTrimDevice
    ...                drives CC2642 trim MMIO that isn't modelled, so it is skipped;
    ...                everything else runs unchanged.
    Create Boot Machine
    Create Log Tester         10
    Hook Return               SetupTrimDevice
    ${main}=    Resolve Symbol    main
    Execute Command           cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Start Emulation
    Wait For Log Entry        REACHED_MAIN

# --- CRC primitives (direct leaf calls) --------------------------------------
# crc32_le is the reflected CRC-32/zlib core (poly 0xEDB88320) the OAD/secrets
# paths use; it returns the *un*-finalised value (no ^0xFFFFFFFF), so the standard
# zlib CRC is the result XOR 0xFFFFFFFF. crc16_modbus is the CRC-16 (poly 0xA001)
# that validates backoffice GATT payloads.

CRC32 Standard Check Value
    [Documentation]    crc32_le(seed=0xFFFFFFFF, "123456789", 9) returns 0x340BC6D9;
    ...                XORed with 0xFFFFFFFF that is 0xCBF43926 — the canonical CRC32-IEEE
    ...                check value. Validates the core against an external reference.
    Create Leaf Machine
    Load Scratch Bytes    0x31  0x32  0x33  0x34  0x35  0x36  0x37  0x38  0x39
    Execute Command       cpu SetRegister 0 0xFFFFFFFF
    Execute Command       cpu SetRegister 1 ${SCRATCH}
    Execute Command       cpu SetRegister 2 9
    ${raw}=    Call Leaf Function    crc32_le
    ${final}=    Evaluate    (${raw} ^ 0xFFFFFFFF) & 0xFFFFFFFF
    Should Be Equal As Integers    ${final}    0xCBF43926

CRC32 Of A Zero-Length Buffer Returns The Seed
    [Documentation]    With len 0 the loop never runs, so crc32_le returns the seed
    ...                unchanged — guards the `while (len != 0)` boundary.
    Create Leaf Machine
    Execute Command       cpu SetRegister 0 0x12345678
    Execute Command       cpu SetRegister 1 ${SCRATCH}
    Execute Command       cpu SetRegister 2 0
    ${raw}=    Call Leaf Function    crc32_le
    Should Be Equal As Integers    ${raw}    0x12345678

CRC16 Modbus Standard Check Value
    [Documentation]    crc16_modbus("123456789", 9, seed=0xFFFF) must equal 0x4B37 — the
    ...                canonical Modbus/CRC-16 check value (poly 0xA001, init 0xFFFF),
    ...                matching the STM32 firmwares' modbus_crc16.
    Create Leaf Machine
    Load Scratch Bytes    0x31  0x32  0x33  0x34  0x35  0x36  0x37  0x38  0x39
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 9
    Execute Command       cpu SetRegister 2 0xFFFF
    ${crc}=    Call Leaf Function    crc16_modbus
    Should Be Equal As Integers    ${crc}    0x4B37

# --- LCG pseudo-random helper (direct leaf call) -----------------------------
# lcg_random_u15 is the Borland/glibc LCG (state = 1103515245*state + 12345),
# returning bits [30:16] as a 15-bit value. It wraps the update in a critical
# section via two RAM-resident lock callbacks installed at runtime; for a leaf
# call we point those at a `bx lr` stub so the body runs uncritically.

LCG Random Produces The Expected Sequence Value
    [Documentation]    Seed the LCG state to 1 and confirm one step yields 0x41C6:
    ...                (1103515245*1 + 12345) = 0x41C67EA6, bits [30:16] = 0x41C6. The
    ...                lock-enter/exit callback pointers are aimed at a 2-byte `bx lr`
    ...                stub in scratch so the critical-section wrappers just return.
    Create Leaf Machine
    # Resolve the lock-callback pointers + state by symbol (their SRAM addresses shift
    # between builds, so never hard-code them).
    ${enter}=    Resolve Symbol    g_lcg_lock_enter
    ${exit}=     Resolve Symbol    g_lcg_lock_exit
    ${state}=    Resolve Symbol    g_lcg_state
    # `bx lr` (Thumb 0x4770) stub for the lock callbacks.
    Execute Command    sysbus WriteWord ${SCRATCH} 0x4770
    ${stub}=    Evaluate    ${SCRATCH} | 1
    Execute Command    sysbus WriteDoubleWord ${enter} ${stub}
    Execute Command    sysbus WriteDoubleWord ${exit} ${stub}
    Execute Command    sysbus WriteDoubleWord ${state} 0x00000001
    ${r}=    Call Leaf Function    lcg_random_u15
    Should Be Equal As Integers    ${r}    0x41C6
    # The state advanced to the full 32-bit LCG output.
    ${st}=    Execute Command    sysbus ReadDoubleWord ${state}
    ${st}=    Strip String       ${st}
    Should Be Equal As Integers    ${st}    0x41C67EA6

# --- Misc string + GATT-dispatch leaves --------------------------------------

Monitor Strlen Counts The String Length
    [Documentation]    monitor_strlen is the console's NUL-terminated string length —
    ...                a plain `while (*s++) n++`. Confirm it against a known string and
    ...                the empty-string boundary.
    Create Leaf Machine
    Load Scratch Bytes    0x48  0x65  0x6C  0x6C  0x6F  0x00     # "Hello\0"
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    ${n}=    Call Leaf Function    monitor_strlen
    Should Be Equal As Integers    ${n}    5
    Create Leaf Machine
    Load Scratch Bytes    0x00                                   # ""
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    ${z}=    Call Leaf Function    monitor_strlen
    Should Be Equal As Integers    ${z}    0

GATT Characteristic UUID Matcher Resolves The Index
    [Documentation]    svc_5560_char_uuid_to_index maps a GATT attribute to its index in
    ...                the service's 9-entry 128-bit-UUID table: it checks the attribute
    ...                type is 0x10 (128-bit UUID) and linearly matches the 16-byte UUID at
    ...                attr+4 against the table, returning the entry index or 0xFF. Seed a
    ...                distinctive UUID into table entry 2 and the matching attribute, and
    ...                confirm the three branches: correct index, no-match, wrong-type.
    # (A) attribute UUID matches table entry 2 -> index 2
    Create Leaf Machine
    ${tbl}=    Resolve Symbol    g_svc_5560_uuid_table
    ${e2}=     Evaluate    ${tbl} + 0x20                         # entry 2, byte 0
    Execute Command    sysbus WriteByte ${e2} 0xAB
    Execute Command    sysbus WriteByte ${SCRATCH} 0x10          # ATTR_TYPE_128BIT_UUID
    ${u}=    Evaluate    ${SCRATCH} + 4                          # UUID byte 0
    Execute Command    sysbus WriteByte ${u} 0xAB
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    ${a}=    Call Leaf Function    svc_5560_char_uuid_to_index
    Should Be Equal As Integers    ${a}    2
    # (B) a 128-bit UUID that matches no table entry -> 0xFF
    Create Leaf Machine
    Execute Command    sysbus WriteByte ${SCRATCH} 0x10
    ${u2}=    Evaluate    ${SCRATCH} + 4
    Execute Command    sysbus WriteByte ${u2} 0xCD
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    ${b}=    Call Leaf Function    svc_5560_char_uuid_to_index
    Should Be Equal As Integers    ${b}    0xFF
    # (C) a non-128-bit-UUID attribute type -> 0xFF (no match attempted)
    Create Leaf Machine
    Execute Command    sysbus WriteByte ${SCRATCH} 0x01
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    ${c}=    Call Leaf Function    svc_5560_char_uuid_to_index
    Should Be Equal As Integers    ${c}    0xFF

GATT UUID Matcher Unwinds Past A CCCD Descriptor
    [Documentation]    When the dispatched attribute is a Client Characteristic Config
    ...                Descriptor (type 0x02, descriptor UUID 0x2902 at +4), the matcher
    ...                walks 0x10 bytes backward to the owning characteristic's 128-bit-UUID
    ...                attribute before matching. Lay out the UUID attribute then a CCCD one
    ...                slot above it, dispatch on the CCCD, and confirm it resolves to the
    ...                characteristic's table index (2).
    Create Leaf Machine
    ${tbl}=    Resolve Symbol    g_svc_5560_uuid_table
    ${e2}=     Evaluate    ${tbl} + 0x20
    Execute Command    sysbus WriteByte ${e2} 0xAB
    # UUID attribute at SCRATCH (type 0x10, UUID byte0 = 0xAB -> entry 2).
    Execute Command    sysbus WriteByte ${SCRATCH} 0x10
    ${u}=    Evaluate    ${SCRATCH} + 4
    Execute Command    sysbus WriteByte ${u} 0xAB
    # CCCD descriptor one 0x10 slot above (type 0x02, UUID 0x2902 LE at +4).
    ${cccd}=    Evaluate    ${SCRATCH} + 0x10
    Execute Command    sysbus WriteByte ${cccd} 0x02
    ${cu0}=    Evaluate    ${cccd} + 4
    ${cu1}=    Evaluate    ${cccd} + 5
    Execute Command    sysbus WriteByte ${cu0} 0x02
    Execute Command    sysbus WriteByte ${cu1} 0x29
    # The 0x10 attr stride means the CCCD record's type byte lands at UUID-offset 0x0C
    # of the characteristic's 16-byte window the matcher compares, so mirror it in the
    # table entry — otherwise the post-unwind 16-byte compare would miss on that byte.
    ${e2c}=    Evaluate    ${tbl} + 0x20 + 0x0C
    Execute Command    sysbus WriteByte ${e2c} 0x02
    # Dispatch on the CCCD; the matcher unwinds to the UUID attribute -> index 2.
    Execute Command    cpu SetRegister 0 ${cccd}
    ${r}=    Call Leaf Function    svc_5560_char_uuid_to_index
    Should Be Equal As Integers    ${r}    2

# --- OAD image header (static metadata the BIM validates) ---------------------
# The application image carries a TI OAD core header at flash 0x00000000 — the
# metadata bleboot's BIM reads to decide whether to launch it. This is a static
# check of the documented fields (no execution), the BLE analogue of the vector-
# table test.

OAD Image Header Is Well Formed
    [Documentation]    Validate the OAD core header at flash 0x00000000: the "OAD NVM1"
    ...                marker, image type 0x07 (application), the BIM-version floor 0x03,
    ...                the post-header program entry offset (0x90, where the vector table
    ...                sits), the 0x2C core-header length, and the 1.4.01 software version.
    Create Leaf Machine
    # imgID[0..7] = "OAD NVM1" (two little-endian words).
    ${id0}=    Execute Command    sysbus ReadDoubleWord 0x00000000
    Should Match Regexp    ${id0}    (?i)0x0*2044414F
    ${id1}=    Execute Command    sysbus ReadDoubleWord 0x00000004
    Should Match Regexp    ${id1}    (?i)0x0*314D564E
    # bimVer (+0x0C) = 0x03, imgType (+0x12) = 0x07.
    ${bim}=    Execute Command    sysbus ReadByte 0x0000000C
    Should Be Equal As Integers    ${bim}    0x03
    ${typ}=    Execute Command    sysbus ReadByte 0x00000012
    Should Be Equal As Integers    ${typ}    0x07
    # prgEntry (+0x1C) = 0x00000090 — the program image (vector table) start.
    ${prg}=    Execute Command    sysbus ReadDoubleWord 0x0000001C
    Should Match Regexp    ${prg}    (?i)0x0*90
    # hdrLen (+0x28) = 0x002C core-header length.
    ${hl}=    Execute Command    sysbus ReadWord 0x00000028
    Should Match Regexp    ${hl}    (?i)0x0*2C
    # softVer (+0x20) = 00 01 04 01 -> version 1.4.01.
    ${sv}=    Execute Command    sysbus ReadDoubleWord 0x00000020
    Should Match Regexp    ${sv}    (?i)0x0*1040100

# --- Timekeeper epoch store/load (direct leaf calls) -------------------------
# The timekeeper keeps a wall-clock epoch + a sysclock anchor in a small RAM state
# block (g_timekeeper_state points at it). submit_epoch latches an epoch into the
# block; read_be reconstructs it. The sysclock anchor is hardware, so sysclock_snapshot
# is stubbed to return (leaving the snapshot zero), making the store/load deterministic.

Timekeeper Submit Latches The Epoch Into State
    [Documentation]    timekeeper_submit_epoch(E) builds a request {0, E, 0} and apply
    ...                writes E to state[0] (epoch) and 0 to state[1]. Point g_timekeeper_state
    ...                at a scratch block, stub the sysclock snapshot, submit a known epoch,
    ...                and confirm the block holds it.
    Create Leaf Machine
    Hook Return    sysclock_snapshot
    ${stp}=    Resolve Symbol    g_timekeeper_state
    Execute Command    sysbus WriteDoubleWord ${stp} ${TK_STATE}
    Execute Command    cpu SetRegister 0 0xDEADBEEF
    Call Leaf Function    timekeeper_submit_epoch
    ${s0}=    Execute Command    sysbus ReadDoubleWord ${TK_STATE}
    ${s0}=    Strip String    ${s0}
    Should Be Equal As Integers    ${s0}    0xDEADBEEF
    ${s1addr}=    Evaluate    ${TK_STATE} + 4
    ${s1}=    Execute Command    sysbus ReadDoubleWord ${s1addr}
    ${s1}=    Strip String    ${s1}
    Should Be Equal As Integers    ${s1}    0

Timekeeper Read Reconstructs The Stored Epoch
    [Documentation]    timekeeper_read_be rebuilds the epoch from the state block: with a
    ...                zero sysclock anchor it returns state[0] unchanged. Seed the block with
    ...                a known epoch (rest zero), stub the snapshot, and confirm read_be
    ...                returns it (the low word of the big-endian u64, in R0).
    Create Leaf Machine
    Hook Return    sysclock_snapshot
    ${stp}=    Resolve Symbol    g_timekeeper_state
    Execute Command    sysbus WriteDoubleWord ${stp} ${TK_STATE}
    Execute Command    sysbus WriteDoubleWord ${TK_STATE} 0xCAFEF00D
    FOR    ${off}    IN    4    8    12    16
        ${a}=    Evaluate    ${TK_STATE} + ${off}
        Execute Command    sysbus WriteDoubleWord ${a} 0
    END
    ${r}=    Call Leaf Function    timekeeper_read_be
    Should Be Equal As Integers    ${r}    0xCAFEF00D
