/*
 * HTTP/2-over-TLS integration smoke test: the full client stack
 * (CN_H2Conn -> CN_Tls with ALPN "h2" -> HostTcp) against a local h2 server.
 * Verifies ALPN negotiated "h2" and that a real h2 GET returns 200 + body.
 * Driven by scripts/test-h2.sh.  Usage: h2_smoke <ip> <port> [ca.pem] [host]
 */
#include "classicnet/cn_h2conn.h"
#include "classicnet/cn_tls.h"
#include "classicnet/cn_errors.h"
#include "host_tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int      completed;
    UInt16   status;
    UInt32   bodyLen;
    OSStatus result;
} Cap;

static void on_response(CNH2Conn *c, const CNH2Response *resp, void *ud)
{ Cap *cap = (Cap *)ud; (void)c; cap->status = resp->status; }
static Boolean on_data(CNH2Conn *c, const void *b, UInt32 len, void *ud)
{ Cap *cap = (Cap *)ud; (void)c; (void)b; cap->bodyLen += len; return true; }
static void on_complete(CNH2Conn *c, OSStatus res, void *ud)
{ Cap *cap = (Cap *)ud; (void)c; cap->completed = 1; cap->result = res; }

int main(int argc, char **argv)
{
    const char *ip       = argc > 1 ? argv[1] : "127.0.0.1";
    UInt16      port     = (UInt16)(argc > 2 ? atoi(argv[2]) : 14533);
    const char *caPath   = argc > 3 ? argv[3] : NULL;
    const char *hostname = argc > 4 ? argv[4] : "localhost";
    HostTcp tcp;
    CNTlsTransport tls;
    CNTransport *tcpT, *tlsT;
    CNH2Conn conn;
    Cap cap;
    CNH2Callbacks cb;
    const char *neg;
    int guard = 0;
    static char caBuf[400000];   /* large enough for a full Mozilla CA bundle */
    const char *caPem = NULL;
    UInt32 caLen = 0;

    if (caPath) {
        FILE *f = fopen(caPath, "rb");
        if (!f) { printf("FAIL: cannot open CA %s\n", caPath); return 1; }
        caLen = (UInt32)fread(caBuf, 1, sizeof(caBuf) - 1, f);
        fclose(f);
        caBuf[caLen] = '\0';
        caLen += 1;                         /* mbedTLS PEM parse wants the NUL */
        caPem = caBuf;
    }

    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_response; cb.onData = on_data; cb.onComplete = on_complete;

    if (HostTcpConnect(&tcp, ip, port, &tcpT) != noErr) {
        printf("FAIL: tcp connect\n"); return 1;
    }
    if (CN_TlsCreate(&tls, tcpT, hostname, caPem, caLen, &tlsT) != noErr) {
        printf("FAIL: tls create\n"); return 1;
    }
    if (CN_TlsSetAlpn(&tls, "h2", 0) != noErr) {
        printf("FAIL: set alpn\n"); return 1;
    }
    /* Exercise the app-entropy hook (on a real Mac the app would collect mouse/
       key/timer jitter; here a token sample proves the reseed path works). */
    {
        unsigned char extra[16];
        UInt32 k;
        for (k = 0; k < sizeof(extra); k++) extra[k] = (unsigned char)(k * 37 + 11);
        if (CN_TlsAddEntropy(&tls, extra, sizeof(extra)) != noErr) {
            printf("FAIL: add entropy\n"); return 1;
        }
    }

    /* The TLS handshake must complete before ALPN is known, so drive poll until
       connected (or failed) before starting the h2 exchange. */
    {
        OSStatus s;
        do { s = tlsT->poll(tlsT); } while (s == kCNErrWouldBlock && guard++ < 5000000);
        if (s != noErr) {
            printf("FAIL: handshake (mbedTLS rc=%d -0x%04X)\n",
                   tls.lastError, (unsigned)(-tls.lastError));
            return 1;
        }
    }

    neg = CN_TlsGetAlpn(&tls);
    if (neg == NULL || strcmp(neg, "h2") != 0) {
        printf("FAIL: ALPN negotiated '%s' (expected h2)\n", neg ? neg : "(none)");
        return 1;
    }
    printf("ALPN: negotiated h2\n");

    if (CN_H2Get(&conn, tlsT, "https", hostname, "/", 0, 0, &cb, &cap) != noErr) {
        printf("FAIL: h2 get start\n"); return 1;
    }
    while (!CN_H2Done(&conn) && guard++ < 5000000)
        CN_H2Pump(&conn);

    CN_TlsDispose(&tls);
    tcpT->close(tcpT);

    if (!cap.completed || cap.result != noErr) {
        printf("FAIL: result=%d status=%u\n", (int)cap.result, cap.status);
        return 1;
    }
    if (cap.status != 200) {
        printf("FAIL: status=%u\n", cap.status);
        return 1;
    }
    printf("OK: h2 GET over TLS -> status %u, %u body bytes\n", cap.status, cap.bodyLen);
    return 0;
}
