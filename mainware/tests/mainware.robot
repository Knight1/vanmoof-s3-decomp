*** Settings ***
Documentation     Renode smoke tests for the VanMoof mainware (STM32F413VGT6) image.
...               Build first:  make -C mainware renode-test   (or: make -C mainware build/mainware-test.elf)
...               then run:     renode-test mainware/tests/mainware.robot
...               (paths below are relative to the repo root).
...
...               mainware is the bike's central-controller application — a
...               Cortex-M4F super-loop that runs eight serial links, the BLE /
...               Modbus command surface, the power-state machine and the modem
...               driver. The whole application layer is reconstructed in C, but
...               the linked image's main() is still the startup spin-stub (the
...               real main is weak until its ~70-callee closure is rooted), so
...               --gc-sections drops every leaf main() doesn't reach. The suite
...               is built against build/mainware-test.elf, which re-roots the
...               pure leaves it exercises (see the Makefile's TEST_KEEP list) by
...               forcing them undefined at link time. Each leaf test loads the
...               image into a fresh machine, sets up AAPCS registers, points PC
...               at a real compiled routine and runs a brief window, then reads
...               back the result — exercising the firmware's own code against
...               externally-known reference values.
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}

*** Variables ***
${ELF}            @${CURDIR}/../build/mainware-test.elf
${REPL}           @${CURDIR}/mainware.repl
# Application vector table: 512-B envelope at the slot base (0x08020000), then
# the STM32 table at 0x08020200 (SP @ +0x00, reset @ +0x04). SystemInit re-points
# VTOR to the flash base at boot; main() would re-point it to the app table.
${VTOR}           0x08020200
${SP_INIT}        0x20037000      # OEM initial SP (vector slot 0, _estack)
# Leaf-call scratch buffer + stack top. Scratch sits well above the firmware's
# globals (which top out near 0x2000A000) and far below the active stack.
${SCRATCH}        0x20020000
${STACKTOP}       0x20037000

*** Keywords ***
Create Leaf Machine
    [Documentation]    Minimal machine for the direct-call leaf tests: load the
    ...                platform and the test ELF. The exercised leaves are pure
    ...                (no UART / peripheral side effects), so no hooks are needed.
    Execute Command           mach create
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus LoadELF ${ELF}

Create App Machine
    [Documentation]    Build the machine and load the image for the boot/liveness
    ...                tests. Point the vector table at the application header and
    ...                register a reset macro so a soft reset restores it. main()
    ...                is reached out of Reset_Handler (.data copy, .bss/free-RAM
    ...                fill, SystemInit register writes, __libc_init_array stub).
    Execute Command           mach create "mainware"
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus LoadELF ${ELF}
    Execute Command           cpu VectorTableOffset ${VTOR}
    Execute Command           macro reset "cpu VectorTableOffset ${VTOR}"

Resolve Symbol
    [Documentation]    Look up a symbol's address in the loaded ELF.
    [Arguments]    ${name}
    ${addr}=    Execute Command    sysbus GetSymbolAddress "${name}"
    ${addr}=    Strip String       ${addr}
    [Return]    ${addr}

Return Trap
    [Documentation]    Address of Default_Handler (a `b .` self-loop) with the Thumb
    ...                bit set, used as LR so a called function parks the CPU harmlessly
    ...                on return (BX LR) instead of running off into undefined code.
    ${trap}=    Resolve Symbol     Default_Handler
    ${trap}=    Evaluate           ${trap} | 1
    [Return]    ${trap}

Call Function
    [Documentation]    Invoke a Thumb function by symbol name for its side effects.
    ...                Arguments R0..R3 (and any stacked args) must already be set by
    ...                the caller. Sets up a fresh stack, points LR at the Return Trap
    ...                self-loop, then runs a brief window so the call completes and
    ...                parks on the trap.
    [Arguments]    ${symbol}
    ${addr}=    Resolve Symbol    ${symbol}
    ${trap}=    Return Trap
    Execute Command           cpu SetRegister 13 ${STACKTOP}
    Execute Command           cpu SetRegister 14 ${trap}
    Execute Command           cpu PC ${addr}
    Execute Command           emulation RunFor "0.01"

