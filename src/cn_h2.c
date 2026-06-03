#include "classicnet/cn_h2.h"
#include "classicnet/cn_errors.h"

#include <string.h>

static void cn_h2_put24(unsigned char *p, UInt32 v)
{
    p[0] = (unsigned char)((v >> 16) & 0xFF);
    p[1] = (unsigned char)((v >>  8) & 0xFF);
    p[2] = (unsigned char)( v        & 0xFF);
}

static void cn_h2_put32(unsigned char *p, UInt32 v)
{
    p[0] = (unsigned char)((v >> 24) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >>  8) & 0xFF);
    p[3] = (unsigned char)( v        & 0xFF);
}

static UInt32 cn_h2_get32(const unsigned char *p)
{
    return ((UInt32)p[0] << 24) | ((UInt32)p[1] << 16) |
           ((UInt32)p[2] <<  8) |  (UInt32)p[3];
}

OSStatus CN_H2ParseFrameHeader(const unsigned char *buf, UInt32 len,
                               CNH2FrameHeader *out)
{
    if (buf == 0 || out == 0)
        return kCNErrBadParam;
    if (len < CN_H2_FRAME_HDR_LEN)
        return kCNErrH2FrameIncomplete;

    out->length   = ((UInt32)buf[0] << 16) | ((UInt32)buf[1] << 8) | (UInt32)buf[2];
    out->type     = buf[3];
    out->flags    = buf[4];
    out->streamId = cn_h2_get32(buf + 5) & 0x7FFFFFFFu;  /* drop reserved bit */
    return noErr;
}

OSStatus CN_H2WriteFrameHeader(const CNH2FrameHeader *h,
                               unsigned char *out, UInt32 outCap, UInt32 *outLen)
{
    if (h == 0 || out == 0)
        return kCNErrBadParam;
    if (h->length > CN_H2_MAX_FRAME_LEN)
        return kCNErrH2FrameTooLarge;
    if (outCap < CN_H2_FRAME_HDR_LEN)
        return kCNErrBufferOverflow;

    cn_h2_put24(out, h->length);
    out[3] = h->type;
    out[4] = h->flags;
    cn_h2_put32(out + 5, h->streamId & 0x7FFFFFFFu);
    if (outLen)
        *outLen = CN_H2_FRAME_HDR_LEN;
    return noErr;
}

OSStatus CN_H2BuildFrame(UInt8 type, UInt8 flags, UInt32 streamId,
                         const unsigned char *payload, UInt32 payloadLen,
                         unsigned char *out, UInt32 outCap, UInt32 *outLen)
{
    CNH2FrameHeader h;
    OSStatus err;

    if (out == 0 || (payload == 0 && payloadLen != 0))
        return kCNErrBadParam;
    if (payloadLen > CN_H2_MAX_FRAME_LEN)
        return kCNErrH2FrameTooLarge;
    if (outCap < CN_H2_FRAME_HDR_LEN + payloadLen)
        return kCNErrBufferOverflow;

    h.length   = payloadLen;
    h.type     = type;
    h.flags    = flags;
    h.streamId = streamId;
    err = CN_H2WriteFrameHeader(&h, out, outCap, 0);
    if (err != noErr)
        return err;

    if (payloadLen)
        memcpy(out + CN_H2_FRAME_HDR_LEN, payload, payloadLen);
    if (outLen)
        *outLen = CN_H2_FRAME_HDR_LEN + payloadLen;
    return noErr;
}

OSStatus CN_H2BuildSettings(const CNH2Setting *settings, UInt32 count,
                            unsigned char *out, UInt32 outCap, UInt32 *outLen)
{
    UInt32 payloadLen = count * 6u;
    UInt32 i;
    unsigned char *p;

    if (out == 0 || (settings == 0 && count != 0))
        return kCNErrBadParam;
    if (outCap < CN_H2_FRAME_HDR_LEN + payloadLen)
        return kCNErrBufferOverflow;

    /* SETTINGS is always on stream 0, no flags (this is not an ACK). */
    {
        CNH2FrameHeader h;
        h.length = payloadLen; h.type = kCNH2Settings; h.flags = 0; h.streamId = 0;
        CN_H2WriteFrameHeader(&h, out, outCap, 0);
    }

    p = out + CN_H2_FRAME_HDR_LEN;
    for (i = 0; i < count; i++) {
        p[0] = (unsigned char)((settings[i].id >> 8) & 0xFF);
        p[1] = (unsigned char)( settings[i].id       & 0xFF);
        cn_h2_put32(p + 2, settings[i].value);
        p += 6;
    }
    if (outLen)
        *outLen = CN_H2_FRAME_HDR_LEN + payloadLen;
    return noErr;
}

OSStatus CN_H2ParseSettings(const unsigned char *payload, UInt32 len,
                            void (*sink)(void *ctx, UInt16 id, UInt32 value),
                            void *ctx)
{
    UInt32 i;

    if ((payload == 0 && len != 0) || sink == 0)
        return kCNErrBadParam;
    if (len % 6u != 0)
        return kCNErrH2BadFrame;

    for (i = 0; i + 6u <= len; i += 6u) {
        UInt16 id    = (UInt16)(((UInt16)payload[i] << 8) | payload[i + 1]);
        UInt32 value = cn_h2_get32(payload + i + 2);
        sink(ctx, id, value);
    }
    return noErr;
}
