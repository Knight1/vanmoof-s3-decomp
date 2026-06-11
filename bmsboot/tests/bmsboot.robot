*** Settings ***
Documentation     Renode smoke tests for the VanMoof bmsboot (STM32L072) loader image.
...               Build the test ELF first:  bash bmsboot/tests/build-test-elf.sh
...               then run:  renode-test bmsboot/tests/bmsboot.robot
...               (the ELF links the real src/ objects against tests/stubs.c, which
...               stands in for the STM32L0 HAL/CMSIS leaves the decomp leaves extern).
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}

*** Variables ***
${ELF}            @${CURDIR}/../build/bmsboot_test.elf
${REPL}           @${CURDIR}/bmsboot.repl
${VTOR_RAM}       0x20000000      # boot_hw_init relocates the vector table here
${SCB_VTOR}       0xE000ED08      # SCB->VTOR register

*** Keywords ***
Create Loader Machine
    [Documentation]    Build the machine and load the bmsboot test ELF. The CPU resets
    ...                straight into the loader's own vector table at flash 0x08000000
    ...                (no VanMoof image header), so LoadELF's entry/SP are correct as-is.
    Execute Command           mach create "bmsboot"
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus LoadELF ${ELF}

Resolve Symbol
    [Documentation]    Look up a symbol's address in the loaded ELF, stripped.
    [Arguments]    ${name}
    ${addr}=    Execute Command    sysbus GetSymbolAddress "${name}"
    ${addr}=    Strip String       ${addr}
    [Return]    ${addr}

*** Test Cases ***
Vector Table Is Well Formed
    [Documentation]    Static check (no execution): vector slot 0 (initial SP) points
    ...                at the top of SRAM and slot 1 (reset PC) into loader flash with
    ...                the Thumb bit set — the CPU will reset cleanly into this image.
    Create Loader Machine
    ${sp}=    Execute Command    sysbus ReadDoubleWord 0x08000000
    ${pc}=    Execute Command    sysbus ReadDoubleWord 0x08000004
    # Initial SP at the top of the 20 KB SRAM (0x20005000).
    Should Match Regexp    ${sp}    (?i)0x0*20005000
    # Reset vector in loader flash (0x0800_xxxx) with the Thumb bit (odd) set.
    Should Match Regexp    ${pc}    (?i)0x0*800[0-9a-f]*[13579bdf]

Reset Chain Reaches Main
    [Documentation]    The startup chain — Reset_Handler (.data copy, .bss zero, libc
    ...                init) then boot_hw_init (vector relocation, clock/CRC/IWDG/GPIO
    ...                bring-up via the HAL stubs) — must hand control to main().
    Create Loader Machine
    Create Log Tester         10
    ${main}=    Resolve Symbol    main
    Execute Command           cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Start Emulation
    Wait For Log Entry        REACHED_MAIN

Boot Relocates The Vector Table Into SRAM
    [Documentation]    Loader-specific: boot_hw_init copies the 0xC0-byte vector table
    ...                to SRAM 0x20000000 and points SCB->VTOR at it. After reaching
    ...                main, VTOR must equal 0x20000000 and the relocated table's first
    ...                two words (SP, reset) must match the flash originals.
    Create Loader Machine
    Create Log Tester         10
    ${main}=    Resolve Symbol    main
    Execute Command           cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Start Emulation
    Wait For Log Entry        REACHED_MAIN
    ${vtor}=    Execute Command    sysbus ReadDoubleWord ${SCB_VTOR}
    Should Match Regexp       ${vtor}    (?i)0x0*20000000
    ${ram_sp}=    Execute Command    sysbus ReadDoubleWord ${VTOR_RAM}
    ${flash_sp}=    Execute Command    sysbus ReadDoubleWord 0x08000000
    Should Be Equal           ${ram_sp}    ${flash_sp}

UART Banner Is Queued For Transmission
    [Documentation]    UART output check. With the download pin (PA10) asserted the
    ...                loader brings up USART1 and main() queues the V007 startup banner
    ...                into the TX ring. Force the pin high (hal_gpio_read -> 1) and
    ...                short-circuit the blocking uart_tx_flush (no ISR services TX on
    ...                the bare platform), then confirm the bytes the loader queued for
    ...                transmission are exactly the "I am VanMoof BL V007 ..." banner.
    Create Loader Machine
    Create Log Tester         10
    ${gpio_read}=    Resolve Symbol    hal_gpio_read
    ${flush}=        Resolve Symbol    uart_tx_flush
    ${banner}=       Resolve Symbol    STR_BANNER_V007
    ${txbuf}=        Resolve Symbol    s_tx_buf
    # PA10 high -> download_pin_check brings USART1 up (s_uart_enabled = 1).
    Execute Command    cpu AddHook ${gpio_read} "self.SetRegisterUlong(0, 1); cpu.PC = cpu.LR"
    # uart_tx_flush would spin forever waiting on the TX-complete ISR — signal that
    # the banner has been queued (the enqueue happens on the line before) and return.
    Execute Command    cpu AddHook ${flush} "self.Log(LogLevel.Error, 'BANNER_QUEUED'); cpu.PC = cpu.LR"
    Start Emulation
    Wait For Log Entry    BANNER_QUEUED
    # The 42-byte banner (incl. leading \\n and trailing \\r) the loader queued must
    # match the flash banner string byte-for-byte.
    ${expected}=    Execute Command    sysbus ReadBytes ${banner} 42
    ${actual}=      Execute Command    sysbus ReadBytes ${txbuf} 42
    Should Be Equal    ${actual}    ${expected}

Normal Boot Leaves The Comms Port Disabled
    [Documentation]    With PA10 low (hal_gpio_read -> 0, the stub default) the loader
    ...                takes comms_disable, never bringing USART1 up — so the banner
    ...                enqueue is a no-op and the TX ring stays empty. Confirms the
    ...                download path is properly gated.
    Create Loader Machine
    Create Log Tester         10
    ${main}=     Resolve Symbol    main
    ${enabled}=  Resolve Symbol    s_uart_enabled
    Execute Command    cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Start Emulation
    Wait For Log Entry    REACHED_MAIN
    ${en}=    Execute Command    sysbus ReadByte ${enabled}
    Should Be Equal As Integers    ${en}    0
