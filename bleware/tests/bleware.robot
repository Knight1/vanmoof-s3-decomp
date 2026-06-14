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

Write Bytes At
    [Documentation]    Write a list of byte literals starting at ${base}, one byte per
    ...                ascending address.
    [Arguments]    ${base}    @{bytes}
    ${i}=    Set Variable    ${0}
    FOR    ${b}    IN    @{bytes}
        ${addr}=    Evaluate    ${base} + ${i}
        Execute Command    sysbus WriteByte ${addr} ${b}
        ${i}=    Evaluate    ${i} + 1
    END

Load Scratch Bytes
    [Documentation]    Write a list of byte literals into the SRAM scratch buffer
    ...                (${SCRATCH}), one byte per ascending address.
    [Arguments]    @{bytes}
    Write Bytes At    ${SCRATCH}    @{bytes}

Stub Connection Semaphores
    [Documentation]    The per-connection accessors guard every access with TI-RTOS
    ...                Semaphore_pend/post (weak aliases that tail-branch to ROM thunks
    ...                absent from this leaf image). Hook both to return immediately so the
    ...                guarded body runs without the kernel.
    Hook Return    ti_semaphore_pend
    Hook Return    ti_semaphore_post

Hook Returns Pointer
    [Documentation]    Hook a function (by symbol) to return a fixed pointer value in R0 —
    ...                set R0 then cpu.PC = cpu.LR, so a record-lookup helper hands back a
    ...                caller-supplied scratch buffer instead of touching real state.
    [Arguments]    ${symbol}    ${value}
    ${a}=    Resolve Symbol    ${symbol}
    Execute Command    cpu AddHook ${a} "cpu.SetRegisterUlong(0, ${value}); cpu.PC = cpu.LR"

Read Byte
    [Documentation]    Read one byte from ${addr}, stripped.
    [Arguments]    ${addr}
    ${v}=    Execute Command    sysbus ReadByte ${addr}
    ${v}=    Strip String       ${v}
    [Return]    ${v}

Read DoubleWord
    [Documentation]    Read a 32-bit word from ${addr}, stripped.
    [Arguments]    ${addr}
    ${v}=    Execute Command    sysbus ReadDoubleWord ${addr}
    ${v}=    Strip String       ${v}
    [Return]    ${v}

Connection Entry Address
    [Documentation]    Resolve the connection-state table and return the byte address of
    ...                entry ${idx} (stride 0x7C). g_ble_connection_table is a pointer
    ...                variable holding the storage base; write it explicitly so the test
    ...                doesn't depend on .data being loaded, then index into the storage.
    [Arguments]    ${idx}
    ${ptr}=     Resolve Symbol    g_ble_connection_table
    ${stor}=    Resolve Symbol    g_ble_connection_table_storage
    Execute Command    sysbus WriteDoubleWord ${ptr} ${stor}
    ${e}=    Evaluate    ${stor} + ${idx} * 0x7C
    [Return]    ${e}

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

# --- Per-connection state accessors (direct leaf calls) ----------------------
# The BLE stack keeps a 0x7C-byte record per connection (table g_ble_connection_
# table, stride 0x7C). The accessors all share one shape: Semaphore_pend, check the
# caller's conn matches the record's stored conn_handle (+0x48), read/write a field,
# Semaphore_post. That conn_handle gate is the security-relevant part — a stale or
# spoofed handle must be rejected. The semaphores are hooked to return (see
# Stub Connection Semaphores); each test seeds a record then calls one accessor.

