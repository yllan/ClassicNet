#ifndef CLASSICNET_CN_H2_H
#define CLASSICNET_CN_H2_H

#include "cn_types.h"

/*
 * HTTP/2 framing layer (RFC 7540 §4-§6).
 *
 * This module handles only the wire framing -- the fixed 9-byte frame header,
 * the connection preface, and SETTINGS payloads.  Header *compression* lives in
 * cn_hpack.{c,h}; stream multiplexing and flow control belong to a higher
 * connection layer.  Everything here is strictly length-bounded and NUL-safe so
 * it can be fuzzed against untrusted network input.
 */

/* The client connection preface a client must send before any frames. */
#define CN_H2_PREFACE      "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define CN_H2_PREFACE_LEN  24

#define CN_H2_FRAME_HDR_LEN 9           /* length(3) type(1) flags(1) streamId(4) */
#define CN_H2_MAX_FRAME_LEN 0x00FFFFFFu /* 24-bit length field maximum */

typedef enum {
    kCNH2Data         = 0x0,
    kCNH2Headers      = 0x1,
    kCNH2Priority     = 0x2,
    kCNH2RstStream    = 0x3,
    kCNH2Settings     = 0x4,
    kCNH2PushPromise  = 0x5,
    kCNH2Ping         = 0x6,
    kCNH2Goaway       = 0x7,
    kCNH2WindowUpdate = 0x8,
    kCNH2Continuation = 0x9
} CNH2FrameType;

/* Frame flags (meaning depends on frame type). */
enum {
    kCNH2FlagEndStream  = 0x01,  /* DATA, HEADERS */
    kCNH2FlagAck        = 0x01,  /* SETTINGS, PING */
    kCNH2FlagEndHeaders = 0x04,  /* HEADERS, PUSH_PROMISE, CONTINUATION */
    kCNH2FlagPadded     = 0x08,  /* DATA, HEADERS, PUSH_PROMISE */
    kCNH2FlagPriority   = 0x20   /* HEADERS */
};

/* Standard SETTINGS identifiers (RFC 7540 §6.5.2). */
enum {
    kCNH2SettingHeaderTableSize      = 0x1,
    kCNH2SettingEnablePush           = 0x2,
    kCNH2SettingMaxConcurrentStreams = 0x3,
    kCNH2SettingInitialWindowSize    = 0x4,
    kCNH2SettingMaxFrameSize         = 0x5,
    kCNH2SettingMaxHeaderListSize    = 0x6
};

typedef struct {
    UInt32 length;     /* payload length, 0..2^24-1 */
    UInt8  type;       /* CNH2FrameType */
    UInt8  flags;
    UInt32 streamId;   /* 31-bit; the reserved high bit is masked off on parse */
} CNH2FrameHeader;

typedef struct {
    UInt16 id;
    UInt32 value;
} CNH2Setting;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse a 9-byte frame header from buf[0,len). Does not touch the payload.
 *   noErr                    header parsed into *out
 *   kCNErrH2FrameIncomplete  fewer than 9 bytes available
 */
OSStatus CN_H2ParseFrameHeader(const unsigned char *buf, UInt32 len,
                               CNH2FrameHeader *out);

/*
 * Write a 9-byte frame header into out[0,outCap).
 *   kCNErrBufferOverflow   outCap < 9
 *   kCNErrH2FrameTooLarge  h->length exceeds CN_H2_MAX_FRAME_LEN
 */
OSStatus CN_H2WriteFrameHeader(const CNH2FrameHeader *h,
                               unsigned char *out, UInt32 outCap, UInt32 *outLen);

/*
 * Write a complete frame (9-byte header + payload) into out[0,outCap).
 * payload may be 0 when payloadLen is 0.
 */
OSStatus CN_H2BuildFrame(UInt8 type, UInt8 flags, UInt32 streamId,
                         const unsigned char *payload, UInt32 payloadLen,
                         unsigned char *out, UInt32 outCap, UInt32 *outLen);

/*
 * Build a complete SETTINGS frame (stream 0) carrying `count` settings.
 * Pass count 0 for an empty SETTINGS frame.
 */
OSStatus CN_H2BuildSettings(const CNH2Setting *settings, UInt32 count,
                            unsigned char *out, UInt32 outCap, UInt32 *outLen);

/*
 * Iterate the (id, value) pairs in a SETTINGS frame *payload* (not including the
 * 9-byte header). len must be a multiple of 6 (RFC 7540 §6.5).
 *   noErr               every entry delivered to sink
 *   kCNErrH2BadFrame    len is not a multiple of 6
 */
OSStatus CN_H2ParseSettings(const unsigned char *payload, UInt32 len,
                            void (*sink)(void *ctx, UInt16 id, UInt32 value),
                            void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CLASSICNET_CN_H2_H */
