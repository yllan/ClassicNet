#include "classicnet/cn_h2conn.h"
#include "classicnet/cn_h2.h"
#include "classicnet/cn_errors.h"

#include <string.h>

#ifdef CN_HOST
/* Host-only diagnostic frame trace, enabled by setting CN_H2_TRACE in the env.
 * Never compiled into the Mac target. */
#include <stdio.h>
#include <stdlib.h>
static int cn_h2_trace_on(void) {
    static int v = -1;
    if (v < 0) v = getenv("CN_H2_TRACE") ? 1 : 0;
    return v;
}
#endif

enum { ST_CONNECT, ST_SEND, ST_RECV, ST_DONE, ST_ERR };

/* ------------------------------------------------------------------ *
 * Streams
 * ------------------------------------------------------------------ */
static CNH2Stream *find_stream(CNH2Conn *c, UInt32 id)
{
    UInt32 i;
    if (id == 0) return 0;
    for (i = 0; i < CN_H2_MAX_STREAMS; i++)
        if (c->streams[i].id == id && !c->streams[i].done)
            return &c->streams[i];
    return 0;
}

static void stream_complete(CNH2Conn *c, CNH2Stream *st, OSStatus res)
{
    if (st == 0 || st->done) return;
    st->done = true;
    if (c->openCount) c->openCount--;
    if (st->cb.onComplete) st->cb.onComplete(c, res, st->ud);
    st->id = 0;                                   /* free the slot; late frames ignored */
}

static void maybe_complete(CNH2Conn *c, CNH2Stream *st)
{
    if (st && !st->done && st->closed && st->gotResponse)
        stream_complete(c, st, noErr);
}

/* ------------------------------------------------------------------ *
 * Connection-level helpers
 * ------------------------------------------------------------------ */
static OSStatus h2_fail(CNH2Conn *c, OSStatus e)
{
    UInt32 i;
    c->result = e;
    c->state  = ST_ERR;
    for (i = 0; i < CN_H2_MAX_STREAMS; i++) {     /* fail every still-open stream */
        CNH2Stream *st = &c->streams[i];
        if (st->id && !st->done) {
            st->done = true;
            if (st->cb.onComplete) st->cb.onComplete(c, e, st->ud);
        }
    }
    c->openCount = 0;
    return e;
}

static OSStatus h2_finish(CNH2Conn *c)
{
    c->result = noErr;
    c->state  = ST_DONE;                          /* streams already fired onComplete */
    return noErr;
}

/* Append bytes to the outbound queue, compacting already-sent bytes first. */
static OSStatus h2_queue(CNH2Conn *c, const unsigned char *p, UInt32 n)
{
    if (c->outOff > 0) {
        memmove(c->out, c->out + c->outOff, c->outLen - c->outOff);
        c->outLen -= c->outOff;
        c->outOff  = 0;
    }
    if (c->outLen + n > CN_H2_OUT_CAP)
        return kCNErrBufferOverflow;
    memcpy(c->out + c->outLen, p, n);
    c->outLen += n;
    return noErr;
}

/* Try to flush the outbound queue. noErr = fully flushed; kCNErrWouldBlock =
   partial (call again); other = transport error. */
static OSStatus h2_flush(CNH2Conn *c)
{
    while (c->outOff < c->outLen) {
        UInt32 sent = 0;
        OSStatus s = c->t->send(c->t, c->out + c->outOff, c->outLen - c->outOff, &sent);
        if (s != noErr) return s;
        if (sent == 0) return kCNErrWouldBlock;
        c->outOff += sent;
    }
    c->outOff = c->outLen = 0;
    return noErr;
}

static OSStatus h2_window_update(CNH2Conn *c, UInt32 streamId, UInt32 inc)
{
    unsigned char frame[CN_H2_FRAME_HDR_LEN + 4];
    unsigned char payload[4];
    UInt32 n = 0;
    OSStatus s;
    payload[0] = (unsigned char)((inc >> 24) & 0x7F);
    payload[1] = (unsigned char)((inc >> 16) & 0xFF);
    payload[2] = (unsigned char)((inc >>  8) & 0xFF);
    payload[3] = (unsigned char)( inc        & 0xFF);
    s = CN_H2BuildFrame(kCNH2WindowUpdate, 0, streamId, payload, 4, frame, sizeof(frame), &n);
    if (s != noErr) return s;
    return h2_queue(c, frame, n);
}