Connection Indicate-Seq Peek Returns The Stored Sequence
    [Documentation]    indicate_seq_peek(conn, *out) copies the record's indicate
    ...                sequence number (+0x00) to *out and returns 0 when conn matches the
    ...                stored conn_handle (+0x48). Seed entry 0 with handle 0 / seq 0x1234
    ...                and confirm the read; then prove a mismatched handle is rejected (-1).
    # (A) handle matches -> rc 0, sequence copied out
    Create Leaf Machine
    Stub Connection Semaphores
    ${e}=    Connection Entry Address    0
    ${ch}=    Evaluate    ${e} + 0x48
    Execute Command    sysbus WriteWord ${ch} 0
    Execute Command    sysbus WriteWord ${e} 0x1234
    Execute Command    cpu SetRegister 0 0
    Execute Command    cpu SetRegister 1 ${SCRATCH}
    ${rc}=    Call Leaf Function    indicate_seq_peek
    Should Be Equal As Integers    ${rc}    0
    ${seq}=    Execute Command    sysbus ReadWord ${SCRATCH}
    ${seq}=    Strip String    ${seq}
    Should Be Equal As Integers    ${seq}    0x1234
    # (B) stored handle 7, caller asks for conn 0 -> rejected with -1
    Create Leaf Machine
    Stub Connection Semaphores
    ${e2}=    Connection Entry Address    0
    ${ch2}=    Evaluate    ${e2} + 0x48
    Execute Command    sysbus WriteWord ${ch2} 7
    Execute Command    cpu SetRegister 0 0
    Execute Command    cpu SetRegister 1 ${SCRATCH}
    ${rc2}=    Call Leaf Function    indicate_seq_peek
    Should Be Equal As Integers    ${rc2}    0xFFFFFFFF

Connection Is-Active Reflects The Handle Match
    [Documentation]    ble_connection_is_active(conn) returns 1 when the record's
    ...                conn_handle equals conn, else 0. Confirm both: a live handle (0) and
    ...                an unallocated slot (0xFFFF).
    # live
    Create Leaf Machine
    Stub Connection Semaphores
    ${e}=    Connection Entry Address    0
    ${ch}=    Evaluate    ${e} + 0x48
    Execute Command    sysbus WriteWord ${ch} 0
    Execute Command    cpu SetRegister 0 0
    ${a}=    Call Leaf Function    ble_connection_is_active
    Should Be Equal As Integers    ${a}    1
    # unallocated (handle 0xFFFF) -> 0
    Create Leaf Machine
    Stub Connection Semaphores
    ${e2}=    Connection Entry Address    0
    ${ch2}=    Evaluate    ${e2} + 0x48
    Execute Command    sysbus WriteWord ${ch2} 0xFFFF
    Execute Command    cpu SetRegister 0 0
    ${b}=    Call Leaf Function    ble_connection_is_active
    Should Be Equal As Integers    ${b}    0

ATT MTU Clamp Reads The Negotiated MTU
    [Documentation]    att_mtu_clamp(conn, *len) overwrites *len with the record's
    ...                negotiated ATT MTU (+0x5C) and returns 0 on a handle match. Seed the
    ...                MTU to 0x00F4 (244), pre-load *len with 0xFFFF, and confirm it is
    ...                clamped down to the stored MTU.
    Create Leaf Machine
    Stub Connection Semaphores
    ${e}=    Connection Entry Address    0
    ${ch}=    Evaluate    ${e} + 0x48
    Execute Command    sysbus WriteWord ${ch} 0
    ${mtu}=    Evaluate    ${e} + 0x5C
    Execute Command    sysbus WriteWord ${mtu} 0x00F4
    Execute Command    sysbus WriteWord ${SCRATCH} 0xFFFF
    Execute Command    cpu SetRegister 0 0
    Execute Command    cpu SetRegister 1 ${SCRATCH}
    ${rc}=    Call Leaf Function    att_mtu_clamp
    Should Be Equal As Integers    ${rc}    0
    ${len}=    Execute Command    sysbus ReadWord ${SCRATCH}
    ${len}=    Strip String    ${len}
    Should Be Equal As Integers    ${len}    0x00F4

