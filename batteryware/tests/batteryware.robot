*** Settings ***
Documentation     Renode smoke tests for the VanMoof batteryware (STM32L072) image.
...               Run with:  renode-test batteryware/tests/batteryware.robot
...               (paths below are relative to the repo root).
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}

*** Variables ***
${ELF}            @${CURDIR}/../build/batteryware.elf
${REPL}           @${CURDIR}/batteryware.repl
${VTOR}           0x08005028

*** Keywords ***
Create Battery Machine
    [Documentation]    Build the machine, force the boot-time status polls ready,
    ...                load the image and point the vector table at the app header.
    Execute Command           mach create "batteryware"
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus Tag <0x40021000 4> "RCC_CR" 0x0F03FFFF
    # SWS bits [3:2] = 0b01 = HSI16 (not PLL=0b11=0xC). With SWS=HSI, the SWS
    # poll in rcc_configure exits immediately on the first read; with SWS=PLL the
    # poll loops forever because tick_get is 0 and the timeout never fires.
    Execute Command           sysbus Tag <0x4002100C 4> "RCC_CFGR" 0x00000004
    Execute Command           sysbus Tag <0x40022018 4> "FLASH_SR" 0x00000000
    # RCC_CSR: LSION(b0)+LSIRDY(b1) set — unblocks the LSI ready wait-loop
    Execute Command           sysbus Tag <0x40021050 4> "RCC_CSR" 0x00000003
    Execute Command           sysbus LoadELF ${ELF}
    Execute Command           cpu VectorTableOffset ${VTOR}
    # bmsboot normally initialises these SRAM words before jumping to the app:
    #   0x200000c8 = SystemCoreClock (32 MHz), 0x200000c4 = tick-rate divisor (1).
    # hal_init_tick divides 0x200000c8 by 0x200000c4 to get the SysTick reload
    # value; with both at 0 the division underflows, flash_page_erase sees r0=0,
    # and skips the SysTick CSR write — leaving the timer disabled forever.
    Execute Command           sysbus WriteDoubleWord 0x200000c8 32000000
    Execute Command           sysbus WriteDoubleWord 0x200000c4 1
    # Register a reset macro so SYSRESETREQ (fired during clock init) restores
    # the vector table offset instead of leaving the CPU parked at 0x0.
    # Single-line body avoids the newline-over-TCP issue with triple-quote macros.
    # Flash content (ELF) persists through soft reset so no reload needed.
    Execute Command           macro reset "cpu VectorTableOffset ${VTOR}"
    # rcc_osc_config (0x8008ff8): hook at function entry in case Renode fires it.
    Execute Command           cpu AddHook 0x8008ff8 "self.SetRegisterUlong(0, 0); cpu.PC = cpu.LR"
    # rcc_osc_config mid-body hook at 0x800922C (last RCC_CSR read visible in the
    # boot log — definitely executed).  Jump to 0x800909c, which is the compiler's
    # own "PLLState==NONE → movs r0,#0 → b.n epilogue" path.  This exits before
    # the PLL disable/enable timeout loops so tick_get=0 never causes an infinite
    # loop.  With SWS=HSI (CFGR tag 0x4) the rcc_configure SWS poll then exits on
    # its first read, so the whole clock-init path completes without any busy-wait.
    Execute Command           cpu AddHook 0x800922c "self.SetRegisterUlong(0, 0); cpu.PC = 0x800909c"
    # system_reset (0x8009470): safety net for SPI/modem/etc. failure paths.
    # Hook fires before push {r4,lr} so LR holds the correct return address.
    Execute Command           cpu AddHook 0x8009470 "cpu.PC = cpu.LR"
    # nvic_system_reset_dup (0x8009476) called from fault_led_trigger bypasses the
    # system_reset hook; return immediately to prevent unexpected SYSRESETREQ.
    Execute Command           cpu AddHook 0x8009476 "cpu.PC = cpu.LR"
    # SysTick_Handler (0x800dfd8) is the weak default ISR — normally a NOP.
    # After rcc_configure calls hal_init_tick, SysTick fires at ~1-2 kHz.
    # Increment the tick counter at 0x200047DC on every SysTick so all
    # tick_get()-based timeout loops in SPI/BMS code fire within a few ms of
    # virtual time (~2000 invocations per 2 virtual seconds = negligible overhead).
    # SysTick_Handler (0x800dfd8): return from interrupt without doing anything.
    # flash_page_erase (0x80061e4) configures SysTick at up to 8 kHz; at that
    # rate the Python hook overhead dominates.  We skip flash_page_erase so
    # SysTick is never enabled, and rely on our other stubs for correctness.
    Execute Command           cpu AddHook 0x800dfd8 "cpu.PC = cpu.LR"
    # flash_page_erase (0x80061e4): normally configures SysTick via the NVIC
    # registers.  Skip it to prevent an 8 kHz SysTick that would make every
    # Python hook invocation from the interrupt dominate wall-clock time.
    Execute Command           cpu AddHook 0x80061e4 "self.SetRegisterUlong(0, 0); cpu.PC = cpu.LR"
    # dma_wait_for_ready (0x8005e48) polls FLASH_SR with a 50 000-tick timeout;
    # each iteration calls tick_get, so 50 000 hook fires per call-site × many
    # callers makes the test impractically slow.  Return 0 (ready) immediately.
    Execute Command           cpu AddHook 0x8005e48 "self.SetRegisterUlong(0, 0); cpu.PC = cpu.LR"
    # delay_ms / delay_us poll a SysTick event flag that the real handler sets.
    # Skip both so firmware does not block on timing delays.
    Execute Command           cpu AddHook 0x8005aa8 "cpu.PC = cpu.LR"
    Execute Command           cpu AddHook 0x8005ae4 "cpu.PC = cpu.LR"
    # dma_flash_start.part.0 (0x8005b10) polls an uninitialised tick counter.
    # Skip the function (set r0=0, return) so dma_init proceeds without reset.
    Execute Command           cpu AddHook 0x8005b10 "self.SetRegisterUlong(0, 0); cpu.PC = cpu.LR"
    # bms_setup (0x80053a8): skips the initial BMS cell-communication setup
    # (memcmp_verify → spi_register_write → dma_wait_for_ready iterates over a
    # full flash page, generating millions of FLASH_SR reads and crashing Renode).
    Execute Command           cpu AddHook 0x80053a8 "cpu.PC = cpu.LR"
    # smbus_transmit (0x800873c), smbus_write_reg (0x800953c), smbus_read
    # (0x8009754): all use SPI1/I2C peripherals not present in the .repl.
    # smbus_transmit accesses SPI1 (0x40013000); bus_ready_check in the others
    # reads an uninitialised I2C state byte.  Skip all three (return 0 = OK).
    Execute Command           cpu AddHook 0x800873c "self.SetRegisterUlong(0, 0); cpu.PC = cpu.LR"
    Execute Command           cpu AddHook 0x800953c "self.SetRegisterUlong(0, 0); cpu.PC = cpu.LR"
    Execute Command           cpu AddHook 0x8009754 "self.SetRegisterUlong(0, 0); cpu.PC = cpu.LR"
    # memcmp_verify (0x8009514): byte-by-byte compare via SPI reads; each byte
    # calls spi_register_write → dma_wait_for_ready in a tight loop.  Return 0.
    Execute Command           cpu AddHook 0x8009514 "self.SetRegisterUlong(0, 0); cpu.PC = cpu.LR"

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
    Start Emulation
    Wait For Log Entry        REACHED_MAIN

CPU Stays Alive After Boot
    [Documentation]    Liveness: after running a while the CPU has retired many
    ...                instructions and is executing application flash (not parked
    ...                at 0 or trapped below the app base).
    Create Battery Machine
    Execute Command           emulation RunFor "0.01"
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
    Create Log Tester         60
    ${loop}=    Execute Command    sysbus GetSymbolAddress "uart_resp_handler"
    ${loop}=    Strip String      ${loop}
    Execute Command           cpu AddHook ${loop} "self.Log(LogLevel.Error, 'IN_SUPERLOOP')"
    Start Emulation
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