/* ------------------------------------------------------------------ *
 * HPACK sink: build the response (status + headers) from decoded fields.
 * ------------------------------------------------------------------ */
static OSStatus collect_header(void *ctx, const char *name, UInt32 nameLen,
                               const char *value, UInt32 valueLen)
{
    CNH2Response *r = (CNH2Response *)ctx;

    if (nameLen >= 1 && name[0] == ':') {
        if (nameLen == 7 && memcmp(name, ":status", 7) == 0) {
            UInt32 i, v = 0;
            for (i = 0; i < valueLen; i++) {
                if (value[i] < '0' || value[i] > '9') return kCNErrH2BadFrame;
                v = v * 10u + (UInt32)(value[i] - '0');
            }
            r->status = (UInt16)v;
        }
        return noErr;
    }
    if (r->headerCount >= CN_HTTP_MAX_HEADERS)
        return noErr;
    if (nameLen >= CN_HTTP_MAX_NAME || valueLen >= CN_HTTP_MAX_VALUE)
        return noErr;
    {
        CNHeaderField *f = &r->headers[r->headerCount++];
        memcpy(f->name, name, nameLen);   f->name[nameLen]   = 0;
        memcpy(f->value, value, valueLen); f->value[valueLen] = 0;
    }
    return noErr;
}

/* A header block is complete: HPACK-decode it (always, to keep the dynamic
   table in sync) and deliver to the owning stream if one is still open. */
static OSStatus deliver_headers(CNH2Conn *c)
{
    OSStatus s;
    CNH2Stream *st;
    memset(&c->resp, 0, sizeof(c->resp));
    s = CN_HpackDecode(&c->hpack, c->hblk, c->hblkLen, collect_header, &c->resp);
    c->hblkLen   = 0;
    c->inHeaders = false;
    if (s != noErr) return s;                     /* HPACK error == connection error */
    st = find_stream(c, c->hdrStream);
    if (st) {
        st->gotResponse = true;
        if (st->cb.onResponse) st->cb.onResponse(c, &c->resp, st->ud);
    }
    return noErr;
}

/* ------------------------------------------------------------------ *
 * Process one complete frame at payload (length validated present).
 * ------------------------------------------------------------------ */
