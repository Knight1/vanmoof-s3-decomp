#ifndef MAINWARE_NET_H
#define MAINWARE_NET_H

#include <stdint.h>

/* Cloud / modem HTTP request-string builders. Each is handed a descriptor that
 * points at a destination buffer, its size, and a printf-style format string;
 * the builder formats the request (embedding the fixed VanMoof host name, and
 * for u-blox the device auth token) via snprintf. Returns 0 on success, 3 on
 * snprintf error or truncation.
 *
 * These feed the modem's AT+UHTTPC uplink: bikecomm.vanmoof.com is the backend
 * the bike reports to; ublox1.vanmoof.com is the u-blox cellular-module HTTP
 * endpoint. Part of the anti-theft tracking path (the modem subsystem). */
typedef struct {
    char       *buf;   /* +0x00  destination buffer */
    uint32_t    size;  /* +0x04  buffer size */
    const char *fmt;   /* +0x08  printf-style format */
} net_req_t;

/* OEM bikecomm_request_build at 0x0802F8A0 (fmt consumes one %s = host). */
uint32_t bikecomm_request_build(net_req_t *req);

/* OEM ublox_request_build at 0x0802F940 (fmt consumes host, host, token). */
uint32_t ublox_request_build(net_req_t *req);

#endif
