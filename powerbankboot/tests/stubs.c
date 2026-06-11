/* tests/stubs.c — minimal vendor-leaf stubs so the powerbankboot reconstruction
 * links into a self-contained ELF for Renode testing.
 *
 * The real bootloader logic (startup, boot_hw_init, main, boot_main's A/B boot
 * decision and the USART2 ring/banner path) is exercised unchanged; these only
 * stand in for the STM32F0 HAL and the X-CUBE-STL (IEC-60730 Class-B) leaves that
 * are `extern` in the decomp — they resolve from a vendored HAL / ST STL library
 * in a real build. Every HAL call returns HAL_OK (0) so no bring-up step fails,
 * and nothing busy-waits on absent silicon.
 *
 * The STL self-test scaffold (stl_*) lives in main()'s dormant template, reached
 * only if boot_main() returns — which it never does on a normal boot — so those
 * stubs exist purely to satisfy the linker; the trace strings likewise.
 *
 * NOT part of the firmware image — test scaffolding only.
 */
#include <stdint.h>

/* ---- STM32F0 HAL: clock / RTC / UART / IWDG / GPIO init (HAL_OK == 0) ---- */
void     hal_init(void)                                      { }
int      hal_rcc_osc_config(void *osc)                       { (void)osc; return 0; }
int      hal_rcc_clock_config(void *clk, uint32_t lat)       { (void)clk; (void)lat; return 0; }
int      hal_rccex_periphclk_config(void *periph)            { (void)periph; return 0; }
int      hal_rtc_init(void *hrtc)                            { (void)hrtc; return 0; }
int      hal_uart_init(void *huart)                          { (void)huart; return 0; }
void     hal_iwdg_init(void *hiwdg)                          { (void)hiwdg; }
void     hal_gpio_init(uint32_t port, void *gpio)            { (void)port; (void)gpio; }

/* ---- NVIC (no-op: tests don't rely on interrupts firing) ---- */
void nvic_set_priority(int irqn, uint32_t pre, uint32_t sub) { (void)irqn; (void)pre; (void)sub; }
void nvic_enable_irq(int irqn)                               { (void)irqn; }

/* ---- flash (HAL leaves) ---- */
void     flash_lock(void)                                    { }
void     flash_hal_unlock(void)                              { }
void     flash_hal_clear_errors(void)                        { }
uint32_t flash_hal_erase_page(uint32_t page_addr, uint32_t *page_error) { (void)page_addr; if (page_error) *page_error = 0xFFFFFFFFu; return 0; }
uint32_t flash_hal_program_word(uint32_t addr, uint32_t data){ (void)addr; (void)data; return 0; }

/* ---- CRC-32 (MPEG-2). Return a fixed value so image_verify's CRC check fails on
 * an empty bank, keeping the loader resident instead of jumping into a blank AP
 * bank. The valid-image test hooks image_verify itself, so these aren't on that
 * path. ---- */
uint32_t crc32_calc(void *hcrc, const void *data, uint32_t nwords)       { (void)hcrc; (void)data; (void)nwords; return 0; }
uint32_t crc32_accumulate(void *hcrc, const void *data, uint32_t nwords) { (void)hcrc; (void)data; (void)nwords; return 0; }

/* ---- trace logger: swallow output (the real one gates on a trace-enable flag
 * and drives USART1; the tests read the USART2 TX ring directly). ---- */
void dbg_printf(const char *fmt, ...)                        { (void)fmt; }

/* ---- X-CUBE-STL fail-safe: a no-op return rather than the real spin-forever, so
 * a stray call can't hang a test (no tested path reaches it — clock config
 * returns HAL_OK). ---- */
void stl_failsafe(void)                                      { }

/* ---- X-CUBE-STL leaves (dormant template; satisfy the linker, return "pass") ---- */
void     stl_log_init(void)                                  { }
void     stl_log_init2(void)                                 { }
void     stl_startup(void)                                   { }
void     stl_clock_uart_startup(void)                        { }
int      stl_cpu_test(void)                                  { return 1; }
void     stl_iwdg_test(void)                                 { }
void     stl_crc_init(void)                                  { }
void     stl_flash_crc_test(void)                            { }
unsigned char stl_checkpoint_verify(unsigned expected)       { (void)expected; return 1; }
int      stl_ram_march_test(void *start, void *end, int pat) { (void)start; (void)end; (void)pat; return 1; }
void     stl_uart_handle_reset(void)                         { }
void     stl_iwdg_config(int test_mode)                      { (void)test_mode; }
void     stl_clock_test(void)                                { }
void     stl_clock_recover(void)                             { }
uint16_t stl_tick_phase(uint32_t subdiv, uint32_t period)    { (void)subdiv; (void)period; return 0; }

/* ---- STL trace strings (vendor rodata; referenced by the dormant template) ---- */
const char STL_INIT[] = "";
const char CPU_OK[]   = "";
const char CPU_NG[]   = "";
const char CP1_OK[]   = "";
const char CP1_NG[]   = "";
const char RAM_OK[]   = "";
const char RAM_NG[]   = "";
const char CP2_OK[]   = "";
const char CP2_NG[]   = "";
const char STL_DONE[] = "";
