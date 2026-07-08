#include "classicnet/cn_request.h"
#include "classicnet/cn_errors.h"
#include "cn_test.h"

#include <string.h>

/* --- a scripted, non-blocking fake transport ------------------------------ */

typedef struct {
    CNTransport base;
    const char *resp;
    UInt32      respLen, respOff;
    UInt32      dribble;       /* max bytes returned per recv (0 = unlimited) */
    int         connectPolls;  /* recv kCNErrWouldBlock this many times first */
    char        sent[2048];
    UInt32      sentLen;
} FakeT;

static OSStatus f_poll(CNTransport *t)
{
    FakeT *f = (FakeT *)t;
    if (f->connectPolls > 0) { f->connectPolls--; return kCNErrWouldBlock; }
    return noErr;
}

static OSStatus f_send(CNTransport *t, const void *d, UInt32 len, UInt32 *sent)
{
    FakeT *f = (FakeT *)t;
    UInt32 n = len;
    if (f->sentLen + n > sizeof(f->sent)) n = (UInt32)sizeof(f->sent) - f->sentLen;
    memcpy(f->sent + f->sentLen, d, n);
    f->sentLen += n;
    *sent = len;   /* accept everything */
    return noErr;
}

static OSStatus f_recv(CNTransport *t, void *b, UInt32 cap, UInt32 *got, Boolean *eof)
{
    FakeT *f = (FakeT *)t;
    UInt32 avail = f->respLen - f->respOff;
    UInt32 n = avail < cap ? avail : cap;
    if (f->dribble && n > f->dribble) n = f->dribble;
    if (n > 0) { memcpy(b, f->resp + f->respOff, n); f->respOff += n; }
    *got = n;
    *eof = (Boolean)(f->respOff >= f->respLen);
    return noErr;
}

static void f_close(CNTransport *t) { (void)t; }

static void fake_init(FakeT *f, const char *resp, UInt32 rl, UInt32 dribble, int cp)
{
    memset(f, 0, sizeof(*f));
    f->base.poll = f_poll;
    f->base.send = f_send;
    f->base.recv = f_recv;
    f->base.close = f_close;
    f->resp = resp; f->respLen = rl;
    f->dribble = dribble; f->connectPolls = cp;
}

/* --- capture client callbacks --------------------------------------------- */

typedef struct {
    int      gotResponse;
    UInt16   status;
    char     body[1024];
    UInt32   bodyLen;
    int      completed;
    OSStatus result;
} Cap;

static void on_response(CNRequest *r, const CNHttpResponse *resp, void *ud)
{
    Cap *c = (Cap *)ud; (void)r;
    c->gotResponse = 1;
    c->status = resp->status;
}
static Boolean on_data(CNRequest *r, const void *bytes, UInt32 len, void *ud)
{
    Cap *c = (Cap *)ud; (void)r;
    if (c->bodyLen + len <= sizeof(c->body)) {
        memcpy(c->body + c->bodyLen, bytes, len);
        c->bodyLen += len;
    }
    return true;
}
static void on_complete(CNRequest *r, OSStatus result, void *ud)
{
    Cap *c = (Cap *)ud; (void)r;
    c->completed = 1;
    c->result = result;
}

static void drive(CNRequest *r)
{
    int guard = 0;
    while (!CN_RequestDone(r) && guard++ < 1000000)
        CN_RequestPump(r);
}

static UInt32 append_str(char *buf, UInt32 i, const char *s)
{
    UInt32 n = (UInt32)strlen(s);
    memcpy(buf + i, s, n);
    return i + n;
}

/* --- tests ---------------------------------------------------------------- */

static void test_content_length_flow(void)
{
    const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 11\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "hello world";
    FakeT f;
    CNRequest req;
    Cap cap;
    CNRequestCallbacks cb;
    /* dribble 1 byte per recv + 2 connect polls: maximally incremental */
    fake_init(&f, resp, (UInt32)(sizeof(resp) - 1), 1, 2);
    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_response; cb.onData = on_data; cb.onComplete = on_complete;

    CN_CHECK(CN_RequestStart(&req, &f.base, "GET", "/data", "example.com",
                             0, 0, &cb, &cap) == noErr);
    drive(&req);

    CN_CHECK(cap.completed == 1);
    CN_CHECK(cap.result == noErr);
    CN_CHECK(cap.gotResponse == 1);
    CN_CHECK(cap.status == 200);
    CN_CHECK(cap.bodyLen == 11);
    CN_CHECK(memcmp(cap.body, "hello world", 11) == 0);

    /* the client actually sent a well-formed request */
    f.sent[f.sentLen] = '\0';
    CN_CHECK(strstr(f.sent, "GET /data HTTP/1.1\r\n") == f.sent);
    CN_CHECK(strstr(f.sent, "Host: example.com\r\n") != 0);
}