static OSStatus process_frame(CNH2Conn *c, const CNH2FrameHeader *h,
                              const unsigned char *payload)
{
#ifdef CN_HOST
    if (cn_h2_trace_on())
        fprintf(stderr, "[h2 recv] type=%u flags=0x%02x stream=%lu len=%lu\n",
                (unsigned)h->type, (unsigned)h->flags,
                (unsigned long)h->streamId, (unsigned long)h->length);
#endif
    switch (h->type) {

    case kCNH2Settings:
        if (h->streamId != 0) return kCNErrH2BadFrame;          /* RFC 7540 §6.5: stream 0 only */
        if (h->flags & kCNH2FlagAck)
            return (h->length == 0) ? noErr : kCNErrH2BadFrame; /* §6.5: ACK carries no payload */
        if (h->length % 6 != 0) return kCNErrH2BadFrame;        /* §6.5: payload is a multiple of 6 */
        {   UInt32 i;                                            /* learn the peer's flow-control limits */
            for (i = 0; i + 6 <= h->length; i += 6) {
                UInt16 sid = (UInt16)(((UInt32)payload[i] << 8) | payload[i + 1]);
                UInt32 val = ((UInt32)payload[i + 2] << 24) | ((UInt32)payload[i + 3] << 16)
                           | ((UInt32)payload[i + 4] << 8)  |  (UInt32)payload[i + 5];
                if (sid == 4) {                                  /* SETTINGS_INITIAL_WINDOW_SIZE */
                    SInt32 delta;
                    UInt32 k;
                    if (val > 0x7FFFFFFFu) return kCNErrH2BadFrame;  /* §6.5.2: FLOW_CONTROL_ERROR */
                    delta = (SInt32)val - (SInt32)c->peerInitWindow;
                    c->peerInitWindow = val;
                    for (k = 0; k < CN_H2_MAX_STREAMS; k++)      /* RFC 7540 6.9.2: adjust open streams */
                        if (c->streams[k].id) c->streams[k].sendWindow += delta;
                } else if (sid == 5) {                           /* SETTINGS_MAX_FRAME_SIZE */
                    if (val < 16384u || val > 16777215u) return kCNErrH2BadFrame;  /* §6.5.2: PROTOCOL_ERROR */
                    c->peerMaxFrame = val;
                }
            }
        }
        {
            unsigned char ack[CN_H2_FRAME_HDR_LEN];
            UInt32 n = 0;
            CNH2FrameHeader a;
            a.length = 0; a.type = kCNH2Settings; a.flags = kCNH2FlagAck; a.streamId = 0;
            CN_H2WriteFrameHeader(&a, ack, sizeof(ack), &n);
            return h2_queue(c, ack, n);
        }

    case kCNH2Ping:
        if (h->streamId != 0) return kCNErrH2BadFrame;   /* §6.7: stream 0 only */
        if (h->length != 8)   return kCNErrH2BadFrame;   /* §6.7: length 8, ACK included */
        if (h->flags & kCNH2FlagAck)
            return noErr;
        {
            unsigned char pong[CN_H2_FRAME_HDR_LEN + 8];
            UInt32 n = 0;
            CN_H2BuildFrame(kCNH2Ping, kCNH2FlagAck, 0, payload, 8, pong, sizeof(pong), &n);
            return h2_queue(c, pong, n);
        }

    case kCNH2Goaway:
#ifdef CN_HOST
        if (cn_h2_trace_on() && h->length >= 8)
            fprintf(stderr, "[h2 recv] GOAWAY lastStream=%lu errorCode=%lu\n",
                    (unsigned long)(((UInt32)payload[0] << 24) | ((UInt32)payload[1] << 16)
                                    | ((UInt32)payload[2] << 8) | payload[3]),
                    (unsigned long)(((UInt32)payload[4] << 24) | ((UInt32)payload[5] << 16)
                                    | ((UInt32)payload[6] << 8) | payload[7]));
#endif
        return kCNErrH2StreamError;               /* terminal for the whole connection */

    case kCNH2RstStream:
#ifdef CN_HOST
        if (cn_h2_trace_on() && h->length >= 4)
            fprintf(stderr, "[h2 recv] RST_STREAM stream=%lu errorCode=%lu\n",
                    (unsigned long)h->streamId,
                    (unsigned long)(((UInt32)payload[0] << 24) | ((UInt32)payload[1] << 16)
                                    | ((UInt32)payload[2] << 8) | payload[3]));
#endif
        stream_complete(c, find_stream(c, h->streamId), kCNErrH2StreamError);
        return noErr;                             /* other streams continue */

    case kCNH2WindowUpdate: {
        UInt32 inc;
        if (h->length != 4) return kCNErrH2BadFrame;     /* §6.9: FRAME_SIZE_ERROR */
        inc = (((UInt32)payload[0] << 24) | ((UInt32)payload[1] << 16)
             | ((UInt32)payload[2] <<  8) |  (UInt32)payload[3]) & 0x7FFFFFFFu;
        if (inc == 0) {                                  /* §6.9: a zero increment is an error */
            if (h->streamId == 0) return kCNErrH2BadFrame;
            stream_complete(c, find_stream(c, h->streamId), kCNErrH2StreamError);
            return noErr;
        }
        if (h->streamId == 0) {                          /* §6.9.1: window must not exceed 2^31-1 */
            if (c->connSendWindow > 0 && inc > (UInt32)(0x7FFFFFFF - c->connSendWindow))
                return kCNErrH2BadFrame;
            c->connSendWindow += (SInt32)inc;            /* credit the connection send window */
        } else {
            CNH2Stream *st = find_stream(c, h->streamId);
            if (st) {
                if (st->sendWindow > 0 && inc > (UInt32)(0x7FFFFFFF - st->sendWindow)) {
                    stream_complete(c, st, kCNErrH2StreamError);   /* §6.9.1: stream FLOW_CONTROL_ERROR */
                    return noErr;
                }
                st->sendWindow += (SInt32)inc;           /* credit the stream send window */
            }
        }
        return noErr;
    }
    case kCNH2Priority:
        return noErr;

    case kCNH2Headers:
    case kCNH2Continuation: {
        const unsigned char *frag = payload;
        UInt32 fragLen = h->length;
        CNH2Stream *st;

        if (h->type == kCNH2Headers) {
            UInt32 padLen = 0;
            if (h->flags & kCNH2FlagPadded) {
                if (fragLen < 1) return kCNErrH2BadFrame;
                padLen = payload[0];
                frag += 1; fragLen -= 1;
            }
            if (h->flags & kCNH2FlagPriority) {
                if (fragLen < 5) return kCNErrH2BadFrame;
                frag += 5; fragLen -= 5;
            }
            if (padLen > fragLen) return kCNErrH2BadFrame;
            fragLen -= padLen;
            c->hdrStream = h->streamId;
            c->hblkLen   = 0;
            c->inHeaders = true;
        } else {
            if (!c->inHeaders || h->streamId != c->hdrStream)
                return kCNErrH2BadFrame;          /* CONTINUATION must not interleave */
        }

        if (c->hblkLen + fragLen > CN_H2_HDRBLK_CAP)
            return kCNErrH2FrameTooLarge;
        if (fragLen) {
            memcpy(c->hblk + c->hblkLen, frag, fragLen);
            c->hblkLen += fragLen;
        }

        st = find_stream(c, c->hdrStream);
        if (h->type == kCNH2Headers && (h->flags & kCNH2FlagEndStream) && st)
            st->closed = true;

        if (h->flags & kCNH2FlagEndHeaders) {
            OSStatus s = deliver_headers(c);
            if (s != noErr) return s;
            maybe_complete(c, find_stream(c, c->hdrStream));
        }
        return noErr;
    }

    case kCNH2Data: {
        const unsigned char *data = payload;
        UInt32 dataLen = h->length;
        UInt32 padLen = 0;
        CNH2Stream *st = find_stream(c, h->streamId);

        if (h->flags & kCNH2FlagPadded) {
            if (dataLen < 1) return kCNErrH2BadFrame;
            padLen = payload[0];
            data += 1; dataLen -= 1;
        }
        if (padLen > dataLen) return kCNErrH2BadFrame;
        dataLen -= padLen;

        if (st && dataLen && st->cb.onData)
            st->cb.onData(c, data, dataLen, st->ud);

        if (h->length) {
            /* Connection-level flow control covers EVERY DATA frame, including the
               one carrying END_STREAM (RFC 7540 §6.9). Skipping it leaks the
               connection window on each response's final frame until it reaches 0
               and ALL later transfers stall. The stream window only matters while
               the stream is still open. */
            OSStatus s = h2_window_update(c, 0, h->length);
            if (s != noErr) return s;
            if (st && !(h->flags & kCNH2FlagEndStream)) {
                s = h2_window_update(c, st->id, h->length);
                if (s != noErr) return s;
            }
        }
        if ((h->flags & kCNH2FlagEndStream) && st)
            st->closed = true;
        maybe_complete(c, st);
        return noErr;
    }

    default:
        return noErr;                             /* unknown frame: ignore (§4.1) */
    }
}

