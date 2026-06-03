/* strings.c — flash banner / trace strings, byte-for-byte from the OEM image.
 *
 * Addresses (OEM): banner 0x08006CB4, trace block 0x08006B8C..0x08006C8F.
 * The "%4x" forms are consumed by dbg_printf (the tinyprintf behind it).
 */
#include "powerbankboot.h"

const char STR_BANNER[]         = "\nI am VM-BATT BL\r";
const char STR_IN_BL[]          = "-->In BL \n\r";
const char STR_UPGRADE_FIN[]    = "-->fBMS_Upgrade_Finish = 1 \n\r";
const char STR_CRC_VERIFY[]     = "-->CRC Verify 0x%4x%4x = ";
const char STR_RC_OK[]          = "0\n\r";
const char STR_RC_CRC[]         = "1\n\r";
const char STR_RC_MAGIC[]       = "2\n\r";
const char STR_COPY_AP2SH[]     = "Copy AP to Shadow";
const char STR_COPY_SH2AP[]     = "Copy Shadow to AP";
const char STR_DONE[]           = "--> Done\n\r";
const char STR_GOTO_APP[]       = "-->Goto_Application()\n\r";
const char STR_WRITE_FLASH_NG[] = "Write Flash NG\n\r";
const char STR_HARDFAULT_NG[]   = "HardFault NG\n\r";
const char STR_SVC_NG[]         = "SVC NG\n\r";
const char STR_NMI_NG1[]        = "NMI NG1\n\r";
const char STR_NMI_NG2[]        = "NMI NG2\n\r";
