/*
 * HTTP/2-over-TLS POST integration smoke test: drive CN_H2Post with a request
 * body through CN_Tls (ALPN "h2") and HostTcp against the local h2 server, and
 * verify the server echoed the exact body back (200 + matching bytes). This is
 * the host-side proof that the POST/DATA-frame path (needed for LINE Thrift
 * calls) works over a real TLS+h2 connection. Driven by scripts/test-h2-post.sh.
 *   usage: h2_post_smoke <ip> <port> [ca.pem] [host]
 */
#include "classicnet/cn_h2conn.h"
#include "classicnet/cn_tls.h"
#include "classicnet/cn_errors.h"
#include "host_tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A body with embedded NULs and high bytes -- like a Thrift TCompact payload --
   to prove the path is length-bounded, not C-string bound. */
static const unsigned char REQ_BODY[] = {
    0x82, 0x21, 0x00, 0x0b, 's', 'y', 'n', 'c',
    0x00, 0x01, 0x02, 0xff, 0xfe, 0x10, 0x00, 0x7f,
    'h', 'e', 'l', 'l', 'o', 0x00, 0x00, 0x42
};
#define REQ_BODY_LEN ((UInt32)sizeof(REQ_BODY))

typedef struct {
    int           completed;
    UInt16        status;
    unsigned char body[256];
    UInt32        bodyLen;
    OSStatus      result;
} Cap;

static void on_response(CNH2Conn *c, const CNH2Response *resp, void *ud)
{ Cap *cap = (Cap *)ud; (void)c; cap->status = resp->status; }
static Boolean on_data(CNH2Conn *c, const void *b, UInt32 len, void *ud)
{
    Cap *cap = (Cap *)ud; (void)c;
    if (cap->bodyLen + len <= sizeof(cap->body)) {
        memcpy(cap->body + cap->bodyLen, b, len);
        cap->bodyLen += len;
    }
    return true;
}
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
    static char caBuf[400000];
    const char *caPem = NULL;
    UInt32 caLen = 0;

    if (caPath) {
        FILE *f = fopen(caPath, "rb");
        if (!f) { printf("FAIL: cannot open CA %s\n", caPath); return 1; }
        caLen = (UInt32)fread(caBuf, 1, sizeof(caBuf) - 1, f);
        fclose(f);
        caBuf[caLen] = '\0';
        caLen += 1;
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

    if (CN_H2Post(&conn, tlsT, "https", hostname, "/api/v4/TalkService.do",
                  0, 0, REQ_BODY, REQ_BODY_LEN, &cb, &cap) != noErr) {
        printf("FAIL: h2 post start\n"); return 1;
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
    if (cap.bodyLen != REQ_BODY_LEN ||
        memcmp(cap.body, REQ_BODY, REQ_BODY_LEN) != 0) {
        printf("FAIL: echoed body mismatch (sent %u, got %u)\n",
               REQ_BODY_LEN, cap.bodyLen);
        return 1;
    }
    printf("OK: h2 POST over TLS -> status %u, %u body bytes echoed intact\n",
           cap.status, cap.bodyLen);
    return 0;
}