Call Leaf Function
    [Documentation]    Like Call Function but returns R0 (the AAPCS return value)
    ...                as a stripped string, for pure routines under test.
    [Arguments]    ${symbol}
    Call Function    ${symbol}
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

Read Byte
    [Documentation]    Read a single SRAM byte and return it as an integer.
    [Arguments]    ${addr}
    ${v}=    Execute Command    sysbus ReadByte ${addr}
    ${v}=    Strip String       ${v}
    ${v}=    Convert To Integer    ${v}    16
    [Return]    ${v}

Read Word
    [Documentation]    Read a 16-bit SRAM half-word and return it as an integer.
    [Arguments]    ${addr}
    ${v}=    Execute Command    sysbus ReadWord ${addr}
    ${v}=    Strip String       ${v}
    ${v}=    Convert To Integer    ${v}    16
    [Return]    ${v}

Modbus Crc16
    [Documentation]    Compute the Modbus CRC-16 (poly 0xA001, init 0xFFFF) over six
    ...                byte values in pure Python — the reference the firmware's crc16
    ...                must match. The args are substituted as raw Python literals, so
    ...                hex forms (0x20) parse directly. Mirrors crc16_modbus_update's
    ...                8-round reflected reduction.
    [Arguments]    ${b0}    ${b1}    ${b2}    ${b3}    ${b4}    ${b5}
    ${crc}=    Evaluate    functools.reduce(lambda c,b: functools.reduce(lambda x,_:(x>>1)^0xA001 if x&1 else x>>1, range(8), c^b), [${b0}, ${b1}, ${b2}, ${b3}, ${b4}, ${b5}], 0xFFFF)    modules=functools
    [Return]    ${crc}

Bcd To Bin Should Be
    [Documentation]    Fresh-machine helper: call bcd_to_bin(${in}) and assert its
    ...                byte return. A new machine per case (one call window each).
    [Arguments]    ${in}    ${expected}
    Create Leaf Machine
    Execute Command    cpu SetRegister 0 ${in}
    ${r}=    Call Leaf Function    bcd_to_bin
    Should Be Equal As Integers    ${r}    ${expected}

Bin To Bcd Should Be
    [Documentation]    Fresh-machine helper: call bin_to_bcd(${in}) and assert its
    ...                packed-BCD byte return.
    [Arguments]    ${in}    ${expected}
    Create Leaf Machine
    Execute Command    cpu SetRegister 0 ${in}
    ${r}=    Call Leaf Function    bin_to_bcd
    Should Be Equal As Integers    ${r}    ${expected}

Map Clamp Should Be
    [Documentation]    Fresh-machine helper for telemetry_map_clamp(v,a,b,c,d): the
    ...                first four args go in R0..R3, the fifth (d) is passed on the
    ...                stack at the call-entry SP (${STACKTOP}) per AAPCS. Asserts the
    ...                clamped-and-scaled byte return.
    [Arguments]    ${v}    ${a}    ${b}    ${c}    ${d}    ${expected}
    Create Leaf Machine
    Execute Command    cpu SetRegister 0 ${v}
    Execute Command    cpu SetRegister 1 ${a}
    Execute Command    cpu SetRegister 2 ${b}
    Execute Command    cpu SetRegister 3 ${c}
    Execute Command    sysbus WriteDoubleWord ${STACKTOP} ${d}
    ${r}=    Call Leaf Function    telemetry_map_clamp
    Should Be Equal As Integers    ${r}    ${expected}

Init Ringbuf
    [Documentation]    Lay out a ringbuf_t descriptor at ${SCRATCH} with the given
    ...                fields, backing buffer at ${SCRATCH}+0x40. Layout: data ptr
    ...                @+0x00, cap(u16) @+0x04, count(i16) @+0x06, head(u16) @+0x08,
    ...                tail(u16) @+0x0A.
    [Arguments]    ${cap}    ${count}    ${head}    ${tail}
    ${backing}=    Evaluate    ${SCRATCH} + 0x40
    ${w1}=    Evaluate    (${cap} & 0xFFFF) | ((${count} & 0xFFFF) << 16)
    ${w2}=    Evaluate    (${head} & 0xFFFF) | ((${tail} & 0xFFFF) << 16)
    Execute Command    sysbus WriteDoubleWord ${SCRATCH} ${backing}
    Execute Command    sysbus WriteDoubleWord ${{ ${SCRATCH} + 0x04 }} ${w1}
    Execute Command    sysbus WriteDoubleWord ${{ ${SCRATCH} + 0x08 }} ${w2}

