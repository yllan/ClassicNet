/*
 * TLS integration smoke test: real HTTPS GET over the full ClassicNet stack
 * (CNRequest -> CN_Tls -> HostTcp) against a local `openssl s_server -www`.
 * Driven by scripts/test-tls.sh.  Usage: tls_smoke <ip> <port>
 */
#include "classicnet/cn_request.h"
#include "classicnet/cn_tls.h"
#include "classicnet/cn_errors.h"
#include "host_tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int    completed;
    UInt16 status;
    UInt32 bodyLen;
    OSStatus result;
} Cap;

static void on_response(CNRequest *r, const CNHttpResponse *resp, void *ud)
{ Cap *c = (Cap *)ud; (void)r; c->status = resp->status; }
static Boolean on_data(CNRequest *r, const void *b, UInt32 len, void *ud)
{ Cap *c = (Cap *)ud; (void)r; (void)b; c->bodyLen += len; return true; }
static void on_complete(CNRequest *r, OSStatus res, void *ud)
{ Cap *c = (Cap *)ud; (void)r; c->completed = 1; c->result = res; }

/* Usage: tls_smoke <ip> <port> [ca-bundle.pem] [hostname]
   With a CA bundle, certificate verification is required (VERIFY_REQUIRED). */
int main(int argc, char **argv)
{
    const char *ip   = argc > 1 ? argv[1] : "127.0.0.1";
    UInt16      port = (UInt16)(argc > 2 ? atoi(argv[2]) : 14433);
    const char *caPath   = argc > 3 ? argv[3] : NULL;
    const char *hostname = argc > 4 ? argv[4] : "localhost";
    HostTcp tcp;
    CNTlsTransport tls;
    CNTransport *tcpT, *tlsT;
    CNRequest req;
    Cap cap;
    CNRequestCallbacks cb;
    int guard = 0;
    static char caBuf[32768];
    const char *caPem = NULL;
    UInt32 caLen = 0;

    if (caPath) {
        FILE *f = fopen(caPath, "rb");
        if (!f) { printf("FAIL: cannot open CA %s\n", caPath); return 1; }
        caLen = (UInt32)fread(caBuf, 1, sizeof(caBuf) - 1, f);
        fclose(f);
        caBuf[caLen] = '\0';
        caLen += 1;             /* mbedTLS PEM parse wants the NUL included */
        caPem = caBuf;
        printf("verify: REQUIRED (CA %s, host '%s')\n", caPath, hostname);
    } else {
        printf("verify: NONE (insecure)\n");
    }

    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_response; cb.onData = on_data; cb.onComplete = on_complete;

    if (HostTcpConnect(&tcp, ip, port, &tcpT) != noErr) {
        printf("FAIL: tcp connect\n"); return 1;
    }
    if (CN_TlsCreate(&tls, tcpT, hostname, caPem, caLen, &tlsT) != noErr) {
        printf("FAIL: tls create\n"); return 1;
    }
    if (CN_RequestStart(&req, tlsT, "GET", "/", hostname, 0, 0, &cb, &cap) != noErr) {
        printf("FAIL: request start\n"); return 1;
    }

    while (!CN_RequestDone(&req) && guard++ < 5000000)
        CN_RequestPump(&req);

    CN_TlsDispose(&tls);
    tcpT->close(tcpT);

    if (!cap.completed || cap.result != noErr) {
        printf("FAIL: result=%d status=%u (mbedTLS rc=%d -0x%04X)\n",
               (int)cap.result, cap.status, tls.lastError, (unsigned)(-tls.lastError));
        return 1;
    }
    if (cap.status != 200) {
        printf("FAIL: status=%u\n", cap.status);
        return 1;
    }
    printf("OK: HTTPS GET over TLS -> status %u, %u body bytes\n",
           cap.status, cap.bodyLen);
    return 0;
}
