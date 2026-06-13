*** Settings ***
Documentation     Renode smoke tests for the VanMoof bleboot image — the TI BIM
...               (Boot Image Manager) on the CC2642R1F (ARM Cortex-M4F).
...               Build first:  make -C bleboot all
...               then run:     renode-test bleboot/tests/bleboot.robot
...               (paths below are relative to the repo root).
...
...               The BIM links at flash 0x00056000 (its own vector table; the CPU
...               resets straight into it). Its CRC-32 over an OAD image is the
...               integrity gate before a launch, so the CRC routine is the main
...               functional surface exercised here.
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}

*** Variables ***
${ELF}            @${CURDIR}/../build/bleboot.elf
${REPL}           @${CURDIR}/bleboot.repl
${VTOR}           0x00056000      # BIM vector table base (SP @ +0, reset @ +4)
# Scratch + stack for the direct-call (leaf) tests. The BIM's own CRC scratch is at
# 0x20000300; keep our data well clear of it, high in the 80 KB SRAM.
${SCRATCH}        0x20007000
${STACKTOP}       0x20014000

*** Keywords ***
Create Boot Machine
    [Documentation]    Build the machine, load the BIM image and point the vector
    ...                table at 0x00056000. Register a reset macro so a soft reset
    ...                restores the offset.
    Execute Command           mach create "bleboot"
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus LoadELF ${ELF}
    Execute Command           cpu VectorTableOffset ${VTOR}
    Execute Command           macro reset "cpu VectorTableOffset ${VTOR}"

Create Leaf Machine
    [Documentation]    Minimal machine for direct-call (leaf) tests: load the platform
    ...                and ELF, nothing else. The CRC routine touches no trim/flash
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
    [Documentation]    Address of Default_Handler (a `b .` self-loop) with the Thumb
    ...                bit set, used as LR so a called function parks the CPU harmlessly
    ...                on return (BX LR) instead of running off into undefined code.
    ${trap}=    Resolve Symbol     Default_Handler
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

CRC32 Of Scratch Should Be
    [Documentation]    bim_crc32_buffer(addr=${SCRATCH}, len, use_spi=0) reads ${len}
    ...                bytes from the scratch buffer (via bim_memcpy_safe, no SPI) and
    ...                returns the standard CRC32-IEEE (poly 0xEDB88320, init/xorout
    ...                0xFFFFFFFF). Assert it equals ${expected}.
    [Arguments]    ${len}    ${expected}
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 ${len}
    Execute Command    cpu SetRegister 2 0
    ${crc}=    Call Leaf Function    bim_crc32_buffer
    Should Be Equal As Integers    ${crc}    ${expected}

*** Test Cases ***
Vector Table Is Well Formed
    [Documentation]    Static check (no execution): vector slot 0 (initial SP) is the
    ...                top of the 80 KB SRAM and slot 1 (reset PC) points into BIM flash
    ...                with the Thumb bit set — the CPU resets cleanly into this image.
    Create Boot Machine
    ${sp}=    Execute Command    sysbus ReadDoubleWord 0x00056000
    ${pc}=    Execute Command    sysbus ReadDoubleWord 0x00056004
    # Initial SP at the top of SRAM (0x20014000).
    Should Match Regexp    ${sp}    (?i)0x0*20014000
    # Reset vector in BIM flash (0x0005_xxxx) with the Thumb bit (odd) set.
    Should Match Regexp    ${pc}    (?i)0x0*5[0-9a-f]*[13579bdf]

Reset Chain Reaches Main
    [Documentation]    The startup chain — Reset_Handler (silicon trim, then
    ...                ResetISR_body: MSP reset, FPU enable, C-runtime init) — must hand
    ...                control to main(). SetupTrimDevice drives CC2642 trim MMIO that
    ...                isn't modelled, so it is skipped; everything else runs unchanged.
    Create Boot Machine
    Create Log Tester         10
    Hook Return               SetupTrimDevice
    ${main}=    Resolve Symbol    main
    Execute Command           cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Start Emulation
    Wait For Log Entry        REACHED_MAIN

# --- CRC-32 integrity gate (direct leaf calls) -------------------------------
# bim_crc32_buffer is the BIM's flat-range CRC32-IEEE — the integrity check it
# runs over a candidate image before launching it. With use_spi=0 it pulls bytes
# from a RAM source via bim_memcpy_safe (no SPI hardware), so it is a clean leaf:
# seed the scratch buffer, call, and check R0 against external reference values.

CRC32 Standard Check Value
    [Documentation]    bim_crc32_buffer over ASCII "123456789" must equal 0xCBF43926 —
    ...                the canonical CRC32-IEEE check value. Validates the BIM's image
    ...                integrity CRC against an external reference, not against itself.
    Create Leaf Machine
    Load Scratch Bytes    0x31  0x32  0x33  0x34  0x35  0x36  0x37  0x38  0x39
    CRC32 Of Scratch Should Be    9    0xCBF43926

CRC32 Of A Single Zero Byte
    [Documentation]    CRC32-IEEE of one 0x00 byte is 0xD202EF8D — a second external
    ...                reference value that exercises a different length/content than
    ...                the check string.
    Create Leaf Machine
    Load Scratch Bytes    0x00
    CRC32 Of Scratch Should Be    1    0xD202EF8D

CRC32 Of A Zero-Length Buffer Is Zero
    [Documentation]    bim_crc32_buffer guards len 0 (and the all-ones sentinel) and
    ...                returns 0 without touching the buffer — the documented bail value.
    Create Leaf Machine
    CRC32 Of Scratch Should Be    0    0
