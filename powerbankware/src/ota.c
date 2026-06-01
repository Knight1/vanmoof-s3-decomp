#include "powerbankware.h"

/*
 * Binary-channel firmware-update (OTA) receiver — OEM FUN_0800b518, the binary
 * path of uart_rx_handler. A small framed protocol that stages an incoming
 * image into the OTA flash region (0x08024000) and, on the execute command,
 * verifies it and copies it into the live AP/BL region before resetting.
 *
 *   modem_rx_byte = FUN_0800b518   image_verify = FUN_0800fed8
 *   ota_respond   = FUN_0800ba58 (ack byte + reset protocol state)
 *
 * State cells: state 0x2000037e, byte index 0x2000037c, frame buffer
 * 0x20000278, xor 0x20000276, command 0x20000277, length+1 0x20000274,
 * target address 0x20000378. Flash regions: BL 0x08000000, AP 0x08008000,
 * OTA staging 0x08024000.
 */

extern void uart_flush_ch1(void);                  /* FUN_080161b4 */
extern void flash_lock(void);                      /* FUN_0801a298 (src/flash.c) */
extern int  image_verify_ap(int *image);           /* FUN_0800fe48: AP-header CRC verify */
void image_copy(uint32_t dst, uint32_t src, uint32_t len);  /* FUN_0800ff18 — below */

extern const char s_copy_shadow_to_ap[], s_copy_ap_to_shadow[],
                  s_copy_shadow_to_bl[], s_copy_done[];

static volatile uint8_t  * const s_state = (volatile uint8_t  *)0x2000037e;
static volatile uint16_t * const s_idx   = (volatile uint16_t *)0x2000037c;
static volatile uint8_t  * const s_buf   = (volatile uint8_t  *)0x20000278;
static volatile uint8_t  * const s_xor   = (volatile uint8_t  *)0x20000276;
static volatile uint8_t  * const s_cmd   = (volatile uint8_t  *)0x20000277;
static volatile uint16_t * const s_len   = (volatile uint16_t *)0x20000274;
static volatile uint32_t * const s_addr  = (volatile uint32_t *)0x20000378;
static volatile uint16_t * const s_mode  = (volatile uint16_t *)0x200006a0;

/* CRC the 0x1fff-word image at `addr` and compare to its stored CRC at
 * +0x7ffc. Returns true on mismatch. */
static bool image_verify(uint32_t addr)
{
    uint32_t crc = crc_accumulate((void *)0x200006c0, (const void *)addr, 0x1fff);
    return (int32_t)crc != *(volatile int32_t *)(addr + 0x7ffc);
}

/* Send a response byte (ACK 0x79 / NAK 0x1f) and reset the protocol state. */
static void ota_respond(uint8_t channel, uint8_t b)
{
    uart_putchar(b);
    (void)channel;
    *s_xor = 0;
    *s_addr = 0;
    *s_cmd = 0;
    *s_state = 0;
    *s_idx = 0;
}

static void ota_flush(uint8_t channel)
{
    if (channel == 1) {
        uart_flush_ch1();
    } else {
        uart_flush();
    }
}

/* Latch the upgrade flag (`flag`/~flag) into the RTC backup regs and reset. */
static void ota_set_flag_and_reset(uint8_t flag)
{
    *(volatile uint8_t *)0x20000724 = flag;
    *(volatile uint8_t *)0x20000698 = (uint8_t)~*(volatile uint8_t *)0x20000724;
    rtc_backup_write((void *)0x200006f0, 0, *(volatile uint8_t *)0x20000724);
    rtc_backup_write((void *)0x200006f0, 1, *(volatile uint8_t *)0x20000698);
    system_reset_ota();
}

