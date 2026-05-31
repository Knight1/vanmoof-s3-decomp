*** Settings ***
Documentation     Renode smoke tests for the VanMoof batteryware (STM32L072) image.
...               Run with:  renode-test batteryware/tests/batteryware.robot
...               (paths below are relative to the repo root).
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}

*** Variables ***
${ELF}            @${CURDIR}/build/batteryware.elf
${REPL}           @${CURDIR}/tests/batteryware.repl
${VTOR}           0x08005028

*** Keywords ***
Create Battery Machine
    [Documentation]    Build the machine, force the boot-time status polls ready,
    ...                load the image and point the vector table at the app header.
    Execute Command           mach create "batteryware"
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus Tag <0x40021000, 0x40021003> "RCC_CR"   0x0F03FFFF
    Execute Command           sysbus Tag <0x4002100C, 0x4002100F> "RCC_CFGR" 0x0000000C
    Execute Command           sysbus Tag <0x40022018, 0x4002201B> "FLASH_SR" 0x00000000
    Execute Command           sysbus LoadELF ${ELF}
    Execute Command           cpu VectorTableOffset ${VTOR}

*** Test Cases ***
Vector Table Is Well Formed
    [Documentation]    Static check (no execution): the reset SP points into SRAM
    ...                and the reset PC into application flash with the Thumb bit set.
    Create Battery Machine
    ${sp}=    Execute Command    sysbus ReadDoubleWord 0x08005028
    ${pc}=    Execute Command    sysbus ReadDoubleWord 0x0800502C
    # SP in SRAM 0x2000_xxxx (regex tolerates Renode's optional leading-zero trim)
    Should Match Regexp    ${sp}    (?i)0x0*2000[0-9a-f]+
    # Reset vector in app flash (0x080x_xxxx)
    Should Match Regexp    ${pc}    (?i)0x0*80[0-9a-f]+

Reset Chain Reaches Main
    [Documentation]    The startup chain — .data copy, .bss zero, SystemInit,
    ...                __libc_init_array_lite — must hand control to main(). This
    ...                touches no peripherals, so it is a hard requirement.
    Create Battery Machine
    Create Log Tester         5
    ${main}=    Execute Command    sysbus GetSymbolAddress "main"
    Execute Command           cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Execute Command           emulation RunFor "0.05"
    Wait For Log Entry        REACHED_MAIN

CPU Stays Alive After Boot
    [Documentation]    Liveness: after running a while the CPU has retired many
    ...                instructions and is executing application flash (not parked
    ...                at 0 or trapped below the app base).
    Create Battery Machine
    Execute Command           emulation RunFor "0.5"
    ${insns}=   Execute Command    cpu ExecutedInstructions
    Should Not Be Empty       ${insns}
    ${pc}=      Execute Command    cpu PC
    Should Match Regexp       ${pc}    (?i)0x0*80[0-9a-f]+

Boots Into The Service Super-Loop
    [Documentation]    Deeper boot: with the clock/flash status registers pinned
    ...                ready, the firmware should reach uart_resp_handler (the RX
    ...                drain called from the main super-loop). If this stalls,
    ...                the RCC_CR / RCC_CFGR tags in 'Create Battery Machine' need
    ...                tuning for the clock tree this build selects.
    Create Battery Machine
    Create Log Tester         10
    ${loop}=    Execute Command    sysbus GetSymbolAddress "uart_resp_handler"
    Execute Command           cpu AddHook ${loop} "self.Log(LogLevel.Error, 'IN_SUPERLOOP')"
    Execute Command           emulation RunFor "2"
    Wait For Log Entry        IN_SUPERLOOP

# --- Modbus functional test (scaffold) ---------------------------------------
# Once "Boots Into The Service Super-Loop" passes, the USART1 Modbus link can be
# exercised. The BMS is a Modbus RTU slave at address 0xAA; a Read Holding
# Registers (func 0x03) request for the live snapshot looks like:
#
#     AA 03 00 02 00 2B <crcLo> <crcHi>     # read 0x2B regs from reg 2
#
# Feed the request a byte at a time into the RX path and capture the reply:
#
#     Execute Command   sysbus.usart1 WriteChar 0xAA
#     Execute Command   sysbus.usart1 WriteChar 0x03
#     ...               (remaining request bytes incl. CRC-16/0xA001)
#     # capture TX: hook sysbus.usart1 or attach a tester, then assert the reply
#     # opens with AA 03 <byteCount> ... and ends with a valid CRC-16.
#
# This needs byte-level UART capture (the line-oriented Terminal Tester does not
# fit binary RTU framing); left as a follow-up to wire against your Renode build.
