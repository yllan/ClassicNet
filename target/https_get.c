/*
 * On-target HTTPS GET (L-D, the real goal): TLS over Open Transport on OS 9.
 * Composes CN_Tls on top of CN_OT (both are CNTransport) and drives a CNRequest.
 * Fetches https://10.0.2.2:8443/ -- a host `openssl s_server -www` reached via
 * the QEMU user-net gateway. Self-signed, so verification is disabled here.
 * Reports the TLS handshake + fetch time in ticks (1/60 s).
 */
#include "classicnet/cn_ot.h"
#include "classicnet/cn_tls.h"
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
    CNTlsTransport tls;
    CNTransport *otT, *tlsT;
    CNRequest req;
    Cap cap;
    CNRequestCallbacks cb;
    UInt32 t0, t1;
    const char *host = "10.0.2.2";
    UInt16 port = 8443;

    printf("ClassicNet: HTTPS GET over TLS + Open Transport\r\n");

    if (CN_OTStartup() != noErr) { printf("OT startup failed\r\n"); goto wait; }
    if (CN_OTCreate(&ot, host, port, &otT) != noErr) { printf("OT create failed\r\n"); goto wait; }
    /* INSECURE: self-signed test server -> no certificate verification. */
    if (CN_TlsCreate(&tls, otT, "localhost", NULL, 0, &tlsT) != noErr) {
        printf("TLS create failed\r\n"); goto wait;
    }

    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_resp; cb.onData = on_data; cb.onComplete = on_done;
    if (CN_RequestStart(&req, tlsT, "GET", "/", host, 0, 0, &cb, &cap) != noErr) {
        printf("request start failed\r\n"); goto wait;
    }

    printf("TLS handshake + fetch to %s:%u ...\r\n", host, (unsigned)port);
    t0 = TickCount();
    while (!CN_RequestDone(&req) && (TickCount() - t0) < 45UL * 60UL) /* 45 s */
        CN_RequestPump(&req);
    t1 = TickCount();

    if (cap.done)
        printf("RESULT: result=%ld  status=%u  body=%lu bytes  (%lu ticks, ~%lu.%02lu s)\r\n",
               (long)cap.result, (unsigned)cap.status, (unsigned long)cap.bodyLen,
               (unsigned long)(t1 - t0),
               (unsigned long)((t1 - t0) / 60), (unsigned long)(((t1 - t0) % 60) * 100 / 60));
    else
        printf("TIMEOUT\r\n");

    if (!cap.done || cap.result != noErr)
        printf("  last mbedTLS rc = %d (-0x%04X)\r\n",
               tls.lastError, (unsigned)(-tls.lastError));

    CN_TlsDispose(&tls);
    CN_OTDispose(&ot);
    CN_OTShutdown();

wait:
    printf("\r\n--- Press Return to quit. ---\r\n");
    fflush(stdout);
    getchar();
    return 0;
}
