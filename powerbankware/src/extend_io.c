#include "powerbankware.h"

/*
 * Extend_IO LED-bar output driver — OEM FUN_08014350.
 *
 * powerbank-specific (no batteryware analogue): maps the BMS state byte plus
 * the SOC % to an LED-bar level pattern and pushes it to an I/O-expander on
 * the *same* SPI bus as the FEDL5236, but with CS on GPIOA PA8 (0x100). The
 * level byte is only re-sent when it changes (shadowed at 0x200006e5).
 *
 * SOC -> bar level: 0xFC (>=90%), 0xF4 (>=75%), 0xE4 (>=50%), 0xC4 (>=25%),
 * 0x84 (1..24%), 0x00 (empty). State 2 (charging) animates the bar by
 * stepping a blink counter; state 3 blinks the whole bar between full and a
 * one-segment-dimmer pattern. States 7..0x18 and 0x19..0x1b/0xFF force 0xFA
 * (all-on/fault); states 5/6 clear it.
 *
 * Shared SRAM / SPI cells (widths verified against the disassembly):
 *   0x200006a0 u16 mode      0x200005ac u8 state     0x2000052a u8 SOC
 *   0x200006a4 u8 led_level  0x20000728 u16 blink    0x20000420 u32 cnt
 *   0x200003c8 u32 val644    0x200006ec u8 led_out   0x200006e5 u8 led_shadow
 *   0x2000077c u8 tick-flags  + the FEDL5236 SPI handle/buffers.
 */

#define EXTIO_SPI_HANDLE ((void *)0x20000634)
#define EXTIO_CS_PORT    0x48000000u    /* GPIOA */
#define EXTIO_CS_PIN     0x100u         /* PA8   */

static volatile uint16_t * const s_mode       = (volatile uint16_t *)0x200006a0;
static volatile uint8_t  * const s_state      = (volatile uint8_t  *)0x200005ac;
static volatile uint8_t  * const s_soc        = (volatile uint8_t  *)0x2000052a;
static volatile uint8_t  * const s_led_level  = (volatile uint8_t  *)0x200006a4;
static volatile uint16_t * const s_blink      = (volatile uint16_t *)0x20000728;
static volatile uint32_t * const s_cnt        = (volatile uint32_t *)0x20000420;
static volatile uint32_t * const s_val644     = (volatile uint32_t *)0x200003c8;
static volatile uint8_t  * const s_led_out    = (volatile uint8_t  *)0x200006ec;
static volatile uint8_t  * const s_led_shadow = (volatile uint8_t  *)0x200006e5;
static volatile uint8_t  * const s_flags      = (volatile uint8_t  *)0x2000077c;
static volatile uint8_t  * const s_tx         = (volatile uint8_t  *)0x200005f4;
static volatile uint8_t  * const s_rx         = (volatile uint8_t  *)0x20000614;
static volatile uint8_t  * const s_count      = (volatile uint8_t  *)0x200005f1;

extern const char s_extio_busy[];
extern const char s_extio_retry[];
extern const char s_extio_timeout[];

/* FUN_080149bc — empty in this firmware (bit-11 gated hook). */
static void extend_io_aux(void)
{
}

static uint8_t soc_bar(uint8_t soc)
{
    if (soc >= 0x5a) return 0xfc;
    if (soc >= 0x4b) return 0xf4;
    if (soc >= 0x32) return 0xe4;
    if (soc >= 0x19) return 0xc4;
    if (soc != 0)    return 0x84;
    return 0;
}

/* One bar segment dimmer than soc_bar — used for the state-3 blink. */
static uint8_t soc_dim(uint8_t soc)
{
    if (soc >= 0x5a) return 0xf4;
    if (soc >= 0x4b) return 0xe4;
    if (soc >= 0x32) return 0xc4;
    if (soc >= 0x19) return 0x84;
    return 0;
}

/* Advance the blink counter, wrapping past `limit`. */
static void blink_step(uint16_t limit)
{
    uint16_t v = *s_blink;
    *s_blink = (uint16_t)(v + 1);
    if (limit < (uint16_t)(v + 1)) {
        *s_blink = 0;
    }
}

/* LAB_0801482c — fold mode bit 12 into bit 0, OR the level, and push to the
 * Extend_IO expander when it differs from the last value sent. */
