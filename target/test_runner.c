/*
 * On-target (Classic Mac OS / PowerPC) smoke runner.
 *
 * Runs the portable protocol/crypto code on real big-endian PowerPC to catch
 * what host (little-endian, x86) tests cannot -- chiefly byte-order bugs in
 * SHA-1, Base64, the WebSocket length fields, and HTTP parsing.  Built as a
 * Retro68 CONSOLE app; prints a PASS/FAIL summary.
 */
#include "classicnet/cn_url.h"
#include "classicnet/cn_http.h"
#include "classicnet/cn_ws.h"
#include "classicnet/cn_sha1.h"
#include "classicnet/cn_base64.h"
#include "classicnet/cn_request.h"
#include "classicnet/cn_errors.h"
#include "cn_test.h"

#include <string.h>

static int sha1_is(const UInt8 *d, const char *hex)
{
    static const char *h = "0123456789abcdef";
    int i;
    for (i = 0; i < CN_SHA1_DIGEST_LEN; i++) {
        if (hex[i * 2] != h[(d[i] >> 4) & 0xF]) return 0;
        if (hex[i * 2 + 1] != h[d[i] & 0xF]) return 0;
    }
    return 1;
}

static void smoke_url(void)
{
    CNUrl u;
    CN_CHECK(CN_ParseURL("https://example.com:8443/p?q=1", &u) == noErr);
    CN_CHECK(u.scheme == kCNSchemeHTTPS && u.port == 8443);
    CN_CHECK(strcmp(u.host, "example.com") == 0);
    CN_CHECK(strcmp(u.path, "/p?q=1") == 0);
}

static void smoke_http(void)
{
    CNHttpResponse r;
    const char msg[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    CN_CHECK(CN_ParseHttpResponse(msg, (UInt32)(sizeof(msg) - 1), &r) == noErr);
    CN_CHECK(r.status == 200 && r.headerCount == 1);
}

static void smoke_ws_lengths(void)
{
    /* 16-bit and 64-bit length fields are the byte-order-sensitive paths. */
    CNWSFrame fr;
    const unsigned char f126[4] = { 0x82, 126, 0x01, 0x00 };           /* len 256 */
    const unsigned char f127[10] = { 0x82, 127, 0,0,0,0, 0,1,0,0 };    /* len 65536 */
    CN_CHECK(CN_WSParseFrame(f126, sizeof(f126), &fr) == noErr && fr.payloadLen == 256);
    CN_CHECK(CN_WSParseFrame(f127, sizeof(f127), &fr) == noErr && fr.payloadLen == 65536);
}

static void smoke_sha1(void)
{
    UInt8 d[CN_SHA1_DIGEST_LEN];
    CN_Sha1("abc", 3, d);
    CN_CHECK(sha1_is(d, "a9993e364706816aba3e25717850c26c9cd0d89d"));
}

static void smoke_base64(void)
{
    char enc[16];
    UInt32 ol;
    CN_CHECK(CN_Base64Encode("foobar", 6, enc, sizeof(enc), &ol) == noErr);
    CN_CHECK(strcmp(enc, "Zm9vYmFy") == 0);
}

static void smoke_ws_accept(void)
{
    char accept[29];
    CN_CHECK(CN_WSAcceptKey("dGhlIHNhbXBsZSBub25jZQ==", accept) == noErr);
    CN_CHECK(strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
}

/* --- minimal fake transport to drive the async state machine on-target --- */

typedef struct {
    CNTransport base;
    const char *resp;
    UInt32 len, off;
} FT;

static OSStatus ft_poll(CNTransport *t) { (void)t; return noErr; }
static OSStatus ft_send(CNTransport *t, const void *d, UInt32 l, UInt32 *s)
{ (void)t; (void)d; *s = l; return noErr; }
static OSStatus ft_recv(CNTransport *t, void *b, UInt32 cap, UInt32 *got, Boolean *eof)
{
    FT *f = (FT *)t;
    UInt32 n = f->len - f->off;
    if (n > cap) n = cap;
    if (n > 0) { memcpy(b, f->resp + f->off, n); f->off += n; }
    *got = n;
    *eof = (Boolean)(f->off >= f->len);
    return noErr;
}
static void ft_close(CNTransport *t) { (void)t; }

typedef struct { int done; UInt16 status; UInt32 bodyLen; OSStatus result; } Cap;
static void on_resp(CNRequest *r, const CNHttpResponse *resp, void *ud)
{ Cap *c = (Cap *)ud; (void)r; c->status = resp->status; }
static Boolean on_data(CNRequest *r, const void *b, UInt32 l, void *ud)
{ Cap *c = (Cap *)ud; (void)r; (void)b; c->bodyLen += l; return true; }
static void on_done(CNRequest *r, OSStatus res, void *ud)
{ Cap *c = (Cap *)ud; (void)r; c->done = 1; c->result = res; }

static void smoke_request_flow(void)
{
    static const char resp[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello world";
    FT f;
    CNRequest req;
    Cap cap;
    CNRequestCallbacks cb;
    int guard = 0;

    memset(&f, 0, sizeof(f));
    f.base.poll = ft_poll; f.base.send = ft_send; f.base.recv = ft_recv; f.base.close = ft_close;
    f.resp = resp; f.len = (UInt32)(sizeof(resp) - 1);
    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_resp; cb.onData = on_data; cb.onComplete = on_done;

    CN_CHECK(CN_RequestStart(&req, &f.base, "GET", "/", "h", 0, 0, &cb, &cap) == noErr);
    while (!CN_RequestDone(&req) && guard++ < 100000) CN_RequestPump(&req);
    CN_CHECK(cap.done && cap.result == noErr && cap.status == 200 && cap.bodyLen == 11);
}

int main(void)
{
    CN_RUN(smoke_url);
    CN_RUN(smoke_http);
    CN_RUN(smoke_ws_lengths);
    CN_RUN(smoke_sha1);
    CN_RUN(smoke_base64);
    CN_RUN(smoke_ws_accept);
    CN_RUN(smoke_request_flow);
    CN_SUMMARY();
    /* Keep the Retro68 console window open so the result is readable. */
    printf("\n--- Press Return to quit. ---\n");
    fflush(stdout);
    getchar();
    return cn_tests_failed == 0 ? 0 : 1;
}
