#ifndef MAINBOOT_STRING_H
#define MAINBOOT_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal C string utilities the Muco bootloader needs. We declare
 * these locally rather than pulling in <string.h> from a hosted libc
 * because the build is `-nostdlib -nostdinc`-style and we want each
 * symbol's emitted code to come from this source tree.
 *
 * Where the prototypes match the standard C signatures (strlen,
 * memcpy, etc.), we keep the standard names so any inlining or
 * builtin recognition by GCC keeps working consistently — even
 * though `-fno-builtin` in the Makefile already disables that. */
size_t strlen(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* MAINBOOT_STRING_H */