void modem_rx_byte(uint8_t channel, uint8_t b)
{
    char st = (char)*s_state;

    if (st == 1) {
        /* Accumulate a 5-byte address frame (4 addr bytes + XOR checksum). */
        s_buf[*s_idx] = b;
        uint16_t prev = *s_idx;
        *s_idx = (uint16_t)(prev + 1);
        if ((uint16_t)(prev + 1) <= 4) {
            return;
        }
        *s_idx = 0;
        uint8_t x = (uint8_t)(s_buf[0] ^ s_buf[1] ^ s_buf[2] ^ s_buf[3]);
        *s_xor = x;
        if (s_buf[4] != x) {
            ota_respond(channel, 0x1f);
            return;
        }
        uint32_t addr = ((uint32_t)s_buf[0] << 24) | ((uint32_t)s_buf[1] << 16) |
                        ((uint32_t)s_buf[2] << 8) | s_buf[3];

        if ((*s_mode & 1) == 0) {                       /* AP update mode */
            if (addr > 0x08007fff && addr <= 0x08023fff) {
                if (*s_cmd == 0x31) {
                    ota_respond(channel, 0x79);
                    *s_addr = addr;
                    *s_state = 2;
                } else if (*s_cmd == 0x21) {
                    if (addr == 0x08008000) {
                        ota_respond(channel, 0x79);
                        ota_flush(channel);
                        ota_set_flag_and_reset(1);
                    } else {
                        ota_respond(channel, 0x1f);
                    }
                }
            } else {
                ota_respond(channel, 0x1f);
            }
        } else if (addr > 0x07ffffff && addr <= 0x08007fff) {   /* BL update mode */
            if (*s_cmd == 0x31) {
                ota_respond(channel, 0x79);
                *s_addr = addr;
                *s_state = 2;
            } else if (*s_cmd == 0x21) {
                if (addr == 0x08000000 && !image_verify(0x08024000)) {
                    ota_respond(channel, 0x79);
                    ota_flush(channel);
                    image_copy(0x08000000, 0x08024000, 0x8000);   /* BL */
                    image_copy(0x08024000, 0x08008000, 0x1c000);  /* AP */
                    ota_set_flag_and_reset(0);
                }
                ota_respond(channel, 0x1f);
                image_copy(0x08024000, 0x08008000, 0x1c000);
            }
        } else {
            ota_respond(channel, 0x1f);
        }
        return;
    }

    if (st == 2) {
        /* Data block: [length][data...][xor]. */
        if (*s_idx == 0) {
            *s_xor = b;
            *s_len = (uint16_t)(b + 1);
            *s_idx = (uint16_t)(*s_idx + 1);
        } else if ((uint32_t)*s_idx < (uint32_t)(*s_len + 1)) {
            s_buf[*s_idx - 1] = b;
            *s_xor = (uint8_t)(b ^ *s_xor);
            *s_idx = (uint16_t)(*s_idx + 1);
        } else if (b == *s_xor) {
            /* Relocate the target address into the OTA staging region. */
            if ((*s_mode & 1) == 0) {
                *s_addr += 0xf7ff8000u;
            } else {
                *s_addr += 0xf8000000u;
            }
            *s_addr += 0x08024000u;

            if ((*s_addr & 0x7ff) == 0) {
                flash_erase_page(*s_addr);
            }
            uint32_t a = *s_addr;
            mem_copy((void *)((a & 0x7ff) + 0x20001e04), (const void *)s_buf, *s_len);
            int r = flash_program_words(*s_addr & 0xffffff00, *s_len, (const void *)s_buf);
            while (r != 0) {
                **(volatile uint32_t **)0x200006ac = 0x0000AAAAu;   /* IWDG kick */
                flash_erase_page(*s_addr & 0xfffff800);
                r = flash_program_words(*s_addr & 0xfffff800,
                                        (uint16_t)((a & 0x7ff) + *s_len),
                                        (const void *)0x20001e04);
            }
            ota_respond(channel, 0x79);
        } else {
            ota_respond(channel, 0x1f);
        }
        return;
    }

    if (st == 0) {
        /* Wait for command (0x21/0x31) then its magic byte (0xde/0xce). */
        if (*s_idx == 0) {
            if (b == 0x21 || b == 0x31) {
                *s_cmd = b;
                *s_idx = (uint16_t)(*s_idx + 1);
            } else {
                *s_cmd = 0;
                *s_idx = 0;
            }
        } else if (*s_idx == 1) {
            if ((b == 0xde && *s_cmd == 0x21) || (b == 0xce && *s_cmd == 0x31)) {
                ota_respond(channel, 0x79);
                *s_state = (uint8_t)(*s_state + 1);
                *s_cmd = (uint8_t)~b;
            } else {
                ota_respond(channel, 0x1f);
            }
        }
    }
}

/* FUN_080139e0 — copy `len` bytes from flash `src` (word-read) into `dst`. */
static void flash_read(uint32_t src, uint16_t len, uint8_t *dst)
{
    const volatile uint32_t *s = (const volatile uint32_t *)src;
    uint16_t i = 0;
    while (i < len) {
        uint32_t w = *s++;
        if (i < len) { dst[i++] = (uint8_t)w; }
        if (i < len) { dst[i++] = (uint8_t)(w >> 8); }
        if (i < len) { dst[i++] = (uint8_t)(w >> 16); }
        if (i < len) { dst[i++] = (uint8_t)(w >> 24); }
    }
}

/*
 * image_copy — OEM FUN_0800ff18. Copy a `len`-byte flash region from `src` to
 * `dst` 2 KB page at a time (read page -> erase dst page -> program, retrying a
 * page until it programs), then verify the whole destination image and retry
 * the entire copy until it passes. The BL region (0x08000000) uses the raw CRC
 * check; the AP / staging regions use the VanMoof-header CRC verify.
 */
void image_copy(uint32_t dst, uint32_t src, uint32_t len)
{
    if (dst == 0x08008000) {
        log_print(2, s_copy_shadow_to_ap);
    } else if (dst == 0x08024000) {
        log_print(2, s_copy_ap_to_shadow);
    } else if (dst == 0x08000000) {
        log_print(2, s_copy_shadow_to_bl);
    }
    uart_flush();

    uint8_t page[0x800];
    int bad;
    do {
        for (uint32_t i = 0; i < len; i += 0x800) {
            flash_read(src + i, 0x800, page);
            int r;
            do {
                flash_erase_page(dst + i);
                r = flash_program_words(dst + i, 0x800, page);
            } while (r != 0);
        }
        if (dst == 0x08000000) {
            bad = image_verify(dst) ? 1 : 0;
        } else {
            bad = image_verify_ap((int *)dst);
        }
    } while (bad != 0);

    flash_lock();
    log_print(2, s_copy_done);
    uart_flush();
}
