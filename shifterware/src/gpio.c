/* gpio.c — port clocking and pin configuration helpers. */

#include "gpio.h"
#include "mm32f031.h"

/* Raw byte offsets used by the OEM `gpio_pin_configure`. The
 * STM32F1-style GPIO layout the MM32F031 actually uses puts:
 *   CRL @ 0x00, CRH @ 0x04, IDR @ 0x08, ODR @ 0x0C, BSRR @ 0x10,
 *   BRR @ 0x14, LCKR @ 0x18.
 * (The speculative `gpio_t` struct in `mm32f031.h` is the STM32F0
 * layout — wrong for this MCU — which is why these accesses bypass
 * the struct and use raw offsets.) */
#define GPIO_CRL_OFFS    0x00u
#define GPIO_CRH_OFFS    0x04u
#define GPIO_BSRR_OFFS   0x10u
#define GPIO_BRR_OFFS    0x14u

void gpio_port_clock_enable(const gpio_t *port)
{
    uint32_t bit = 0u;
    if      (port == GPIOA) bit = RCC_AHBENR_IOPAEN_Msk;
    else if (port == GPIOB) bit = RCC_AHBENR_IOPBEN_Msk;
    else if (port == GPIOC) bit = RCC_AHBENR_IOPCEN_Msk;
    else if (port == GPIOD) bit = RCC_AHBENR_IOPDEN_Msk;
    else if (port == GPIOF) bit = RCC_AHBENR_IOPFEN_Msk;
    if (bit != 0u) {
        RCC->AHBENR |= bit;
    }
}

void gpio_pin_mode(gpio_t *port, uint8_t pin, gpio_mode_t mode)
{
    const uint32_t shift = (uint32_t)(pin & 0xFu) * 2u;
    uint32_t v = port->MODER;
    v &= ~(0x3u << shift);
    v |= ((uint32_t)mode & 0x3u) << shift;
    port->MODER = v;
}

void gpio_pin_output_type(gpio_t *port, uint8_t pin, gpio_otype_t ot)
{
    const uint32_t bit = 1u << (pin & 0xFu);
    if (ot == GPIO_OTYPE_OD) {
        port->OTYPER |= bit;
    } else {
        port->OTYPER &= ~bit;
    }
}

void gpio_pin_speed(gpio_t *port, uint8_t pin, gpio_speed_t sp)
{
    const uint32_t shift = (uint32_t)(pin & 0xFu) * 2u;
    uint32_t v = port->OSPEEDR;
    v &= ~(0x3u << shift);
    v |= ((uint32_t)sp & 0x3u) << shift;
    port->OSPEEDR = v;
}

void gpio_pin_pull(gpio_t *port, uint8_t pin, gpio_pull_t pull)
{
    const uint32_t shift = (uint32_t)(pin & 0xFu) * 2u;
    uint32_t v = port->PUPDR;
    v &= ~(0x3u << shift);
    v |= ((uint32_t)pull & 0x3u) << shift;
    port->PUPDR = v;
}

void gpio_pin_alt_func(gpio_t *port, uint8_t pin, uint8_t af)
{
    const uint8_t idx   = (uint8_t)((pin & 0xFu) >> 3);   /* 0 for pin 0-7, 1 for 8-15 */
    const uint32_t shift = (uint32_t)(pin & 0x7u) * 4u;
    uint32_t v = port->AFR[idx];
    v &= ~(0xFu << shift);
    v |= ((uint32_t)af & 0xFu) << shift;
    port->AFR[idx] = v;
}

void gpio_pin_set(gpio_t *port, uint8_t pin)
{
    port->BSRR = 1u << (pin & 0xFu);
}

void gpio_pin_clear(gpio_t *port, uint8_t pin)
{
    port->BRR = 1u << (pin & 0xFu);
}

void gpio_pin_write(gpio_t *port, uint8_t pin, bool value)
{
    if (value) {
        gpio_pin_set(port, pin);
    } else {
        gpio_pin_clear(port, pin);
    }
}

bool gpio_pin_read(const gpio_t *port, uint8_t pin)
{
    return (port->IDR & (1u << (pin & 0xFu))) != 0u;
}

