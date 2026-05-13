# Peripherals used by shifterware

Tick off as decompilation confirms each block is touched by the OEM.

| Peripheral | Used? | Pin(s) | Driver file | Notes |
| --- | --- | --- | --- | --- |
| RCC (clock tree) | ❓ | — | `src/system_mm32f031.c` | likely runs PLL @ 48 MHz off HSI |
| GPIO A | ❓ | — | `src/gpio.c` | |
| GPIO B | ❓ | — | `src/gpio.c` | |
| GPIO C | ❓ | — | `src/gpio.c` | |
| GPIO D | ❓ | — | `src/gpio.c` | |
| GPIO F | ❓ | — | `src/gpio.c` | LQFP-20 has limited pins; F may not be bonded |
| USART1 | ❓ | — | `src/uart.c` | candidate for main-module link |
| USART2 | ❓ | — | `src/uart.c` | |
| I2C1 | ❓ | — | `src/i2c.c` | candidate for sensor / EEPROM |
| SPI1 | ❓ | — | `src/spi.c` | |
| TIM1 | ❓ | — | `src/timer.c` | advanced TIM, candidate for motor PWM |
| TIM3 | ❓ | — | `src/timer.c` | general-purpose, candidate for stepping |
| TIM14 | ❓ | — | `src/timer.c` | |
| TIM16 | ❓ | — | `src/timer.c` | |
| TIM17 | ❓ | — | `src/timer.c` | |
| ADC1 | ❓ | — | `src/adc.c` | likely voltage / Hall position |
| EXTI | ❓ | — | `src/exti.c` | external interrupts (gear button?) |
| RTC | ❓ | — | — | unlikely on a peripheral MCU |
| IWDG | ❓ | — | `src/watchdog.c` | independent watchdog |
| WWDG | ❓ | — | `src/watchdog.c` | window watchdog |
| FLASH (writes) | ❓ | — | `src/flash.c` | calibration / config write back? |
| CRC | ❓ | — | `src/crc.c` | hardware CRC unit |
| DMA1 | ❓ | — | `src/dma.c` | likely UART / SPI offload |

The MM32F031F6U6 is the **LQFP-20 package** — only ~16 GPIO pins are
bonded. Pinout to confirm against PCB photos in
[`ciborg971/VanmoofX3RE`](https://github.com/ciborg971/VanmoofX3RE).