/* ------------------------------------------------------------------ *
 * Opening streams
 * ------------------------------------------------------------------ */
/* Format a UInt32 as decimal into out (max 10 digits); returns the length. */
static UInt32 u32_to_dec(UInt32 v, char *out)
{
    char tmp[10];
    UInt32 n = 0, i;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v != 0);
    for (i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

static OSStatus open_stream(CNH2Conn *c,
                            const char *method,
                            const char *scheme, const char *authority, const char *path,
                            const CNHeaderKV *headers, UInt32 headerCount,
                            const unsigned char *body, UInt32 bodyLen,
                            const CNH2Callbacks *cb, void *ud, UInt32 *streamId)
{
    unsigned char *hpackBuf = c->hdrScratch;   /* connection-owned: keep open_stream off the stack */
    unsigned char fhdr[CN_H2_FRAME_HDR_LEN]; UInt32 fhlen = 0;
    CNH2FrameHeader fh;
    UInt32 hlen = 0, i, id;
    UInt8  hflags;
    OSStatus s;
    CNH2Stream *slot = 0;

    if (cb == 0 || method == 0 || scheme == 0 || authority == 0 || path == 0)
        return kCNErrBadParam;
    if (bodyLen > 0 && body == 0)
        return kCNErrBadParam;

    for (i = 0; i < CN_H2_MAX_STREAMS; i++)
        if (c->streams[i].id == 0) { slot = &c->streams[i]; break; }
    if (slot == 0)
        return kCNErrH2StreamError;               /* no free stream slot */

    s = CN_HpackEncodeField(":method", 7, method, (UInt32)strlen(method),
                            hpackBuf, CN_H2_HENC_CAP, &hlen);
    if (s == noErr) s = CN_HpackEncodeField(":scheme", 7, scheme, (UInt32)strlen(scheme),
                                            hpackBuf, CN_H2_HENC_CAP, &hlen);
    if (s == noErr) s = CN_HpackEncodeField(":path", 5, path, (UInt32)strlen(path),
                                            hpackBuf, CN_H2_HENC_CAP, &hlen);
    if (s == noErr) s = CN_HpackEncodeField(":authority", 10, authority,
                                            (UInt32)strlen(authority),
                                            hpackBuf, CN_H2_HENC_CAP, &hlen);
    if (s == noErr && bodyLen > 0) {              /* content-length for the body */
        char clbuf[10];
        UInt32 cllen = u32_to_dec(bodyLen, clbuf);
        s = CN_HpackEncodeField("content-length", 14, clbuf, cllen,
                                hpackBuf, CN_H2_HENC_CAP, &hlen);
    }
    for (i = 0; s == noErr && i < headerCount; i++)
        s = CN_HpackEncodeField(headers[i].name, (UInt32)strlen(headers[i].name),
                                headers[i].value, (UInt32)strlen(headers[i].value),
                                hpackBuf, CN_H2_HENC_CAP, &hlen);
    if (s != noErr) return s;

    id = c->nextStreamId;
#ifdef CN_HOST
    if (cn_h2_trace_on()) {
        UInt32 k;
        fprintf(stderr, "[h2 send] %s %s (stream %lu)\n", method, path, (unsigned long)id);
        fprintf(stderr, "    :authority: %s\n", authority);
        for (k = 0; k < headerCount; k++)
            fprintf(stderr, "    %s: %s\n", headers[k].name, headers[k].value);
        if (bodyLen) fprintf(stderr, "    [body %lu bytes]\n", (unsigned long)bodyLen);
    }
#endif
    /* END_STREAM on HEADERS only when there is no body; otherwise the DATA
       frame below carries END_STREAM. */
    hflags = (UInt8)(kCNH2FlagEndHeaders | (bodyLen > 0 ? 0 : kCNH2FlagEndStream));
    fh.length = hlen; fh.type = (UInt8)kCNH2Headers; fh.flags = hflags; fh.streamId = id;
    s = CN_H2WriteFrameHeader(&fh, fhdr, sizeof(fhdr), &fhlen);
    if (s != noErr) return s;
    s = h2_queue(c, fhdr, fhlen);
    if (s != noErr) return s;
    s = h2_queue(c, hpackBuf, hlen);
    if (s != noErr) return s;


    c->nextStreamId += 2;
    slot->id = id;
    slot->gotResponse = false;
    slot->closed = false;
    slot->done = false;
    slot->cb = *cb;
    slot->ud = ud;
        slot->body       = (bodyLen > 0) ? body : 0;   /* sent in chunks by h2_send_bodies */
        slot->bodyLen    = bodyLen;
        slot->bodyOff    = 0;
        slot->sendWindow = (SInt32)c->peerInitWindow;
    c->openCount++;
    if (c->state == ST_DONE)                       /* reopen an idle connection */
        c->state = ST_RECV;
    if (streamId) *streamId = id;
    return noErr;
}

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */
OSStatus CN_H2ConnStart(CNH2Conn *c, CNTransport *t)
{
    OSStatus s;
    CNH2Setting settings[2];

    if (c == 0 || t == 0)
        return kCNErrBadParam;

    memset(c, 0, sizeof(*c));
    c->t = t;
    c->nextStreamId = 1;
    c->connSendWindow = 65535;            /* HTTP/2 default connection send window  */
    c->peerMaxFrame   = 16384;            /* HTTP/2 default max frame size           */
    c->peerInitWindow = 65535;            /* HTTP/2 default initial stream window     */
    CN_HpackDecInit(&c->hpack, CN_HPACK_DYN_CAP);

    s = h2_queue(c, (const unsigned char *)CN_H2_PREFACE, CN_H2_PREFACE_LEN);
    if (s != noErr) return s;

    settings[0].id = kCNH2SettingEnablePush;      settings[0].value = 0;
    settings[1].id = kCNH2SettingHeaderTableSize; settings[1].value = CN_HPACK_DYN_CAP;
    {
        unsigned char sf[CN_H2_FRAME_HDR_LEN + 12];
        UInt32 sn = 0;
        s = CN_H2BuildSettings(settings, 2, sf, sizeof(sf), &sn);
        if (s != noErr) return s;
        s = h2_queue(c, sf, sn);
        if (s != noErr) return s;
    }
    c->state = ST_CONNECT;
    return noErr;
}

OSStatus CN_H2Request(CNH2Conn *c,
                      const char *scheme, const char *authority, const char *path,
                      const CNHeaderKV *headers, UInt32 headerCount,
                      const CNH2Callbacks *cb, void *ud, UInt32 *streamId)
{
    if (c == 0) return kCNErrBadParam;
    if (c->state == ST_ERR) return c->result;
    return open_stream(c, "GET", scheme, authority, path, headers, headerCount,
                       0, 0, cb, ud, streamId);
}

OSStatus CN_H2Get(CNH2Conn *c, CNTransport *t,
                  const char *scheme, const char *authority, const char *path,
                  const CNHeaderKV *headers, UInt32 headerCount,
                  const CNH2Callbacks *cb, void *ud)
{
    OSStatus s = CN_H2ConnStart(c, t);
    if (s != noErr) return s;
    return open_stream(c, "GET", scheme, authority, path, headers, headerCount,
                       0, 0, cb, ud, 0);
}

OSStatus CN_H2RequestEx(CNH2Conn *c, const char *method,
                        const char *scheme, const char *authority, const char *path,
                        const CNHeaderKV *headers, UInt32 headerCount,
                        const void *body, UInt32 bodyLen,
                        const CNH2Callbacks *cb, void *ud, UInt32 *streamId)
{
    if (c == 0) return kCNErrBadParam;
    if (c->state == ST_ERR) return c->result;
    return open_stream(c, method, scheme, authority, path, headers, headerCount,
                       (const unsigned char *)body, bodyLen, cb, ud, streamId);
}

OSStatus CN_H2Post(CNH2Conn *c, CNTransport *t,
                   const char *scheme, const char *authority, const char *path,
                   const CNHeaderKV *headers, UInt32 headerCount,
                   const void *body, UInt32 bodyLen,
                   const CNH2Callbacks *cb, void *ud)
{
    OSStatus s = CN_H2ConnStart(c, t);
    if (s != noErr) return s;
    return open_stream(c, "POST", scheme, authority, path, headers, headerCount,
                       (const unsigned char *)body, bodyLen, cb, ud, 0);
}

/* Push as much pending request body as the flow-control windows and the out
 * buffer allow. DATA frames are capped at the peer's max frame size; the final
 * chunk carries END_STREAM. Driven from the pump after crediting WINDOW_UPDATEs,
 * so a >64KB body streams across pumps respecting the peer's window. */
static OSStatus h2_send_bodies(CNH2Conn *c)
{
    UInt32 i;
    for (i = 0; i < CN_H2_MAX_STREAMS; i++) {
        CNH2Stream *st = &c->streams[i];
        if (st->id == 0 || st->body == 0) continue;
        while (st->bodyOff < st->bodyLen) {
            UInt32 remain = st->bodyLen - st->bodyOff;
            SInt32 win = (c->connSendWindow < st->sendWindow) ? c->connSendWindow : st->sendWindow;
            UInt32 chunk, cap;
            unsigned char fh[CN_H2_FRAME_HDR_LEN]; UInt32 fn;
            CNH2FrameHeader dh; OSStatus s; Boolean last;

            if (win <= 0) break;                         /* flow control: await WINDOW_UPDATE */
            s = h2_flush(c);                             /* drain the queue before adding a frame */
            if (s == kCNErrWouldBlock) return noErr;
            if (s != noErr) return s;

            cap = CN_H2_OUT_CAP - CN_H2_FRAME_HDR_LEN;
            chunk = remain;
            if (chunk > c->peerMaxFrame) chunk = c->peerMaxFrame;
            if (chunk > cap)             chunk = cap;
            if (chunk > (UInt32)win)     chunk = (UInt32)win;

            last = (Boolean)(st->bodyOff + chunk >= st->bodyLen);
            dh.length = chunk; dh.type = (UInt8)kCNH2Data;
            dh.flags  = (UInt8)(last ? kCNH2FlagEndStream : 0); dh.streamId = st->id;
            s = CN_H2WriteFrameHeader(&dh, fh, sizeof(fh), &fn); if (s != noErr) return s;
            s = h2_queue(c, fh, fn);                            if (s != noErr) return s;
            s = h2_queue(c, st->body + st->bodyOff, chunk);     if (s != noErr) return s;

            st->bodyOff       += chunk;
            c->connSendWindow -= (SInt32)chunk;
            st->sendWindow    -= (SInt32)chunk;
            if (last) st->body = 0;                      /* fully queued; END_STREAM sent */

            s = h2_flush(c);
            if (s == kCNErrWouldBlock) return noErr;     /* queued; flushes next pump */
            if (s != noErr) return s;
        }
    }
    return noErr;
}

OSStatus CN_H2Pump(CNH2Conn *c)
{
    if (c == 0) return kCNErrBadParam;

    for (;;) {
        switch (c->state) {

        case ST_CONNECT: {
            OSStatus s = c->t->poll(c->t);
            if (s == kCNErrWouldBlock) return noErr;
            if (s != noErr) return h2_fail(c, s);
            c->state = ST_SEND;
            break;
        }

        case ST_SEND: {
            OSStatus s = h2_flush(c);
            if (s == kCNErrWouldBlock) return noErr;
            if (s != noErr) return h2_fail(c, s);
            c->state = ST_RECV;
            break;
        }

        case ST_RECV: {
            UInt32 got = 0;
            Boolean eof = false;
            OSStatus s;

            s = h2_flush(c);                       /* push queued HEADERS/control */
            if (s == kCNErrWouldBlock) return noErr;
            if (s != noErr) return h2_fail(c, s);

            for (;;) {                             /* drain complete buffered frames */
                CNH2FrameHeader h;
                UInt32 avail = c->rLen - c->rOff;
                UInt32 frameLen;
                OSStatus ps;

                if (avail < CN_H2_FRAME_HDR_LEN)
                    break;
                CN_H2ParseFrameHeader(c->rbuf + c->rOff, avail, &h);
                if (h.length > CN_H2_RECV_CAP - CN_H2_FRAME_HDR_LEN)
                    return h2_fail(c, kCNErrH2FrameTooLarge);
                frameLen = CN_H2_FRAME_HDR_LEN + h.length;
                if (avail < frameLen)
                    break;

                ps = process_frame(c, &h, c->rbuf + c->rOff + CN_H2_FRAME_HDR_LEN);
                if (ps != noErr) return h2_fail(c, ps);
                c->rOff += frameLen;

                s = h2_flush(c);                   /* flush ACK/WINDOW_UPDATE it queued */
                if (s == kCNErrWouldBlock) return noErr;
                if (s != noErr) return h2_fail(c, s);

                if (c->openCount == 0 && c->nextStreamId > 1)
                    return h2_finish(c);
            }

            if (c->openCount == 0 && c->nextStreamId > 1)
                return h2_finish(c);
            {   OSStatus se = h2_send_bodies(c);        /* push pending request bodies */
                if (se != noErr) return h2_fail(c, se);
            }

            if (c->rOff > 0) {
                memmove(c->rbuf, c->rbuf + c->rOff, c->rLen - c->rOff);
                c->rLen -= c->rOff;
                c->rOff  = 0;
            }
            if (c->rLen >= CN_H2_RECV_CAP)
                return h2_fail(c, kCNErrH2FrameTooLarge);

            s = c->t->recv(c->t, c->rbuf + c->rLen, CN_H2_RECV_CAP - c->rLen, &got, &eof);
            if (s != noErr) return h2_fail(c, s);
            if (got == 0) {
                if (eof) {
                    if (c->openCount == 0 && c->nextStreamId > 1) return h2_finish(c);
                    return h2_fail(c, kCNErrConnClosed);
                }
                return noErr;                       /* would block */
            }
            c->rLen += got;
            break;
        }

        case ST_DONE:
        case ST_ERR:
            return c->result;

        default:
            return h2_fail(c, kCNErrBadParam);
        }
    }
}

Boolean CN_H2Done(const CNH2Conn *c)
{
    return (Boolean)(c->state == ST_DONE || c->state == ST_ERR);
}