*** Test Cases ***
# --- Boot / liveness ---------------------------------------------------------

Vector Table Is Well Formed
    [Documentation]    Static check (no execution): vector slot 0 (initial SP) is the
    ...                OEM stack top 0x20037000 and slot 1 (reset PC) points into
    ...                application flash with the Thumb bit set — the CPU resets
    ...                cleanly into this image.
    Create App Machine
    ${sp}=    Execute Command    sysbus ReadDoubleWord ${VTOR}
    ${pc}=    Execute Command    sysbus ReadDoubleWord ${{ ${VTOR} + 4 }}
    Should Match Regexp    ${sp}    (?i)0x0*20037000
    # Reset vector in application flash (0x0802_xxxx / 0x0804_xxxx ..) with Thumb bit.
    Should Match Regexp    ${pc}    (?i)0x0*80[0-9a-f]*[13579bdf]

Reset Chain Reaches Main
    [Documentation]    The startup chain — Reset_Handler (MSP reload, .data copy,
    ...                .bss zero + free-RAM fill, SystemInit, __libc_init_array) —
    ...                must hand control to main(). SystemInit is a straight
    ...                register-write sequence (no ready-bit spins), so this needs
    ...                no stubbing; it is a hard requirement for a bootable image.
    Create App Machine
    Create Log Tester         10
    ${main}=    Resolve Symbol    main
    Execute Command           cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Start Emulation
    Wait For Log Entry        REACHED_MAIN

# --- CRC-16 Modbus (crc.c) ---------------------------------------------------

CRC-16 Modbus Standard Check Value
    [Documentation]    crc16 over ASCII "123456789" seeded with 0xFFFF must produce
    ...                the canonical Modbus/CRC-16 check value 0x4B37 (poly 0xA001).
    Create Leaf Machine
    Load Scratch Bytes    0x31  0x32  0x33  0x34  0x35  0x36  0x37  0x38  0x39
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 9
    Execute Command       cpu SetRegister 2 0xFFFF
    ${crc}=    Call Leaf Function    crc16
    Should Be Equal As Integers    ${crc}    0x4B37

CRC-16 Of An Empty Buffer Is The Init Value
    [Documentation]    With len 0 the `while (len != 0)` loop body never runs, so the
    ...                seed 0xFFFF passes straight through — guards the length boundary.
    Create Leaf Machine
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 0
    Execute Command       cpu SetRegister 2 0xFFFF
    ${crc}=    Call Leaf Function    crc16
    Should Be Equal As Integers    ${crc}    0xFFFF

CRC-16 Of A Shifter Header Matches The Reference
    [Documentation]    Cross-validate the firmware's crc16 against an independent
    ...                Python Modbus CRC over the same six bytes (the eShifter cmd-0x14
    ...                request header 20 14 00 14 00 00). Both must yield 0x7C77.
    Create Leaf Machine
    Load Scratch Bytes    0x20  0x14  0x00  0x14  0x00  0x00
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 6
    Execute Command       cpu SetRegister 2 0xFFFF
    ${crc}=    Call Leaf Function    crc16
    ${ref}=    Modbus Crc16    0x20  0x14  0x00  0x14  0x00  0x00
    Should Be Equal As Integers    ${crc}    ${ref}
    Should Be Equal As Integers    ${crc}    0x7C77

# --- Packed-BCD converters (util.c) ------------------------------------------

BCD To Binary Decodes Packed Digits
    [Documentation]    bcd_to_bin(bcd) = (bcd & 0x0F) + (bcd >> 4) * 10. Each case
    ...                runs in its own machine.
    Bcd To Bin Should Be    0x00    0
    Bcd To Bin Should Be    0x42    42
    Bcd To Bin Should Be    0x99    99

