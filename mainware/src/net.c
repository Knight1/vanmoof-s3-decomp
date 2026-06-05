#include <stdint.h>
#include <stdio.h>

#include "net.h"

/* Cloud/modem request builders (OEM 0x0802F8A0 / 0x0802F940). The host names and
 * the u-blox auth token are fixed strings in flash; they are read at their OEM
 * addresses to keep the reconstruction faithful (the OEM passes these literal
 * pointers straight to snprintf):
 *   0x08050890 = "bikecomm.vanmoof.com"
 *   0x080508C0 = "ublox1.vanmoof.com"
 *   0x080508D4 = "PBNjh0V46Eev8CcfS4LPJg"   (device auth token)
 */
#define HOST_BIKECOMM ((const char *)0x08050890u)
#define HOST_UBLOX    ((const char *)0x080508C0u)
#define UBLOX_TOKEN   ((const char *)0x080508D4u)

uint32_t bikecomm_request_build(net_req_t *req)
{
    int n = snprintf(req->buf, req->size, req->fmt, HOST_BIKECOMM);

    if (n <= 0 || (uint32_t)n >= req->size) {
        return 3;                 /* snprintf error or truncated */
    }
    return 0;
}

uint32_t ublox_request_build(net_req_t *req)
{
    /* fmt embeds the host twice and the token once. */
    int n = snprintf(req->buf, req->size, req->fmt,
                     HOST_UBLOX, HOST_UBLOX, UBLOX_TOKEN);

    if (n < 1 || (uint32_t)n >= req->size) {
        return 3;
    }
    return 0;
}