static void test_chunked_flow(void)
{
    const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    FakeT f;
    CNRequest req;
    Cap cap;
    CNRequestCallbacks cb;
    fake_init(&f, resp, (UInt32)(sizeof(resp) - 1), 3, 0);
    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_response; cb.onData = on_data; cb.onComplete = on_complete;

    CN_CHECK(CN_RequestStart(&req, &f.base, "GET", "/", "h.example",
                             0, 0, &cb, &cap) == noErr);
    drive(&req);

    CN_CHECK(cap.result == noErr);
    CN_CHECK(cap.status == 200);
    CN_CHECK(cap.bodyLen == 11);
    CN_CHECK(memcmp(cap.body, "hello world", 11) == 0);
}

static void test_204_no_body(void)
{
    const char resp[] = "HTTP/1.1 204 No Content\r\n\r\n";
    FakeT f;
    CNRequest req;
    Cap cap;
    CNRequestCallbacks cb;
    fake_init(&f, resp, (UInt32)(sizeof(resp) - 1), 0, 0);
    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_response; cb.onData = on_data; cb.onComplete = on_complete;

    CN_CHECK(CN_RequestStart(&req, &f.base, "GET", "/", "h", 0, 0, &cb, &cap) == noErr);
    drive(&req);

    CN_CHECK(cap.completed == 1);
    CN_CHECK(cap.result == noErr);
    CN_CHECK(cap.status == 204);
    CN_CHECK(cap.bodyLen == 0);
}

static void test_peer_closed_early(void)
{
    /* Content-Length promises 100 bytes but the peer closes after 4. */
    const char resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabcd";
    FakeT f;
    CNRequest req;
    Cap cap;
    CNRequestCallbacks cb;
    fake_init(&f, resp, (UInt32)(sizeof(resp) - 1), 0, 0);
    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_response; cb.onData = on_data; cb.onComplete = on_complete;

    CN_CHECK(CN_RequestStart(&req, &f.base, "GET", "/", "h", 0, 0, &cb, &cap) == noErr);
    drive(&req);

    CN_CHECK(cap.completed == 1);
    CN_CHECK(cap.result == kCNErrConnClosed);
}

/* Transfer-Encoding: chunked must win over Content-Length regardless of header
   order (here TE comes first, a later CL must NOT switch to length framing). */
static void test_chunked_wins_over_content_length(void)
{
    const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 999\r\n"
        "\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    FakeT f; CNRequest req; Cap cap; CNRequestCallbacks cb;
    fake_init(&f, resp, (UInt32)(sizeof(resp) - 1), 0, 0);
    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_response; cb.onData = on_data; cb.onComplete = on_complete;
    CN_CHECK(CN_RequestStart(&req, &f.base, "GET", "/", "h", 0, 0, &cb, &cap) == noErr);
    drive(&req);
    CN_CHECK(cap.completed == 1 && cap.result == noErr && cap.status == 200);
    CN_CHECK(cap.bodyLen == 5 && memcmp(cap.body, "hello", 5) == 0);
}

