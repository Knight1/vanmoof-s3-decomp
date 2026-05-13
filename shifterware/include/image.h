#ifndef SHIFTER_IMAGE_H
#define SHIFTER_IMAGE_H

/* Validate the flash-resident image header at the address held by
 * the module-local g_image pointer (currently 0x08001800).
 *
 *   0 -> OK (magic + CRC32 both match)
 *   1 -> CRC mismatch
 *   2 -> bad magic, or `length` >= 0x3000 (12 KB) */
int image_verify_crc(void);

/* Re-check the receive-slot image and act on the result: on success,
 * latch the "image OK" flag; on failure, erase the slot and reset
 * receive state. Snapshots a status byte and bus-reports unconditionally.
 */
void image_apply(void);

#endif /* SHIFTER_IMAGE_H */