Connection State Byte Reads Record Offset 0x65
    [Documentation]    ble_conn_state_byte(conn, *out) copies the per-connection state
    ...                byte at record +0x65 to *out, returning 0 on a handle match. Seed it
    ...                with 0x5A and confirm the read.
    Create Leaf Machine
    Stub Connection Semaphores
    ${e}=    Connection Entry Address    0
    ${ch}=    Evaluate    ${e} + 0x48
    Execute Command    sysbus WriteWord ${ch} 0
    ${sb}=    Evaluate    ${e} + 0x65
    Execute Command    sysbus WriteByte ${sb} 0x5A
    Execute Command    cpu SetRegister 0 0
    Execute Command    cpu SetRegister 1 ${SCRATCH}
    ${rc}=    Call Leaf Function    ble_conn_state_byte
    Should Be Equal As Integers    ${rc}    0
    ${v}=    Execute Command    sysbus ReadByte ${SCRATCH}
    ${v}=    Strip String    ${v}
    Should Be Equal As Integers    ${v}    0x5A

Session Key Pointer Round-Trips Through The Connection Entry
    [Documentation]    ble_connection_set_session_key(conn, key) stores the key pointer
    ...                at record +0x50; ble_connection_get_session_key(conn) returns it. Set
    ...                a known pointer and confirm the record holds it, then seed a record
    ...                and confirm get returns the stored pointer — both gated on the handle.
    # set: writes the pointer into the record
    Create Leaf Machine
    Stub Connection Semaphores
    ${e}=    Connection Entry Address    0
    ${ch}=    Evaluate    ${e} + 0x48
    Execute Command    sysbus WriteWord ${ch} 0
    Execute Command    cpu SetRegister 0 0
    Execute Command    cpu SetRegister 1 0xCAFE1000
    ${rc}=    Call Leaf Function    ble_connection_set_session_key
    Should Be Equal As Integers    ${rc}    0
    ${kp}=    Evaluate    ${e} + 0x50
    ${stored}=    Execute Command    sysbus ReadDoubleWord ${kp}
    ${stored}=    Strip String    ${stored}
    Should Be Equal As Integers    ${stored}    0xCAFE1000
    # get: returns the stored pointer in R0
    Create Leaf Machine
    Stub Connection Semaphores
    ${e2}=    Connection Entry Address    0
    ${ch2}=    Evaluate    ${e2} + 0x48
    Execute Command    sysbus WriteWord ${ch2} 0
    ${kp2}=    Evaluate    ${e2} + 0x50
    Execute Command    sysbus WriteDoubleWord ${kp2} 0xBEEF2000
    Execute Command    cpu SetRegister 0 0
    ${key}=    Call Leaf Function    ble_connection_get_session_key
    Should Be Equal As Integers    ${key}    0xBEEF2000

# --- C-runtime primitives (direct leaf calls) --------------------------------
# memcpy / memset / memcmp / monitor_strtol are the decompiled equivalents of the
# TI runtime routines bleware links against. They are wholly self-contained
# (no globals, no kernel) so they make clean, externally-checkable leaf tests.

Memcpy Copies The Buffer Including The Unaligned Tail
    [Documentation]    memcpy(dst, src, n) word-copies the bulk then bytes the tail and
    ...                returns dst. Copy a 21-byte ascending pattern (16-byte chunk + word +
    ...                byte tail) between word-aligned buffers and confirm every byte lands
    ...                and R0 == dst.
    Create Leaf Machine
    # ascending pattern 0x40..0x54 at SCRATCH.
    ${n}=    Set Variable    ${21}
    FOR    ${i}    IN RANGE    ${n}
        ${a}=    Evaluate    ${SCRATCH} + ${i}
        ${val}=    Evaluate    0x40 + ${i}
        Execute Command    sysbus WriteByte ${a} ${val}
    END
    ${dst}=    Evaluate    ${SCRATCH} + 0x40
    Execute Command    cpu SetRegister 0 ${dst}
    Execute Command    cpu SetRegister 1 ${SCRATCH}
    Execute Command    cpu SetRegister 2 ${n}
    ${ret}=    Call Leaf Function    memcpy
    Should Be Equal As Integers    ${ret}    ${dst}
    FOR    ${i}    IN RANGE    ${n}
        ${a}=    Evaluate    ${dst} + ${i}
        ${got}=    Read Byte    ${a}
        ${exp}=    Evaluate    0x40 + ${i}
        Should Be Equal As Integers    ${got}    ${exp}
    END

