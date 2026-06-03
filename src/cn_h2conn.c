#include "classicnet/cn_h2conn.h"
#include "classicnet/cn_h2.h"
#include "classicnet/cn_errors.h"

#include <string.h>

enum { ST_CONNECT, ST_SEND, ST_RECV, ST_DONE, ST_ERR };

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */
static OSStatus h2_fail(CNH2Conn *c, OSStatus e)
{
    c->result = e;
    c->state  = ST_ERR;
    if (c->cb.onComplete) c->cb.onComplete(c, e, c->ud);
    return e;
}

static OSStatus h2_finish(CNH2Conn *c)
{
    c->result = noErr;
    c->state  = ST_DONE;
    if (c->cb.onComplete) c->cb.onComplete(c, noErr, c->ud);
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

/* Queue a WINDOW_UPDATE (increment `inc`) for the given stream. */
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
        /* pseudo-header: we only need :status */
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
        return noErr;                                   /* silently cap extra headers */
    if (nameLen >= CN_HTTP_MAX_NAME || valueLen >= CN_HTTP_MAX_VALUE)
        return noErr;                                   /* skip oversized header */
    {
        CNHeaderField *f = &r->headers[r->headerCount++];
        memcpy(f->name, name, nameLen);   f->name[nameLen]   = 0;
        memcpy(f->value, value, valueLen); f->value[valueLen] = 0;
    }
    return noErr;
}

/* HEADERS/CONTINUATION fully reassembled: HPACK-decode and deliver. */
static OSStatus deliver_headers(CNH2Conn *c)
{
    OSStatus s;
    memset(&c->resp, 0, sizeof(c->resp));
    s = CN_HpackDecode(&c->hpack, c->hblk, c->hblkLen, collect_header, &c->resp);
    if (s != noErr) return s;
    c->hblkLen = 0;
    c->inHeaders = false;
    c->gotResponse = true;
    if (c->cb.onResponse) c->cb.onResponse(c, &c->resp, c->ud);
    return noErr;
}

/* ------------------------------------------------------------------ *
 * Process one complete frame at rbuf[rOff], length already validated present.
 * ------------------------------------------------------------------ */
static OSStatus process_frame(CNH2Conn *c, const CNH2FrameHeader *h,
                              const unsigned char *payload)
{
    switch (h->type) {

    case kCNH2Settings:
        if (h->flags & kCNH2FlagAck)
            return noErr;                               /* ack of ours: ignore */
        /* Acknowledge the peer's SETTINGS with an empty ACK. */
        {
            unsigned char ack[CN_H2_FRAME_HDR_LEN];
            UInt32 n = 0;
            CNH2FrameHeader a;
            a.length = 0; a.type = kCNH2Settings; a.flags = kCNH2FlagAck; a.streamId = 0;
            CN_H2WriteFrameHeader(&a, ack, sizeof(ack), &n);
            return h2_queue(c, ack, n);
        }

    case kCNH2Ping:
        if (h->flags & kCNH2FlagAck)
            return noErr;
        if (h->length != 8)
            return kCNErrH2BadFrame;
        {
            unsigned char pong[CN_H2_FRAME_HDR_LEN + 8];
            UInt32 n = 0;
            CN_H2BuildFrame(kCNH2Ping, kCNH2FlagAck, 0, payload, 8, pong, sizeof(pong), &n);
            return h2_queue(c, pong, n);
        }

    case kCNH2Goaway:
        return kCNErrH2StreamError;

    case kCNH2RstStream:
        if (h->streamId == c->streamId)
            return kCNErrH2StreamError;
        return noErr;

    case kCNH2WindowUpdate:
    case kCNH2Priority:
        return noErr;                                   /* nothing to do for a GET */

    case kCNH2Headers:
    case kCNH2Continuation: {
        const unsigned char *frag = payload;
        UInt32 fragLen = h->length;

        if (h->streamId != c->streamId)
            return noErr;                               /* not our stream */

        if (h->type == kCNH2Headers) {
            UInt32 padLen = 0;
            if (h->flags & kCNH2FlagPadded) {
                if (fragLen < 1) return kCNErrH2BadFrame;
                padLen = payload[0];
                frag += 1; fragLen -= 1;
            }
            if (h->flags & kCNH2FlagPriority) {
                if (fragLen < 5) return kCNErrH2BadFrame;
                frag += 5; fragLen -= 5;                /* skip dependency + weight */
            }
            if (padLen > fragLen) return kCNErrH2BadFrame;
            fragLen -= padLen;
            c->inHeaders = true;
        } else {
            if (!c->inHeaders) return kCNErrH2BadFrame;  /* CONTINUATION w/o HEADERS */
        }

        if (c->hblkLen + fragLen > CN_H2_HDRBLK_CAP)
            return kCNErrH2FrameTooLarge;
        if (fragLen) {
            memcpy(c->hblk + c->hblkLen, frag, fragLen);
            c->hblkLen += fragLen;
        }

        if (h->flags & kCNH2FlagEndHeaders) {
            OSStatus s = deliver_headers(c);
            if (s != noErr) return s;
        }
        if (h->flags & kCNH2FlagEndStream)
            c->streamClosed = true;
        return noErr;
    }

    case kCNH2Data: {
        const unsigned char *data = payload;
        UInt32 dataLen = h->length;
        UInt32 padLen = 0;

        if (h->streamId != c->streamId)
            return noErr;
        if (h->flags & kCNH2FlagPadded) {
            if (dataLen < 1) return kCNErrH2BadFrame;
            padLen = payload[0];
            data += 1; dataLen -= 1;
        }
        if (padLen > dataLen) return kCNErrH2BadFrame;
        dataLen -= padLen;

        if (dataLen && c->cb.onData)
            c->cb.onData(c, data, dataLen, c->ud);

        if (h->flags & kCNH2FlagEndStream) {
            c->streamClosed = true;                     /* no more data: skip window top-up */
        } else if (h->length) {
            /* Replenish both the connection and stream windows by the frame size
               (RFC 7540 §6.9) so the server can keep sending. */
            OSStatus s = h2_window_update(c, 0, h->length);
            if (s != noErr) return s;
            s = h2_window_update(c, c->streamId, h->length);
            if (s != noErr) return s;
        }
        return noErr;
    }

    default:
        return noErr;                                   /* unknown frame: ignore (§4.1) */
    }
}

