/*
 * On-target HTTP GET over Open Transport (L-D).
 * Fetches http://10.0.2.2:8080/ -- the QEMU user-net gateway maps 10.0.2.2 to
 * the host, so this hits a plain http.server running on the host (no external
 * DNS needed). Requires the OS 9 guest to have TCP/IP configured (DHCP).
 */
#include "classicnet/cn_ot.h"
#include "classicnet/cn_request.h"
#include "classicnet/cn_errors.h"

#include <Events.h>
#include <stdio.h>
#include <string.h>

typedef struct { int done; UInt16 status; UInt32 bodyLen; OSStatus result; } Cap;

static void on_resp(CNRequest *r, const CNHttpResponse *resp, void *ud)
{ Cap *c = (Cap *)ud; (void)r; c->status = resp->status; }
static Boolean on_data(CNRequest *r, const void *b, UInt32 l, void *ud)
{ Cap *c = (Cap *)ud; (void)r; (void)b; c->bodyLen += l; return true; }
static void on_done(CNRequest *r, OSStatus res, void *ud)
{ Cap *c = (Cap *)ud; (void)r; c->done = 1; c->result = res; }

int main(void)
{
    CNOTTransport ot;
    CNTransport *tr;
    CNRequest req;
    Cap cap;
    CNRequestCallbacks cb;
    UInt32 t0;
    const char *host = "10.0.2.2";
    UInt16 port = 8080;

    printf("ClassicNet: HTTP GET over Open Transport\r\n");

    if (CN_OTStartup() != noErr) { printf("OT startup failed\r\n"); goto wait; }
    if (CN_OTCreate(&ot, host, port, &tr) != noErr) { printf("OT create failed\r\n"); goto wait; }

    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_resp; cb.onData = on_data; cb.onComplete = on_done;

    if (CN_RequestStart(&req, tr, "GET", "/", host, 0, 0, &cb, &cap) != noErr) {
        printf("request start failed\r\n"); goto wait;
    }
    printf("connecting to %s:%u ...\r\n", host, (unsigned)port);

    t0 = TickCount();
    while (!CN_RequestDone(&req) && (TickCount() - t0) < 45UL * 60UL) /* 45 s */
        CN_RequestPump(&req);

    if (cap.done)
        printf("RESULT: result=%ld  status=%u  body=%lu bytes\r\n",
               (long)cap.result, (unsigned)cap.status, (unsigned long)cap.bodyLen);
    else
        printf("TIMEOUT waiting for response\r\n");

    CN_OTDispose(&ot);
    CN_OTShutdown();

wait:
    fflush(stdout);
    return 0;
}