Memset Fills Exactly N Bytes And Stops
    [Documentation]    memset(dst, 0xAB, 10) fills ten bytes and leaves the eleventh
    ...                untouched — guards the length boundary across the word-fill + tail
    ...                logic. Pre-mark the sentinel byte 0x55.
    Create Leaf Machine
    ${sent}=    Evaluate    ${SCRATCH} + 10
    Execute Command    sysbus WriteByte ${sent} 0x55
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 0xAB
    Execute Command    cpu SetRegister 2 10
    ${ret}=    Call Leaf Function    memset
    Should Be Equal As Integers    ${ret}    ${SCRATCH}
    FOR    ${i}    IN RANGE    10
        ${a}=    Evaluate    ${SCRATCH} + ${i}
        ${got}=    Read Byte    ${a}
        Should Be Equal As Integers    ${got}    0xAB
    END
    ${tail}=    Read Byte    ${sent}
    Should Be Equal As Integers    ${tail}    0x55

Memcmp Orders By The First Differing Byte
    [Documentation]    memcmp returns 0 on equal buffers and the signed difference of the
    ...                first differing byte otherwise. Check equal, greater (+1), and less
    ...                (-1) against "ABCD".
    # equal
    Create Leaf Machine
    Load Scratch Bytes    0x41  0x42  0x43  0x44              # "ABCD" at SCRATCH
    ${b}=    Evaluate    ${SCRATCH} + 0x10
    FOR    ${i}    IN RANGE    4
        ${sa}=    Evaluate    ${SCRATCH} + ${i}
        ${da}=    Evaluate    ${b} + ${i}
        ${v}=    Read Byte    ${sa}
        Execute Command    sysbus WriteByte ${da} ${v}
    END
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 ${b}
    Execute Command    cpu SetRegister 2 4
    ${eq}=    Call Leaf Function    memcmp
    Should Be Equal As Integers    ${eq}    0
    # greater: a="ABCE" vs b="ABCD" -> +1
    Create Leaf Machine
    Load Scratch Bytes    0x41  0x42  0x43  0x45
    ${b2}=    Evaluate    ${SCRATCH} + 0x10
    Write Bytes At    ${b2}    0x41  0x42  0x43  0x44
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 ${b2}
    Execute Command    cpu SetRegister 2 4
    ${gt}=    Call Leaf Function    memcmp
    Should Be Equal As Integers    ${gt}    1
    # less: a="ABCC" vs b="ABCD" -> -1
    Create Leaf Machine
    Load Scratch Bytes    0x41  0x42  0x43  0x43
    ${b3}=    Evaluate    ${SCRATCH} + 0x10
    Write Bytes At    ${b3}    0x41  0x42  0x43  0x44
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 ${b3}
    Execute Command    cpu SetRegister 2 4
    ${lt}=    Call Leaf Function    memcmp
    Should Be Equal As Integers    ${lt}    0xFFFFFFFF

Strtol Parses Decimal Hex Negative And Saturates On Overflow
    [Documentation]    monitor_strtol mirrors the C strtol: base-10 digits, '0x' auto-hex
    ...                when base is 0, a leading '-' negates, and a value past 0x7FFFFFFF
    ...                saturates to LONG_MAX. endptr is passed NULL (R1=0). NOTE: hex inputs
    ...                containing the letters A-F/a-f are mis-parsed by the reconstruction's
    ...                s_ctype table (see docs/strtol-hex-letter-bug.md), so the auto-hex case
    ...                here uses numeric-only "0x10" to exercise base detection on a known-good
    ...                path.
    # "12345" base 10 -> 12345
    Create Leaf Machine
    Load Scratch Bytes    0x31  0x32  0x33  0x34  0x35  0x00     # "12345"
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 0
    Execute Command    cpu SetRegister 2 10
    ${d}=    Call Leaf Function    monitor_strtol
    Should Be Equal As Integers    ${d}    12345
    # "0x10" base 0 -> 16 (auto-detect hex; numeric digits only — letters are broken,
    # see docs/strtol-hex-letter-bug.md)
    Create Leaf Machine
    Load Scratch Bytes    0x30  0x78  0x31  0x30  0x00           # "0x10"
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 0
    Execute Command    cpu SetRegister 2 0
    ${h}=    Call Leaf Function    monitor_strtol
    Should Be Equal As Integers    ${h}    16
    # "-42" base 10 -> -42 (0xFFFFFFD6)
    Create Leaf Machine
    Load Scratch Bytes    0x2D  0x34  0x32  0x00                 # "-42"
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 0
    Execute Command    cpu SetRegister 2 10
    ${neg}=    Call Leaf Function    monitor_strtol
    Should Be Equal As Integers    ${neg}    0xFFFFFFD6
    # "99999999999" base 10 -> saturates to LONG_MAX 0x7FFFFFFF
    Create Leaf Machine
    Load Scratch Bytes    0x39  0x39  0x39  0x39  0x39  0x39  0x39  0x39  0x39  0x39  0x39  0x00
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 0
    Execute Command    cpu SetRegister 2 10
    ${ovf}=    Call Leaf Function    monitor_strtol
    Should Be Equal As Integers    ${ovf}    0x7FFFFFFF

