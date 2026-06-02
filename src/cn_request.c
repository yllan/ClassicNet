#include "classicnet/cn_request.h"
#include "classicnet/cn_errors.h"

#include <string.h>

enum { ST_CONNECT, ST_SEND, ST_HEAD, ST_BODY, ST_DONE, ST_ERR };
enum { BM_LENGTH, BM_CHUNKED, BM_EOF };

static char lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (lc(*a) != lc(*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* case-insensitive substring test */
static int ci_has(const char *hay, const char *needle)
{
    UInt32 i, j;
    for (i = 0; hay[i] != '\0'; i++) {
        for (j = 0; needle[j] != '\0'; j++) {
            if (lc(hay[i + j]) != lc(needle[j])) break;
        }
        if (needle[j] == '\0') return 1;
    }
    return 0;
}

static OSStatus parse_u32(const char *s, UInt32 *out)
{
    UInt32 v = 0;
    int any = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') {
        if (v > 429496728u) return kCNErrBadHeader;   /* would overflow */
        v = v * 10 + (UInt32)(*s - '0');
        any = 1;
        s++;
    }
    if (!any) return kCNErrBadHeader;
    *out = v;
    return noErr;
}

static OSStatus req_fail(CNRequest *r, OSStatus e)
{
    r->result = e;
    r->state = ST_ERR;
    if (r->cb.onComplete) r->cb.onComplete(r, e, r->ud);
    return e;
}

static OSStatus req_done(CNRequest *r)
{
    r->result = noErr;
    r->state = ST_DONE;
    if (r->cb.onComplete) r->cb.onComplete(r, noErr, r->ud);
    return noErr;
}

OSStatus CN_RequestStart(CNRequest *r, CNTransport *t,
                         const char *method, const char *path, const char *host,
                         const CNHeaderKV *headers, UInt32 headerCount,
                         const CNRequestCallbacks *cb, void *ud)
{
    OSStatus s;
    if (r == 0 || t == 0 || cb == 0)
        return kCNErrBadParam;

    memset(r, 0, sizeof(*r));
    r->t = t;
    r->ud = ud;
    r->cb = *cb;
    r->result = noErr;

    s = CN_BuildRequest(method, path, host, headers, headerCount,
                        r->buf, (UInt32)sizeof(r->buf), &r->sendLen);
    if (s != noErr) return s;

    r->sendOff = 0;
    r->state = ST_CONNECT;
    return noErr;
}

/* Classify the body framing once headers are known; deliver any already-buffered
   body bytes. Returns noErr (continue), or terminal via req_done/req_fail. */
static OSStatus enter_body(CNRequest *r)
{
    UInt32 i, avail;

    r->bodyOff = r->resp.bodyOffset;
    r->bodyMode = BM_EOF;
    r->contentRemaining = 0;

    for (i = 0; i < r->resp.headerCount; i++) {
        const char *n = r->resp.headers[i].name;
        const char *v = r->resp.headers[i].value;
        if (ci_eq(n, "Transfer-Encoding") && ci_has(v, "chunked")) {
            r->bodyMode = BM_CHUNKED;
        } else if (ci_eq(n, "Content-Length")) {
            UInt32 cl;
            OSStatus ps = parse_u32(v, &cl);
            if (ps != noErr) return req_fail(r, ps);
            r->bodyMode = BM_LENGTH;
            r->contentRemaining = cl;
        }
    }

    /* Responses that carry no body. */
    if (r->resp.status == 204 || r->resp.status == 304)
        return req_done(r);

    avail = r->bufLen - r->bodyOff;
    if (r->bodyMode == BM_LENGTH) {
        UInt32 give = avail < r->contentRemaining ? avail : r->contentRemaining;
        if (give > 0 && r->cb.onData)
            r->cb.onData(r, r->buf + r->bodyOff, give, r->ud);
        r->contentRemaining -= give;
        if (r->contentRemaining == 0)
            return req_done(r);
    } else if (r->bodyMode == BM_EOF) {
        if (avail > 0 && r->cb.onData)
            r->cb.onData(r, r->buf + r->bodyOff, avail, r->ud);
    }
    /* BM_CHUNKED keeps the bytes in buf for the decoder. */

    r->state = ST_BODY;
    return noErr;
}

OSStatus CN_RequestPump(CNRequest *r)
{
    if (r == 0) return kCNErrBadParam;

    for (;;) {
        switch (r->state) {

        case ST_CONNECT: {
            OSStatus s = r->t->poll(r->t);
            if (s == kCNErrWouldBlock) return noErr;
            if (s != noErr) return req_fail(r, s);
            r->state = ST_SEND;
            break;
        }

        case ST_SEND: {
            while (r->sendOff < r->sendLen) {
                UInt32 sent = 0;
                OSStatus s = r->t->send(r->t, r->buf + r->sendOff,
                                        r->sendLen - r->sendOff, &sent);
                if (s != noErr) return req_fail(r, s);
                if (sent == 0) return noErr;           /* would block */
                r->sendOff += sent;
            }
            r->bufLen = 0;                              /* reuse buf for the response */
            r->state = ST_HEAD;
            break;
        }

        case ST_HEAD: {
            UInt32 got = 0;
            Boolean eof = false;
            OSStatus s;
            if (r->bufLen >= sizeof(r->buf))
                return req_fail(r, kCNErrResponseTooLarge);
            s = r->t->recv(r->t, r->buf + r->bufLen,
                           (UInt32)sizeof(r->buf) - r->bufLen, &got, &eof);
            if (s != noErr) return req_fail(r, s);
            if (got == 0) {
                if (eof) return req_fail(r, kCNErrConnClosed);
                return noErr;                          /* would block */
            }
            r->bufLen += got;
            s = CN_ParseHttpResponse(r->buf, r->bufLen, &r->resp);
            if (s == kCNErrHeadersIncomplete) break;   /* recv more */
            if (s != noErr) return req_fail(r, s);
            if (r->cb.onResponse) r->cb.onResponse(r, &r->resp, r->ud);
            {
                OSStatus es = enter_body(r);
                if (r->state == ST_DONE || r->state == ST_ERR) return es;
            }
            break;
        }

        case ST_BODY: {
            OSStatus s;
            UInt32 got = 0;
            Boolean eof = false;

            if (r->bodyMode == BM_LENGTH) {
                char tmp[1024];
                UInt32 want = r->contentRemaining < sizeof(tmp)
                              ? r->contentRemaining : (UInt32)sizeof(tmp);
                s = r->t->recv(r->t, tmp, want, &got, &eof);
                if (s != noErr) return req_fail(r, s);
                if (got > 0) {
                    if (r->cb.onData) r->cb.onData(r, tmp, got, r->ud);
                    r->contentRemaining -= got;
                    if (r->contentRemaining == 0) return req_done(r);
                } else if (eof) {
                    return req_fail(r, kCNErrConnClosed);
                } else {
                    return noErr;
                }
            } else if (r->bodyMode == BM_CHUNKED) {
                char dec[CN_REQ_BUF];
                UInt32 dl = 0, cons = 0;
                OSStatus ds;
                if (r->bufLen < sizeof(r->buf)) {
                    s = r->t->recv(r->t, r->buf + r->bufLen,
                                   (UInt32)sizeof(r->buf) - r->bufLen, &got, &eof);
                    if (s != noErr) return req_fail(r, s);
                    r->bufLen += got;
                }
                ds = CN_DecodeChunked(r->buf + r->bodyOff, r->bufLen - r->bodyOff,
                                      dec, (UInt32)sizeof(dec), &dl, &cons);
                if (ds == noErr) {
                    if (dl > 0 && r->cb.onData) r->cb.onData(r, dec, dl, r->ud);
                    return req_done(r);
                }
                if (ds == kCNErrChunkIncomplete) {
                    if (got == 0) {
                        if (eof) return req_fail(r, kCNErrConnClosed);
                        if (r->bufLen >= sizeof(r->buf))
                            return req_fail(r, kCNErrResponseTooLarge);
                        return noErr;
                    }
                    break;                             /* got more; try again */
                }
                return req_fail(r, ds);
            } else { /* BM_EOF */
                char tmp[1024];
                s = r->t->recv(r->t, tmp, (UInt32)sizeof(tmp), &got, &eof);
                if (s != noErr) return req_fail(r, s);
                if (got > 0) {
                    if (r->cb.onData) r->cb.onData(r, tmp, got, r->ud);
                } else if (eof) {
                    return req_done(r);
                } else {
                    return noErr;
                }
            }
            break;
        }

        case ST_DONE:
        case ST_ERR:
            return r->result;

        default:
            return req_fail(r, kCNErrBadParam);
        }
    }
}

Boolean CN_RequestDone(const CNRequest *r)
{
    return (Boolean)(r->state == ST_DONE || r->state == ST_ERR);
}