/* ------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------ */
OSStatus CN_H2Get(CNH2Conn *c, CNTransport *t,
                  const char *scheme, const char *authority, const char *path,
                  const CNHeaderKV *headers, UInt32 headerCount,
                  const CNH2Callbacks *cb, void *ud)
{
    unsigned char hpackBuf[1024];
    unsigned char frame[CN_H2_FRAME_HDR_LEN + sizeof(hpackBuf)];
    UInt32 hlen = 0, flen = 0, i;
    OSStatus s;
    CNH2Setting settings[2];

    if (c == 0 || t == 0 || cb == 0 || scheme == 0 || authority == 0 || path == 0)
        return kCNErrBadParam;

    memset(c, 0, sizeof(*c));
    c->t = t;
    c->cb = *cb;
    c->ud = ud;
    c->streamId = 1;
    CN_HpackDecInit(&c->hpack, CN_HPACK_DYN_CAP);

    /* 1) connection preface */
    s = h2_queue(c, (const unsigned char *)CN_H2_PREFACE, CN_H2_PREFACE_LEN);
    if (s != noErr) return s;

    /* 2) our SETTINGS: disable server push, advertise our header-table size */
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

    /* 3) request HEADERS: :method :scheme :path :authority + extra headers */
    s = CN_HpackEncodeField(":method", 7, "GET", 3, hpackBuf, sizeof(hpackBuf), &hlen);
    if (s == noErr) s = CN_HpackEncodeField(":scheme", 7, scheme, (UInt32)strlen(scheme),
                                            hpackBuf, sizeof(hpackBuf), &hlen);
    if (s == noErr) s = CN_HpackEncodeField(":path", 5, path, (UInt32)strlen(path),
                                            hpackBuf, sizeof(hpackBuf), &hlen);
    if (s == noErr) s = CN_HpackEncodeField(":authority", 10, authority,
                                            (UInt32)strlen(authority),
                                            hpackBuf, sizeof(hpackBuf), &hlen);
    for (i = 0; s == noErr && i < headerCount; i++)
        s = CN_HpackEncodeField(headers[i].name, (UInt32)strlen(headers[i].name),
                                headers[i].value, (UInt32)strlen(headers[i].value),
                                hpackBuf, sizeof(hpackBuf), &hlen);
    if (s != noErr) return s;

    /* END_HEADERS | END_STREAM: a GET has no request body. */
    s = CN_H2BuildFrame(kCNH2Headers,
                        (UInt8)(kCNH2FlagEndHeaders | kCNH2FlagEndStream),
                        c->streamId, hpackBuf, hlen, frame, sizeof(frame), &flen);
    if (s != noErr) return s;
    s = h2_queue(c, frame, flen);
    if (s != noErr) return s;

    c->state = ST_CONNECT;
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

            /* Flush any queued control/flow frames first. */
            s = h2_flush(c);
            if (s == kCNErrWouldBlock) return noErr;
            if (s != noErr) return h2_fail(c, s);

            /* Process every complete frame already buffered. */
            for (;;) {
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
                    break;                               /* wait for the rest */

                ps = process_frame(c, &h, c->rbuf + c->rOff + CN_H2_FRAME_HDR_LEN);
                if (ps != noErr) return h2_fail(c, ps);
                c->rOff += frameLen;

                /* Re-flush any frames process_frame queued (ACK/WINDOW_UPDATE). */
                s = h2_flush(c);
                if (s == kCNErrWouldBlock) return noErr;
                if (s != noErr) return h2_fail(c, s);

                if (c->streamClosed && c->gotResponse)
                    return h2_finish(c);
            }

            /* Compact consumed bytes, then read more. */
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
                    if (c->streamClosed && c->gotResponse) return h2_finish(c);
                    return h2_fail(c, kCNErrConnClosed);
                }
                return noErr;                            /* would block */
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