/* ---- OEM-confirmed GPIO helpers ----------------------------------- */

/* OEM @ 0x08004DBC (20 B). Test masked bits in the IDR of an
 * STM32F1-style GPIO port. Accesses the register by raw byte offset
 * (0x08) to bypass the speculative `gpio_t` struct, whose IDR field
 * is laid out at the STM32F0 offset (0x10) and so would point at the
 * wrong register on this MCU. Plan-2 will fix `gpio_t` itself. */
bool gpio_idr_test(void *port, uint32_t mask)
{
    return (*(volatile uint32_t *)((char *)port + 0x08u) & mask) != 0u;
}

/* OEM @ 0x0800325C (22 B). True iff GPIOA pin 0 reads high. */
bool input_pa0(void)
{
    return gpio_idr_test((void *)0x48000000u, 0x1u);
}

/* OEM @ 0x080033CC (22 B). True iff GPIOA pin 1 reads high. */
bool input_pa1(void)
{
    return gpio_idr_test((void *)0x48000000u, 0x2u);
}

/* OEM @ 0x08004DF4 (4 B). Raw BSRR write (sets bits at offset 0x10). */
void gpio_bsrr_write(void *port, uint32_t mask)
{
    *(volatile uint32_t *)((char *)port + 0x10u) = mask;
}

/* OEM @ 0x08004DF8 (4 B). Raw BRR write (clears bits at offset 0x14). */
void gpio_brr_write(void *port, uint32_t mask)
{
    *(volatile uint32_t *)((char *)port + 0x14u) = mask;
}

/* OEM @ 0x08004CCE (222 B). Configure one or more pins via CRL/CRH +
 * optional initial-level write. The OEM CRL/CRH encoding is 4-bit
 * MODE+CNF (STM32F1 style). Originally lived in uart.c (the only
 * caller until `boot_init_gpio` was decomp'd); now shared, so the
 * function moved here together with its `gpio_pin_cfg_t` (in
 * gpio.h). */
void gpio_pin_configure(void *port, const gpio_pin_cfg_t *cfg)
{
    char *base = (char *)port;

    uint32_t val = (uint32_t)(cfg->flags & 0xFu);
    if ((cfg->flags & 0x10u) != 0u) {
        val |= cfg->mode_extra;
    }

    /* low 8 pins via CRL */
    if ((cfg->pin_mask & 0xFFu) != 0u) {
        volatile uint32_t *crl = (volatile uint32_t *)(base + GPIO_CRL_OFFS);
        uint32_t cr = *crl;
        for (int pin = 0; pin < 8; pin++) {
            const uint32_t bit = 1u << pin;
            if ((cfg->pin_mask & bit) == 0u) continue;

            const uint32_t shift = (uint32_t)pin * 4u;
            cr = (cr & ~(0xFu << shift)) | (val << shift);

            if (cfg->flags == 0x28u) {
                *(volatile uint32_t *)(base + GPIO_BRR_OFFS)  = bit;
            } else if (cfg->flags == 0x48u) {
                *(volatile uint32_t *)(base + GPIO_BSRR_OFFS) = bit;
            }
        }
        *crl = cr;
    }

    /* high 8 pins via CRH */
    if (cfg->pin_mask > 0xFFu) {
        volatile uint32_t *crh = (volatile uint32_t *)(base + GPIO_CRH_OFFS);
        uint32_t cr = *crh;
        for (int pin = 0; pin < 8; pin++) {
            const uint32_t p16 = (uint32_t)pin + 8u;
            const uint32_t bit = 1u << p16;
            if ((cfg->pin_mask & bit) == 0u) continue;

            const uint32_t shift = (uint32_t)pin * 4u;
            cr = (cr & ~(0xFu << shift)) | (val << shift);

            if (cfg->flags == 0x28u) {
                *(volatile uint32_t *)(base + GPIO_BRR_OFFS)  = bit;
            } else if (cfg->flags == 0x48u) {
                *(volatile uint32_t *)(base + GPIO_BSRR_OFFS) = bit;
            }
        }
        *crh = cr;
    }
}
