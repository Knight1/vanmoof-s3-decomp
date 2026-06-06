#ifndef MAINWARE_UPDATE_H
#define MAINWARE_UPDATE_H

/* OTA firmware-update orchestrator. One super-loop-ticked state machine that
 * pulls a multi-file PACK package over BLE and flashes every subsystem on the
 * bike: mainware (self, shadow flash + NVICReset reboot to the bootloader),
 * motorware (bus), shifterware (bus, Vbat-gated), batteryware (Modbus slave
 * 0xAA, Vbat-gated), bleware (0x11b/0x11c OAD) and the powerbank.
 *
 * param = the session/app context (g_ctx). See docs/ota.md for the full state
 * map. OEM subsystem_update_sm at 0x08031900. */
void subsystem_update_sm(int ctx);

#endif