# --- CCCD write validation (direct leaf call) --------------------------------
# cccd_write_validate gates a Client Characteristic Config Descriptor write: it
# rejects a short value or any bit outside {notify(1), indicate(2)} with 0x80, and
# otherwise stores the new CCCD via the per-connection record (FUN_00025A24 lookup,
# hooked here to hand back a scratch record).

CCCD Write Validate Gates The Descriptor Value
    [Documentation]    Confirm all three branches: a <2-byte value -> 0x80, an unsupported
    ...                bit (0x0004) -> 0x80, and a valid 0x0001 (notify-enable) -> 0 with the
    ...                new value stored into the (hooked) per-connection record.
    # (A) valid notify-enable 0x0001 -> rc 0, stored into record
    Create Leaf Machine
    ${rec}=    Evaluate    ${SCRATCH} + 0x80
    Hook Returns Pointer    FUN_00025A24    ${rec}
    # record's stored CCCD (+2) starts at 0x0000 so the write path triggers.
    ${recv}=    Evaluate    ${rec} + 2
    Execute Command    sysbus WriteWord ${recv} 0x0000
    Execute Command    sysbus WriteByte ${SCRATCH} 0x01         # cccd low byte
    ${vh}=    Evaluate    ${SCRATCH} + 1
    Execute Command    sysbus WriteByte ${vh} 0x00              # cccd high byte
    Execute Command    cpu SetRegister 0 0                      # conn
    Execute Command    cpu SetRegister 1 ${SCRATCH}             # value
    Execute Command    cpu SetRegister 2 2                      # len
    ${ok}=    Call Leaf Function    cccd_write_validate
    Should Be Equal As Integers    ${ok}    0
    ${stored}=    Execute Command    sysbus ReadWord ${recv}
    ${stored}=    Strip String    ${stored}
    Should Be Equal As Integers    ${stored}    0x0001
    # (B) unsupported bit 0x0004 -> 0x80 (rejected before any record write)
    Create Leaf Machine
    ${rec2}=    Evaluate    ${SCRATCH} + 0x80
    Hook Returns Pointer    FUN_00025A24    ${rec2}
    Execute Command    sysbus WriteByte ${SCRATCH} 0x04
    ${vh2}=    Evaluate    ${SCRATCH} + 1
    Execute Command    sysbus WriteByte ${vh2} 0x00
    Execute Command    cpu SetRegister 0 0
    Execute Command    cpu SetRegister 1 ${SCRATCH}
    Execute Command    cpu SetRegister 2 2
    ${bad}=    Call Leaf Function    cccd_write_validate
    Should Be Equal As Integers    ${bad}    0x80
    # (C) short value (len 1) -> 0x80
    Create Leaf Machine
    Execute Command    cpu SetRegister 0 0
    Execute Command    cpu SetRegister 1 ${SCRATCH}
    Execute Command    cpu SetRegister 2 1
    ${short}=    Call Leaf Function    cccd_write_validate
    Should Be Equal As Integers    ${short}    0x80
