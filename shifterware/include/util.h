#ifndef SHIFTER_UTIL_H
#define SHIFTER_UTIL_H

#include <stddef.h>

/* Non-standard: returns void (the OEM implementation does not preserve
 * the original `dst` in r0 at exit). Do not rely on a return value. */
void memcpy(void *dst, const void *src, size_t n);

#endif /* SHIFTER_UTIL_H */
