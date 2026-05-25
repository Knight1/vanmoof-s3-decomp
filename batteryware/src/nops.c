#include "batteryware.h"

/* Compiler-generated empty thunks / ROP call sites.
 * In the OEM binary these are link-time-eliminated or
 * serve as trampolines for exception dispatch.
 */
void nop_e774(void) { }
void nop_e784(void) { }
void nop_a6e0(void) { }
void nop_2ba6(void)
{
    /* Calls veneer_11f08(1) — sub-loop dispatch */
    extern void veneer_11f08(int arg);
    veneer_11f08(1);
}
void nop_2bac(void) { }
void nop_4764(void) { }
void nop_537c(void) { }
