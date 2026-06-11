*** Settings ***
Documentation     Renode smoke tests for the VanMoof powerbankboot (STM32F091) loader.
...               Build the test image first:  make -C powerbankboot test
...               then run:  renode-test powerbankboot/tests/powerbankboot.robot
...               `make test` links the real src/ objects against tests/stubs.c (which
...               stands in for the STM32F0 HAL + X-CUBE-STL leaves the decomp leaves
...               extern) into build/powerbankboot_test.elf.
...
...               powerbankboot is the F0/Cortex-M0 sibling of bmsboot (L0/M0+): same
...               A/B (dual-bank) boot orchestrator + resident serial-download server,
...               but it persists its boot flag in the RTC backup registers (not a data
...               EEPROM) and, being Cortex-M0, runs its vectors at flash base (no VTOR).
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}

*** Variables ***
${ELF}            @${CURDIR}/../build/powerbankboot_test.elf
${REPL}           @${CURDIR}/powerbankboot.repl
# RTC backup registers (RTC_BASE 0x40002800): the persisted upgrade flag lives in
# BKP0R, its redundancy complement in BKP1R.
${RTC_BKP0R}      0x40002850
${RTC_BKP1R}      0x40002854
# Application bank base + image-header size: the real Cortex-M0 vector table sits
# at AP_BASE + 0x28 (initial SP at +0x28, reset entry at +0x2C).
${AP_BASE}        0x08008000
${AP_SP}          0x08008028
${AP_RESET}       0x0800802C

*** Keywords ***
Create Loader Machine
    [Documentation]    Build the machine and load the powerbankboot test ELF. The CPU
    ...                resets straight into the loader's own vector table at flash
    ...                0x08000000 (no VanMoof image header), so LoadELF's entry/SP are
    ...                correct as-is.
    Execute Command           mach create "powerbankboot"
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

Reach Resident Loop
    [Documentation]    Common setup for the resident-download-server tests. The TX is
    ...                an ISR-driven ring; on the bare platform no ISR services it, so
    ...                uart_tx_flush() (called once before the loop) would busy-wait on
    ...                the TX-complete state forever — short-circuit it (cpu.PC = cpu.LR).
    ...                Then log a marker at uart_rx_drain, the first call inside the
    ...                for(;;) server loop, so the suite can wait until the loader is
    ...                resident. The banner has already been enqueued by then.
    [Arguments]    ${marker}
    Create Log Tester    10
    Hook Return          uart_tx_flush
    ${drain}=    Resolve Symbol    uart_rx_drain
    Execute Command      cpu AddHook ${drain} "self.Log(LogLevel.Error, '${marker}')"

*** Test Cases ***
Vector Table Is Well Formed
    [Documentation]    Static check (no execution): vector slot 0 (initial SP) points
    ...                at the top of SRAM and slot 1 (reset PC) into loader flash with
    ...                the Thumb bit set — the CPU resets cleanly into this image.
    Create Loader Machine
    ${sp}=    Execute Command    sysbus ReadDoubleWord 0x08000000
    ${pc}=    Execute Command    sysbus ReadDoubleWord 0x08000004
    # Initial SP at the top of the 32 KB SRAM (0x20008000).
    Should Match Regexp    ${sp}    (?i)0x0*20008000
    # Reset vector in loader flash (0x0800_xxxx) with the Thumb bit (odd) set.
    Should Match Regexp    ${pc}    (?i)0x0*800[0-9a-f]*[13579bdf]

Reset Chain Reaches Main
    [Documentation]    The startup chain — Reset_Handler (set SP, init_data_bss:
    ...                .data copy + .bss zero) — must hand control to main(). main()
    ...                runs the STL log bring-up then calls boot_main; reaching it
    ...                needs no peripheral stubbing beyond the HAL link stubs.
    Create Loader Machine
    Create Log Tester         10
    ${main}=    Resolve Symbol    main
    Execute Command           cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Start Emulation
    Wait For Log Entry        REACHED_MAIN