static void extend_io_commit(void)
{
    *s_led_out = (*s_mode & 0x1000) ? 1 : 0;
    *s_led_out = (uint8_t)(*s_led_out | (*s_led_level & 0xfe));
    if (*s_led_out == *s_led_shadow) {
        return;
    }

    uint8_t retry = 0;
    do {
        do {
            if (spi_get_state(EXTIO_SPI_HANDLE) == 1) {
                *s_led_shadow = *s_led_out;
                gpio_bit_write(EXTIO_CS_PORT, EXTIO_CS_PIN, 0);   /* CS PA8 low */
                s_tx[0] = *s_led_out;
                *s_count = 1;
                if (spi_transmit_receive(EXTIO_SPI_HANDLE, s_tx, s_rx, *s_count) != 0) {
                    log_print(2, s_extio_retry);
                    uart_flush();
                    spi_error_reset();
                }
                retry = 0;
                do {
                    do {
                        if (spi_get_state(EXTIO_SPI_HANDLE) == 1) {
                            return;
                        }
                    } while ((*s_flags & 1) == 0);
                    *s_flags &= (uint8_t)~1u;
                    retry++;
                } while (retry < 10);
                log_print(2, s_extio_timeout);
                uart_flush();
                return;
            }
        } while ((*s_flags & 1) == 0);
        *s_flags &= (uint8_t)~1u;
        retry++;
    } while (retry < 10);

    log_print(2, s_extio_busy);
    uart_flush();
}

void extend_io_update(void)
{
    if ((*s_mode & 0x800) != 0) {      /* mode bit 11 */
        extend_io_aux();
    }

    uint8_t state = *s_state;
    uint8_t soc   = *s_soc;

    /* All-on / fault patterns. */
    if ((state >= 7 && state <= 0x18) ||
        (state >= 0x19 && (state < 0x1c || state == 0xff))) {
        *s_led_level = 0xfa;
        *s_blink = 0;
        extend_io_commit();
        return;
    }

    switch (state) {
    case 3:
        if (*s_cnt < 200) {
            *s_led_level = soc_bar(soc);
            *s_blink = 0;
        } else {
            if (*s_blink == 0) {
                *s_led_level = soc_bar(soc);
            } else if (*s_blink == 0xc) {
                *s_led_level = soc_dim(soc);
            }
            blink_step(0x17);
        }
        extend_io_commit();
        return;

    case 2:
        if (soc < 0x5f || *s_val644 > 299) {
            if (soc < 0x5a) {
                if (soc < 0x4b) {
                    if (soc < 0x32) {
                        if (soc < 0x19) {                  /* 0..0x18 */
                            if (*s_blink == 0)         *s_led_level = 0;
                            else if (*s_blink == 0xc)  *s_led_level = 0x84;
                            blink_step(0x17);
                        } else {                            /* 0x19..0x31 */
                            if (*s_blink == 0)         *s_led_level = 0;
                            else if (*s_blink == 8)    *s_led_level = 0x84;
                            else if (*s_blink == 0x10) *s_led_level = 0xc4;
                            blink_step(0x17);
                        }
                    } else {                                /* 0x32..0x4a */
                        if (*s_blink == 0)         *s_led_level = 0;
                        else if (*s_blink == 6)    *s_led_level = 0x84;
                        else if (*s_blink == 0xc)  *s_led_level = 0xc4;
                        else if (*s_blink == 0x12) *s_led_level = 0xe4;
                        blink_step(0x17);
                    }
                } else {                                    /* 0x4b..0x59 */
                    if (*s_blink == 0)         *s_led_level = 0;
                    else if (*s_blink == 5)    *s_led_level = 0x84;
                    else if (*s_blink == 10)   *s_led_level = 0xc4;
                    else if (*s_blink == 0xf)  *s_led_level = 0xe4;
                    else if (*s_blink == 0x14) *s_led_level = 0xf4;
                    blink_step(0x18);
                }
            } else {                                        /* 0x5a..0x5e */
                if (*s_blink == 0)         *s_led_level = 0;
                else if (*s_blink == 4)    *s_led_level = 0x84;
                else if (*s_blink == 8)    *s_led_level = 0xc4;
                else if (*s_blink == 0xc)  *s_led_level = 0xe4;
                else if (*s_blink == 0x10) *s_led_level = 0xf4;
                else if (*s_blink == 0x14) *s_led_level = 0xfc;
                blink_step(0x17);
            }
        } else {
            *s_led_level = 0xfc;
            *s_blink = 0;
        }
        extend_io_commit();
        return;

    case 5:
    case 6:
        *s_led_level = 0;
        *s_blink = 0;
        *s_mode &= (uint16_t)~0x1000u;
        extend_io_commit();
        return;

    default:
        break;       /* states 0, 1, 4, 0x1c..0xfe: steady SOC bar */
    }

    *s_led_level = soc_bar(soc);
    *s_blink = 0;
    extend_io_commit();
}
