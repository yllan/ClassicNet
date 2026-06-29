#include "classicnet/cn_request.h"
#include "classicnet/cn_errors.h"
#include "cn_ascii.h"

#include <string.h>

enum { ST_CONNECT, ST_SEND, ST_HEAD, ST_BODY, ST_DONE, ST_ERR };
enum { BM_LENGTH, BM_CHUNKED, BM_EOF };

static OSStatus parse_u32(const char *s, UInt32 *out)
{
    UInt32 v = 0;
    int any = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s >= '0' && *s <= '9') {
        if (v > 429496728u) return kCNErrBadHeader;   /* would overflow */
        v = v * 10 + (UInt32)(*s - '0');
        any = 1;
        s++;
    }
    while (*s == ' ' || *s == '\t') s++;
    if (!any || *s != '\0') return kCNErrBadHeader;    /* digits only (reject "5,6", "5x") */
    *out = v;
    return noErr;
}

/* RFC 7230 3.3.1: Transfer-Encoding is a comma list and chunked, if present,
   must be the final coding. Returns 1 if the last token is chunked, 0 if chunked
   is absent, -1 if chunked appears but is not last (framing we cannot use). */
static int te_chunked(const char *v)
{
    int sawChunked = 0, lastChunked = 0;
    UInt32 i = 0;
    while (v[i] != '\0') {
        UInt32 s, e;
        while (v[i] == ' ' || v[i] == '\t' || v[i] == ',') i++;   /* OWS + separators */
        if (v[i] == '\0') break;
        s = i;
        while (v[i] != '\0' && v[i] != ',') i++;                  /* one token */
        e = i;
        while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
        lastChunked = (e - s == 7) &&
            cn_ascii_lower(v[s])     == 'c' && cn_ascii_lower(v[s + 1]) == 'h' &&
            cn_ascii_lower(v[s + 2]) == 'u' && cn_ascii_lower(v[s + 3]) == 'n' &&
            cn_ascii_lower(v[s + 4]) == 'k' && cn_ascii_lower(v[s + 5]) == 'e' &&
            cn_ascii_lower(v[s + 6]) == 'd';
        if (lastChunked) sawChunked = 1;
    }
    if (sawChunked && !lastChunked) return -1;
    return lastChunked;
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
    r->isHead = (method != 0 && cn_ascii_ci_eq(method, "HEAD"));

    s = CN_BuildRequest(method, path, host, headers, headerCount,
                        r->buf, (UInt32)sizeof(r->buf), &r->sendLen);
    if (s != noErr) return s;

    r->sendOff = 0;
    r->state = ST_CONNECT;
    return noErr;
}

/* Sink for the streaming chunked decoder: hand decoded bytes straight to onData. */
static void req_chunk_sink(void *ctx, const char *bytes, UInt32 len)
{
    CNRequest *r = (CNRequest *)ctx;
    if (r->cb.onData) r->cb.onData(r, bytes, len, r->ud);
}

/* Classify the body framing once headers are known; deliver any already-buffered
   body bytes. Returns noErr (continue), or terminal via req_done/req_fail. */
static OSStatus enter_body(CNRequest *r)
{
    UInt32 i, avail;

    r->bodyOff = r->resp.bodyOffset;
    r->bodyMode = BM_EOF;
    r->contentRemaining = 0;

    {
        int teSeen = 0, teChunked = 0, haveCL = 0;
        UInt32 clVal = 0;
        for (i = 0; i < r->resp.headerCount; i++) {
            const char *n = r->resp.headers[i].name;
            const char *v = r->resp.headers[i].value;
            if (cn_ascii_ci_eq(n, "Transfer-Encoding")) {
                int tc;
                if (teChunked) return req_fail(r, kCNErrBadHeader);  /* chunked was not the final coding */
                tc = te_chunked(v);
                if (tc < 0) return req_fail(r, kCNErrBadHeader);     /* chunked present but not last */
                teSeen = 1;
                if (tc > 0) teChunked = 1;
            } else if (cn_ascii_ci_eq(n, "Content-Length")) {
                UInt32 cl;
                OSStatus ps = parse_u32(v, &cl);
                if (ps != noErr) return req_fail(r, ps);
                if (haveCL && cl != clVal) return req_fail(r, kCNErrBadHeader);  /* conflicting Content-Length */
                haveCL = 1; clVal = cl;
            }
        }
        /* RFC 7230 3.3.3: Transfer-Encoding wins over (and forbids) Content-Length;
           chunked frames the body regardless of header order. */
        if (teChunked) {
            r->bodyMode = BM_CHUNKED;
        } else if (teSeen) {
            r->bodyMode = BM_EOF;            /* TE present, non-chunked final coding: close-delimited */
        } else if (haveCL) {
            r->bodyMode = BM_LENGTH;
            r->contentRemaining = clVal;
        }
    }

    /* Responses that carry no body: HEAD, 1xx, 204, 304. (1xx is normally
       consumed earlier in the pump; this is a safety net.) */
    if (r->isHead || r->resp.status == 204 || r->resp.status == 304 ||
        (r->resp.status >= 100 && r->resp.status < 200))
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
    } else { /* BM_CHUNKED: feed the already-buffered body bytes to the decoder */
        UInt32 used;
        OSStatus cs;
        CN_ChunkedInit(&r->chunkDec);
        cs = CN_ChunkedFeed(&r->chunkDec, r->buf + r->bodyOff, avail, &used,
                            req_chunk_sink, r);
        if (cs == noErr) return req_done(r);
        if (cs != kCNErrChunkIncomplete) return req_fail(r, cs);
    }

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
            for (;;) {
                s = CN_ParseHttpResponse(r->buf, r->bufLen, &r->resp);
                if (s == kCNErrHeadersIncomplete) break;     /* need more bytes -> recv */
                if (s != noErr) return req_fail(r, s);
                if (r->resp.status >= 100 && r->resp.status < 200) {
                    /* interim response (100 Continue / 103 Early Hints): discard its
                       head and parse the next response already in the buffer. */
                    UInt32 consumed = r->resp.bodyOffset;
                    memmove(r->buf, r->buf + consumed, r->bufLen - consumed);
                    r->bufLen -= consumed;
                    continue;
                }
                break;                                       /* final response */
            }
            if (s == kCNErrHeadersIncomplete) break;         /* recv more */
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
                char tmp[1024];
                UInt32 used;
                OSStatus ds;
                s = r->t->recv(r->t, tmp, (UInt32)sizeof(tmp), &got, &eof);
                if (s != noErr) return req_fail(r, s);
                if (got > 0) {
                    ds = CN_ChunkedFeed(&r->chunkDec, tmp, got, &used, req_chunk_sink, r);
                    if (ds == noErr) return req_done(r);
                    if (ds != kCNErrChunkIncomplete) return req_fail(r, ds);
                } else if (eof) {
                    return req_fail(r, kCNErrConnClosed);
                } else {
                    return noErr;
                }
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
