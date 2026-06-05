#include "states.h"

/* Map an alarm/bike state code (0..0x3D) to its name (OEM alarm_state_name,
 * 0x08032DF0). The OEM is a `tbh` jump table; this behaviour-equivalent switch
 * returns the same const char* (into flash rodata in the OEM, string literals
 * here). These are the bike's full alarm / power / lock / OTA(OAD) / PIN state
 * names — the codes status_process logs and that maybe_get_bike_state tracks.
 * Out-of-range returns "UNKNOWN". */
const char *alarm_state_name(uint32_t state)
{
    switch (state) {
    case 0x00: return "ALARM_PRE_M1";
    case 0x01: return "ALARM_ACTIVE_M1_CNT";
    case 0x02: return "ALARM_ACTIVE_M2";
    case 0x03: return "ALARM_TRACKING_UNCONFIRMED";
    case 0x04: return "ALARM_TRACKING_CONFIRMED";
    case 0x05: return "ALARM_BMS_REMOVED";
    case 0x06: return "SET_SHIPPING";
    case 0x07: return "SHIPPING";
    case 0x08: return "BIKE_SHIPPING_ACCIDENTAL_WAKE";
    case 0x09: return "BIKE_SHIPPING_LIPOCHARGE";
    case 0x0A: return "START_FROM_SHIPPING";
    case 0x0B: return "PLAY_FIRE";
    case 0x0C: return "RIDING_MODE";
    case 0x0D: return "INIT";
    case 0x0E: return "STANDBY";
    case 0x0F: return "CPU_STOP_MODE";
    case 0x10: return "CPU_STOPPED";
    case 0x11: return "SHOW_LOCK";
    case 0x12: return "AUTOWAKEUP";
    case 0x13: return "CARDRIDGE_REMOVED";
    case 0x14: return "LIPOCHARGE";
    case 0x15: return "CHARGING";
    case 0x16: return "RESET";
    case 0x17: return "DIAGNOSE";
    case 0x18: return "DIAG_RDY";
    case 0x19: return "OAD_UPDATE";
    case 0x1A: return "OAD_FILE_TRF";
    case 0x1B: return "OAD_FAILED";
    case 0x1C: return "OAD_RX_SOUND";
    case 0x1D: return "OAD_FINISH";
    case 0x1E: return "FACTORY_TEST";
    case 0x1F: return "PLAY_SHTDN";
    case 0x20: return "PLAY_LOCK_SHTDN";
    case 0x21: return "PLAY_LOCK_FROM_SLEEP";
    case 0x22: return "PLAY_SHTDN_RDY";
    case 0x23: return "ALARM_DELAY_ON";
    case 0x24: return "TURN_ON";
    case 0x25: return "LOW_SOC";
    case 0x26: return "PIN_START";
    case 0x27: return "PIN_STUCK";
    case 0x28: return "PIN_1ST";
    case 0x29: return "PIN_2ND";
    case 0x2A: return "PIN_3ND";
    case 0x2B: return "PIN_CHECK";
    case 0x2C: return "PIN_OK";
    case 0x2D: return "PIN_SHOW_OK";
    case 0x2E: return "PIN_NOK";
    case 0x2F: return "PIN_NOK_SHOW";
    case 0x30: return "UNLOCK";
    case 0x31: return "EXTRA_ALREADY_UNLOCKED";
    case 0x32: return "UNLOCK_COUNT";
    case 0x33: return "UNLOCK_COUNT_TIMEOUT";
    case 0x34: return "LOCK_PLAY_UNLOCK";
    case 0x35: return "LOCK_PLAY_START";
    case 0x36: return "LOCK_DIM_OFF";
    case 0x37: return "LOCK_CLEAR";
    case 0x38: return "LOCK_SETUP";
    case 0x39: return "LOCK_PIC";
    case 0x3A: return "LOCK_COUNT";
    case 0x3B: return "COUNT_OFF";
    case 0x3C: return "COUNT_CLEAR";
    case 0x3D: return "FIND_MY_PLAY";
    default:   return "UNKNOWN";
    }
}
