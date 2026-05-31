#include "powerbankware.h"

/*
 * Flash log/banner strings, pinned by content from the OEM image. The linker
 * folds .rodata into .text (see linker_stm32f091.ld), so these live in flash
 * like the originals; code references them by symbol.
 *
 * Only the strings reached by already-decompiled code are materialised here;
 * more are added as functions land. OEM addresses are noted for traceability.
 */

/* main() — entry banner + the idblk[2] "Record <fault> Mode" factory entries */
const char s_msg_iam_ap[]      = "\nI am VM-BATT AP\r";        /* 0x0801E05C */
const char s_msg_rec_mosfail[] = "\nRecord MOS Failure Mode\r"; /* 0x0801E070 (sel 0x17) */
const char s_msg_rec_ov2[]     = "\nRecord OV 2nd Mode\r";     /* 0x0801E08C (sel 0x18) */
const char s_msg_rec_cotp[]    = "\nRecord COTP Mode\r";       /* 0x0801E0A4 (sel 0x12) */
const char s_msg_rec_cutp[]    = "\nRecord CUTP Mode\r";       /* 0x0801E0B8 (sel 0x13) */
const char s_msg_rec_dotp[]    = "\nRecord DOTP Mode\r";       /* 0x0801E0CC (sel 0x14) */
const char s_msg_rec_dutp[]    = "\nRecord DUTP Mode\r";       /* 0x0801E0E0 (sel 0x15) */