Binary To BCD Encodes Digits
    [Documentation]    bin_to_bcd(bin) packs tens into the high nibble — the inverse
    ...                of bcd_to_bin across the RTC digit range.
    Bin To Bcd Should Be    0     0x00
    Bin To Bcd Should Be    42    0x42
    Bin To Bcd Should Be    99    0x99

# --- Telemetry linear map + clamp (util.c) -----------------------------------

Telemetry Map Clamp Scales The SoC Range
    [Documentation]    telemetry_map_clamp(v, 0, 0x61, 0, 100) is the BLE 0x5541
    ...                state-of-charge scale: it clamps v to [0, 0x61] then linearly
    ...                maps it onto [0, 100] (unsigned divide). Endpoints map to the
    ...                output bounds, the midpoint truncates (48*100/97 = 49), and an
    ...                over-range input saturates at 100.
    Map Clamp Should Be    0     0    0x61    0    100    0
    Map Clamp Should Be    0x61  0    0x61    0    100    100
    Map Clamp Should Be    0x30  0    0x61    0    100    49
    Map Clamp Should Be    0xFF  0    0x61    0    100    100

# --- Generic byte ring buffer (util.c) ---------------------------------------

Ringbuf Push Stores A Byte And Advances Head
    [Documentation]    ringbuf_push_byte into an empty 8-slot ring returns 1, writes
    ...                the byte at data[head], bumps head to 1 and count to 1.
    Create Leaf Machine
    Init Ringbuf    8    0    0    0
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 0xAB
    ${r}=    Call Leaf Function    ringbuf_push_byte
    Should Be Equal As Integers    ${r}    1
    ${b0}=    Read Byte    ${{ ${SCRATCH} + 0x40 }}
    Should Be Equal As Integers    ${b0}    0xAB
    ${count}=    Read Word    ${{ ${SCRATCH} + 0x06 }}
    Should Be Equal As Integers    ${count}    1
    ${head}=    Read Word    ${{ ${SCRATCH} + 0x08 }}
    Should Be Equal As Integers    ${head}    1

Ringbuf Get Pops The Queued Byte
    [Documentation]    ringbuf_get_byte from a ring holding one byte (count 1, tail 0)
    ...                returns 1, copies data[tail] into *out, and drops count to 0 /
    ...                advances tail to 1.
    Create Leaf Machine
    Init Ringbuf    8    1    1    0
    Execute Command    sysbus WriteByte ${{ ${SCRATCH} + 0x40 }} 0xCD
    ${out}=    Evaluate    ${SCRATCH} + 0x80
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 ${out}
    ${r}=    Call Leaf Function    ringbuf_get_byte
    Should Be Equal As Integers    ${r}    1
    ${popped}=    Read Byte    ${out}
    Should Be Equal As Integers    ${popped}    0xCD
    ${count}=    Read Word    ${{ ${SCRATCH} + 0x06 }}
    Should Be Equal As Integers    ${count}    0
    ${tail}=    Read Word    ${{ ${SCRATCH} + 0x0A }}
    Should Be Equal As Integers    ${tail}    1

Ringbuf Free Space Is Cap Minus Count
    [Documentation]    ringbuf_free_space returns cap - count (8 - 3 = 5) for a
    ...                partially filled ring.
    Create Leaf Machine
    Init Ringbuf    8    3    3    0
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    ${r}=    Call Leaf Function    ringbuf_free_space
    Should Be Equal As Integers    ${r}    5

Ringbuf Get On Empty Returns Zero
    [Documentation]    ringbuf_get_byte on an empty ring (count 0) returns 0 without
    ...                touching *out or the indices.
    Create Leaf Machine
    Init Ringbuf    8    0    0    0
    ${out}=    Evaluate    ${SCRATCH} + 0x80
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 ${out}
    ${r}=    Call Leaf Function    ringbuf_get_byte
    Should Be Equal As Integers    ${r}    0

Ringbuf Push On Full Returns Zero
    [Documentation]    ringbuf_push_byte on a full ring (count == cap) returns 0 —
    ...                the FIFO-full guard, so a byte is never dropped over live data.
    Create Leaf Machine
    Init Ringbuf    8    8    0    0
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 0x55
    ${r}=    Call Leaf Function    ringbuf_push_byte
    Should Be Equal As Integers    ${r}    0