/* Two conflicting Content-Length values are a framing error. */
static void test_conflicting_content_length(void)
{
    const char resp[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello";
    FakeT f; CNRequest req; Cap cap; CNRequestCallbacks cb;
    fake_init(&f, resp, (UInt32)(sizeof(resp) - 1), 0, 0);
    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_response; cb.onData = on_data; cb.onComplete = on_complete;
    CN_CHECK(CN_RequestStart(&req, &f.base, "GET", "/", "h", 0, 0, &cb, &cap) == noErr);
    drive(&req);
    CN_CHECK(cap.completed == 1 && cap.result == kCNErrBadHeader);
}

static void test_late_content_length_after_header_cap(void)
{
    char resp[1024];
    UInt32 i = 0;
    int n;
    FakeT f; CNRequest req; Cap cap; CNRequestCallbacks cb;

    i = append_str(resp, i, "HTTP/1.1 200 OK\r\n");
    for (n = 0; n < CN_HTTP_MAX_HEADERS; n++)
        i = append_str(resp, i, "X-Ignored: y\r\n");
    i = append_str(resp, i, "Content-Length: 5\r\n\r\nhel");

    fake_init(&f, resp, i, 0, 0);
    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_response; cb.onData = on_data; cb.onComplete = on_complete;
    CN_CHECK(CN_RequestStart(&req, &f.base, "GET", "/", "h", 0, 0, &cb, &cap) == noErr);
    drive(&req);

    CN_CHECK(cap.completed == 1);
    CN_CHECK(cap.result == kCNErrConnClosed);
}

static void test_late_transfer_encoding_after_header_cap(void)
{
    char resp[1024];
    UInt32 i = 0;
    int n;
    FakeT f; CNRequest req; Cap cap; CNRequestCallbacks cb;

    i = append_str(resp, i, "HTTP/1.1 200 OK\r\n");
    for (n = 0; n < CN_HTTP_MAX_HEADERS; n++)
        i = append_str(resp, i, "X-Ignored: y\r\n");
    i = append_str(resp, i,
                   "Transfer-Encoding: chunked\r\n\r\n"
                   "5\r\nhello\r\n0\r\n\r\n");

    fake_init(&f, resp, i, 0, 0);
    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_response; cb.onData = on_data; cb.onComplete = on_complete;
    CN_CHECK(CN_RequestStart(&req, &f.base, "GET", "/", "h", 0, 0, &cb, &cap) == noErr);
    drive(&req);

    CN_CHECK(cap.completed == 1 && cap.result == noErr && cap.status == 200);
    CN_CHECK(cap.bodyLen == 5 && memcmp(cap.body, "hello", 5) == 0);
}

/* "notchunked" must NOT be treated as chunked (no substring false positive); it
   is an unknown coding, so the body is connection-delimited. */
static void test_transfer_encoding_not_chunked(void)
{
    const char resp[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: notchunked\r\n\r\nraw";
    FakeT f; CNRequest req; Cap cap; CNRequestCallbacks cb;
    fake_init(&f, resp, (UInt32)(sizeof(resp) - 1), 0, 0);
    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_response; cb.onData = on_data; cb.onComplete = on_complete;
    CN_CHECK(CN_RequestStart(&req, &f.base, "GET", "/", "h", 0, 0, &cb, &cap) == noErr);
    drive(&req);
    CN_CHECK(cap.completed == 1 && cap.result == noErr && cap.status == 200);
    CN_CHECK(cap.bodyLen == 3 && memcmp(cap.body, "raw", 3) == 0);
}

/* A HEAD response has no body even with Content-Length: complete after headers
   instead of hanging waiting for a body that never comes. */
static void test_head_no_body(void)
{
    const char resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n";
    FakeT f; CNRequest req; Cap cap; CNRequestCallbacks cb;
    fake_init(&f, resp, (UInt32)(sizeof(resp) - 1), 0, 0);
    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_response; cb.onData = on_data; cb.onComplete = on_complete;
    CN_CHECK(CN_RequestStart(&req, &f.base, "HEAD", "/", "h", 0, 0, &cb, &cap) == noErr);
    drive(&req);
    CN_CHECK(cap.completed == 1 && cap.result == noErr && cap.status == 200);
    CN_CHECK(cap.bodyLen == 0);
}

/* A 100 Continue interim response is skipped; the final response is delivered. */
static void test_100_continue_skipped(void)
{
    const char resp[] =
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";
    FakeT f; CNRequest req; Cap cap; CNRequestCallbacks cb;
    fake_init(&f, resp, (UInt32)(sizeof(resp) - 1), 0, 0);
    memset(&cap, 0, sizeof(cap));
    cb.onResponse = on_response; cb.onData = on_data; cb.onComplete = on_complete;
    CN_CHECK(CN_RequestStart(&req, &f.base, "GET", "/", "h", 0, 0, &cb, &cap) == noErr);
    drive(&req);
    CN_CHECK(cap.completed == 1 && cap.result == noErr && cap.status == 200);
    CN_CHECK(cap.bodyLen == 2 && memcmp(cap.body, "hi", 2) == 0);
}

int main(void)
{
    CN_RUN(test_content_length_flow);
    CN_RUN(test_chunked_flow);
    CN_RUN(test_204_no_body);
    CN_RUN(test_peer_closed_early);
    CN_RUN(test_chunked_wins_over_content_length);
    CN_RUN(test_conflicting_content_length);
    CN_RUN(test_late_content_length_after_header_cap);
    CN_RUN(test_late_transfer_encoding_after_header_cap);
    CN_RUN(test_transfer_encoding_not_chunked);
    CN_RUN(test_head_no_body);
    CN_RUN(test_100_continue_skipped);
    return CN_SUMMARY();
}
