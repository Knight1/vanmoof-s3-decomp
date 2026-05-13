#ifndef SHIFTER_H
#define SHIFTER_H

/* Project-wide types and forward declarations.
 *
 * Add high-level shifter concepts here (gear states, comm message IDs,
 * etc.) once they're identified in the OEM image. Don't speculate. */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Firmware version word as it appears in the OEM image header. */
#define SHIFTERWARE_VERSION_WORD   (0x00ED02C1u)

/* Gear range supported by the S3 e-Shifter (two-speed automatic hub). */
#define SHIFTER_GEAR_MIN           (1u)
#define SHIFTER_GEAR_MAX           (2u)

/* High-level state of the shifter, as observed externally. */
typedef enum {
    SHIFTER_STATE_BOOT      = 0,
    SHIFTER_STATE_IDLE      = 1,
    SHIFTER_STATE_SHIFTING  = 2,
    SHIFTER_STATE_HOMING    = 3,
    SHIFTER_STATE_FAULT     = 0xFF,
} shifter_state_t;

/* Direction of an in-progress shift. */
typedef enum {
    SHIFT_DIR_NONE = 0,
    SHIFT_DIR_UP   = 1,
    SHIFT_DIR_DOWN = 2,
} shift_dir_t;

/* Linker-provided symbols (defined in linker_mm32f031.ld). */
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern uint32_t _estack;

/* Entry points called from startup_mm32f031.S */
void Reset_Handler(void);
void SystemInit(void);
int  main(void);

#endif /* SHIFTER_H */
