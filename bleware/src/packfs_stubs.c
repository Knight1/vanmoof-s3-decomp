/* packfs_stubs.c — PACK filesystem stubs.
 *
 * The PACK filesystem manages firmware asset bundles on external flash.
 * These stubs are no-ops until the packfs driver is fully decoded. */

#include <stdint.h>

void *packfs_open(void *params)  { (void)params; return (void *)0; }
int   packfs_next(void *h, void *e) { (void)h; (void)e; return 0; }
void  packfs_close(void *h)     { (void)h; }
