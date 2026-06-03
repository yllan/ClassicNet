/*
 * On-target HTTP/2 GET over TLS + Open Transport on OS 9 (C v2 step 3).
 * Stacks CN_H2Conn -> CN_Tls (ALPN "h2") -> CN_OT, all CNTransport, and runs a
 * real h2 request against a host h2 server reached via the QEMU user-net gateway
 * (https://10.0.2.2:8444/). Reports the negotiated ALPN, status, body size, and
 * the handshake+fetch time in ticks (1/60 s).
 */
#include "classicnet/cn_ot.h"
#include "classicnet/cn_tls.h"
#include "classicnet/cn_h2conn.h"
#include "classicnet/cn_errors.h"
#include "cn_mac_time.h"   /* cn_collect_jitter */

#include <Events.h>
#include <stdio.h>
#include <string.h>

#include "cn_ca.h"   /* CN_CA / CN_CA_LEN / CN_CA_DESC from -DCN_CA_BUNDLE / -DCN_VERIFY */

typedef struct { int done; UInt16 status; UInt32 bodyLen; OSStatus result; } Cap;

static void on_resp(CNH2Conn *c, const CNH2Response *resp, void *ud)
{ Cap *cap = (Cap *)ud; (void)c; cap->status = resp->status; }
static Boolean on_data(CNH2Conn *c, const void *b, UInt32 l, void *ud)
{ Cap *cap = (Cap *)ud; (void)c; (void)b; cap->bodyLen += l; return true; }
static void on_done(CNH2Conn *c, OSStatus res, void *ud)
{ Cap *cap = (Cap *)ud; (void)c; cap->done = 1; cap->result = res; }

int main(void)
{
    CNOTTransport ot;
    CNTlsTransport tls;
    CNTransport *otT, *tlsT;
    CNH2Conn conn;
    Cap cap;
    CNH2Callbacks cb;
    const char *neg;
    UInt32 t0, t1;
    const char *host = "10.0.2.2";
    UInt16 port = 8444;
    OSStatus s;

    printf("ClassicNet: HTTP/2 GET over TLS + Open Transport\r\n");

    if (CN_OTStartup() != noErr) { printf("OT startup failed\r\n"); goto wait; }
    if (CN_OTCreate(&ot, host, port, &otT) != noErr) { printf("OT create failed\r\n"); goto wait; }

#if defined(CN_CA) /* CN_CA_BUNDLE or CN_VERIFY */
    printf("verify: REQUIRED (%s, host 'localhost')\r\n", CN_CA_DESC);
    if (CN_TlsCreate(&tls, otT, "localhost", CN_CA, CN_CA_LEN, &tlsT) != noErr) {
        printf("TLS create failed\r\n"); goto wait;
    }
#else
    printf("verify: NONE (insecure)\r\n");
    if (CN_TlsCreate(&tls, otT, "localhost", NULL, 0, &tlsT) != noErr) {
        printf("TLS create failed\r\n"); goto wait;
    }
#endif
    if (CN_TlsSetAlpn(&tls, "h2", 0) != noErr) { printf("set alpn failed\r\n"); goto wait; }
    { unsigned char j[32]; cn_collect_jitter(j, sizeof(j)); CN_TlsAddEntropy(&tls, j, sizeof(j)); }

    printf("TLS handshake (ALPN h2) to %s:%u ...\r\n", host, (unsigned)port);
    t0 = TickCount();

    /* Drive the handshake to completion so ALPN is known before the h2 exchange. */
    do { s = tlsT->poll(tlsT); } while (s == kCNErrWouldBlock && (TickCount() - t0) < 45UL * 60UL);
    if (s != noErr) {
        printf("handshake failed: rc=%ld (mbedTLS %d -0x%04X)\r\n",
               (long)s, tls.lastError, (unsigned)(-tls.lastError));
        goto wait;
    }
    neg = CN_TlsGetAlpn(&tls);
    printf("ALPN: %s\r\n", neg ? neg : "(none)");
    if (neg == NULL || strcmp(neg, "h2") != 0) {
        printf("server did not negotiate h2; aborting\r\n"); goto wait;
    }

    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_resp; cb.onData = on_data; cb.onComplete = on_done;
    if (CN_H2Get(&conn, tlsT, "https", host, "/", 0, 0, &cb, &cap) != noErr) {
        printf("h2 get start failed\r\n"); goto wait;
    }
    while (!CN_H2Done(&conn) && (TickCount() - t0) < 45UL * 60UL)
        CN_H2Pump(&conn);
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
    fflush(stdout);
#ifndef CN_NO_PAUSE
    /* Keep the console window open when double-clicked from a CD/Finder. Under
       LaunchAPPL (push-to-run) this pause jams the server, so headless builds
       (-DCN_NO_PAUSE, set by the CN_LAUNCHAPPL CMake option) skip it and exit. */
    printf("\r\n--- Press Return to quit. ---\r\n");
    fflush(stdout);
    getchar();
#endif
    return 0;
}