Boot Reaches The Resident Download Server
    [Documentation]    Deeper boot: with both application banks blank (flash reads 0,
    ...                so image_verify fails the magic check), boot_main falls through
    ...                to case 4 — the resident serial-download server. Confirm the
    ...                loader reaches its for(;;) service loop (uart_rx_drain). If this
    ...                stalls, a bring-up step in boot_hw_init is busy-waiting.
    Create Loader Machine
    Reach Resident Loop    IN_LOOP
    Start Emulation
    Wait For Log Entry     IN_LOOP

UART Banner Is Queued For Transmission
    [Documentation]    UART output check. On the resident path the loader enqueues its
    ...                "I am VM-BATT BL" startup banner into the USART2 TX ring before
    ...                entering the server loop. Run to the loop, then confirm the bytes
    ...                queued in s_tx_buf are exactly the flash banner STR_BANNER,
    ...                byte-for-byte — the bytes the loader would clock onto the wire.
    Create Loader Machine
    Reach Resident Loop    BANNER_QUEUED
    ${banner}=    Resolve Symbol    STR_BANNER
    ${txbuf}=     Resolve Symbol    s_tx_buf
    Start Emulation
    Wait For Log Entry    BANNER_QUEUED
    # The 17-byte banner ("\\nI am VM-BATT BL\\r") the loader queued must match the
    # flash banner string byte-for-byte.
    ${expected}=    Execute Command    sysbus ReadBytes ${banner} 17
    ${actual}=      Execute Command    sysbus ReadBytes ${txbuf} 17
    Should Be Equal    ${actual}    ${expected}

Corrupt Upgrade-Flag Pair Is Reset In RTC Backup
    [Documentation]    boot_read_persistent_flags loads the upgrade flag (BKP0R) and
    ...                its complement (BKP1R) and, when they aren't a valid value+
    ...                complement pair, clears the flag to a safe 0 via store_boot_flag
    ...                — which rewrites BKP0R = 0 and BKP1R = ~0. Seed a deliberately
    ...                inconsistent pair (0xAB / 0x123) and confirm the loader detects
    ...                the corruption and rewrites both backup registers.
    Create Loader Machine
    Execute Command    sysbus WriteDoubleWord ${RTC_BKP0R} 0x000000AB
    Execute Command    sysbus WriteDoubleWord ${RTC_BKP1R} 0x00000123
    Reach Resident Loop    IN_LOOP
    Start Emulation
    Wait For Log Entry     IN_LOOP
    ${bkp0}=    Execute Command    sysbus ReadDoubleWord ${RTC_BKP0R}
    ${bkp0}=    Strip String       ${bkp0}
    Should Be Equal As Integers    ${bkp0}    0
    ${bkp1}=    Execute Command    sysbus ReadDoubleWord ${RTC_BKP1R}
    ${bkp1}=    Strip String       ${bkp1}
    Should Be Equal As Integers    ${bkp1}    0xFFFFFFFF

Valid AP Image Boots Via Goto Application
    [Documentation]    The core bootloader behaviour: when the application bank holds a
    ...                valid image, the loader hands control to it. Force image_verify
    ...                to report the bank good (return IMG_OK = 0), seed the application
    ...                reset vector at AP_BASE+0x2C to point at the loader's harmless
    ...                Default_Handler self-loop (and a plausible SP at +0x28), then
    ...                confirm boot_main takes goto_application and the CPU jumps to the
    ...                application's reset entry — i.e. it parks at that vector.
    Create Loader Machine
    ${trap}=    Resolve Symbol    Default_Handler
    ${trapt}=   Evaluate    ${trap} | 1
    Execute Command    sysbus WriteDoubleWord ${AP_SP} 0x20008000
    Execute Command    sysbus WriteDoubleWord ${AP_RESET} ${trapt}
    # Force both banks "valid" so boot_main takes the AP-good branch (case 2) and jumps.
    ${iv}=    Resolve Symbol    image_verify
    Execute Command    cpu AddHook ${iv} "self.SetRegisterUlong(0, 0); cpu.PC = cpu.LR"
    Execute Command    emulation RunFor "0.02"
    ${pc}=    Execute Command    cpu PC
    ${pc}=    Strip String    ${pc}
    Should Be Equal As Integers    ${pc}    ${trap}
