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

int main(int argc, char **argv)
{
    const char *ip   = argc > 1 ? argv[1] : "127.0.0.1";
    UInt16      port = (UInt16)(argc > 2 ? atoi(argv[2]) : 14433);
    HostTcp tcp;
    CNTlsTransport tls;
    CNTransport *tcpT, *tlsT;
    CNRequest req;
    Cap cap;
    CNRequestCallbacks cb;
    int guard = 0;

    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_response; cb.onData = on_data; cb.onComplete = on_complete;

    if (HostTcpConnect(&tcp, ip, port, &tcpT) != noErr) {
        printf("FAIL: tcp connect\n"); return 1;
    }
    /* INSECURE: self-signed test server, so no CA verification. */
    if (CN_TlsCreate(&tls, tcpT, "localhost", 0, 0, &tlsT) != noErr) {
        printf("FAIL: tls create\n"); return 1;
    }
    if (CN_RequestStart(&req, tlsT, "GET", "/", "localhost", 0, 0, &cb, &cap) != noErr) {
        printf("FAIL: request start\n"); return 1;
    }

    while (!CN_RequestDone(&req) && guard++ < 5000000)
        CN_RequestPump(&req);

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
    printf("OK: HTTPS GET over TLS -> status %u, %u body bytes\n",
           cap.status, cap.bodyLen);
    return 0;
}
