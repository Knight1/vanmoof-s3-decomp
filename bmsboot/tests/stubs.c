/* tests/stubs.c — minimal vendor-leaf stubs so the bmsboot reconstruction links
 * into a self-contained ELF for Renode testing.
 *
 * The real bootloader logic (startup, boot_hw_init, main, the boot decision and
 * the UART ring/banner path) is exercised unchanged; these only stand in for the
 * STM32L0 HAL / CMSIS leaves that are `extern` in the decomp (they resolve from a
 * vendored HAL in a real build). Every HAL call returns HAL_OK (0) so the OEM
 * "retry up to 50× then fail-safe" loops exit on the first attempt, and nothing
 * busy-waits on absent silicon.
 *
 * NOT part of the firmware image — test scaffolding only.
 */
#include <stdint.h>

/* ---- RCC / clock / CRC / IWDG init (HAL_OK == 0) ---- */
uint32_t hal_rcc_osc_config(void *osc)                       { (void)osc; return 0; }
uint32_t hal_rcc_clock_config(void *clk, uint32_t lat)       { (void)clk; (void)lat; return 0; }
uint32_t hal_rcc_periph_clk_config(void *periph)             { (void)periph; return 0; }
uint32_t hal_crc_init(void *hcrc)                            { (void)hcrc; return 0; }
uint32_t hal_iwdg_init(void *hiwdg)                          { (void)hiwdg; return 0; }

/* ---- flash ---- */
void     hal_flash_unlock(void)                              { }
void     hal_flash_unlock2(void)                             { }
uint32_t hal_flash_erase(void *erase_init, uint32_t *perr)   { (void)erase_init; if (perr) *perr = 0xFFFFFFFFu; return 0; }
/* Write-through so the read-back-verify loop in flash_program() terminates and
 * EEPROM-backed state (boot flag, reset cause) actually persists into the mapped
 * EEPROM region — letting the boot-flag state-machine tests observe the result.
 * The decomp only ever calls this with type 0 (byte program). */
uint8_t  hal_flash_program(int type, uint8_t *addr, uint32_t data) {
    if (type == 0) *(volatile uint8_t *)addr = (uint8_t)data;
    else           *(volatile uint32_t *)addr = data;
    return 0;
}
uint32_t hal_flash_program_halfpage(uint32_t addr, const uint8_t *src) { (void)addr; (void)src; return 0; }

/* ---- GPIO ---- */
void hal_gpio_init(uint32_t port, void *gpio)               { (void)port; (void)gpio; }
void hal_gpio_deinit(uint32_t port, uint16_t pins)          { (void)port; (void)pins; }
void hal_gpio_write(uint32_t port, uint16_t pins, int state){ (void)port; (void)pins; (void)state; }
/* PA10 (download request) reads low by default → normal boot. The UART-output
 * test hooks this to return 1 to take the serial-download path. */
int  hal_gpio_read(uint32_t port, uint16_t pin)             { (void)port; (void)pin; return 0; }

/* ---- UART ---- */
uint32_t hal_uart_init(void *huart)                         { (void)huart; return 0; }
void     hal_uart_deinit(void *huart)                       { (void)huart; }

/* ---- NVIC / SysTick (no-op: tests don't rely on interrupts firing) ---- */
void nvic_set_priority(int irqn, uint32_t pre, uint32_t sub){ (void)irqn; (void)pre; (void)sub; }
void nvic_enable_irq(int irqn)                              { (void)irqn; }
void nvic_disable_irq(int irqn)                             { (void)irqn; }
/* return non-zero ⇒ "reload too large"; return 0 and do NOT arm SysTick, so the
 * boot-decision/handoff path stays quiescent and main() loops deterministically. */
uint32_t systick_set_reload(uint32_t ticks)                { (void)ticks; return 0; }

/* ---- CRC-32 accumulate: return a fixed value so image_verify's CRC check fails
 * (no real app is loaded), keeping the loader resident instead of jumping to an
 * empty AP bank. ---- */
uint32_t crc32_accumulate(void *hcrc, const void *data, uint32_t nwords) { (void)hcrc; (void)data; (void)nwords; return 0; }

/* ---- vendor HAL handle storage ---- */
uint8_t  g_huart1[0x84];                 /* USART1 UART_HandleTypeDef */
static uint8_t g_hcrc_storage[0x40];
void    *g_hcrc = g_hcrc_storage;        /* CRC_HandleTypeDef         */
