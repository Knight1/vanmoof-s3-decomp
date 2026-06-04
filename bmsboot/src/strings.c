/* strings.c — flash banner strings, byte-for-byte from the OEM rodata.
 *
 * Three NUL-terminated banners live in the rodata block at 0x08004900..0x0800496E:
 *   0x08004900  "\nI am VanMoof BL V006 \r"   — WHO?-handshake reply
 *   0x08004918  "\nI am VanMoof BL V007 2022-11-04 09:32:30\r" — startup banner
 *   0x08004944  "\nI am VanMoof BL V006 \r"   — super-loop re-announce
 *
 * The V007 string carries the build identity/date; the two V006 copies are the
 * protocol-version banner sent to the host (reproduced verbatim, including the
 * duplication).
 */
#include "bmsboot.h"

/* 0x08004918 — transmitted once at startup (main). */
const char STR_BANNER_V007[] = "\nI am VanMoof BL V007 2022-11-04 09:32:30\r";

/* 0x08004944 — transmitted from the super-loop boot path (main). */
const char STR_BANNER_V006[] = "\nI am VanMoof BL V006 \r";

/* 0x08004900 — transmitted in reply to the "WHO?\r" handshake (ota_process_byte). */
const char STR_BANNER_WHO[]  = "\nI am VanMoof BL V006 \r";
