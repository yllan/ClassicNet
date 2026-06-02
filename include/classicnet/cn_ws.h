#ifndef CLASSICNET_CN_WS_H
#define CLASSICNET_CN_WS_H

#include "cn_types.h"

typedef enum {
    kCNWSContinuation = 0x0,
    kCNWSText         = 0x1,
    kCNWSBinary       = 0x2,
    kCNWSClose        = 0x8,
    kCNWSPing         = 0x9,
    kCNWSPong         = 0xA
} CNWSOpcode;

typedef struct {
    Boolean    fin;
    Boolean    masked;
    CNWSOpcode opcode;
    UInt32     payloadLen;   /* frames whose length exceeds 32 bits are rejected */
    UInt8      maskKey[4];   /* meaningful only when masked != 0 */
    UInt32     headerLen;    /* payload begins at buf[headerLen] */
} CNWSFrame;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode a single RFC 6455 frame *header* from buf[0,len).  Strictly bounded
 * and NUL-agnostic (binary input) -- a fuzz target.  Does not require the
 * payload to be present: reports payloadLen and headerLen so the caller can
 * stream the body.  Returns:
 *   noErr                   header decoded
 *   kCNErrFrameIncomplete   more bytes needed for the header
 *   kCNErrBadFrame          RSV set, reserved opcode, or control-frame rules broken
 *   kCNErrFrameTooLarge     64-bit length does not fit in 32 bits
 */
OSStatus CN_WSParseFrame(const unsigned char *buf, UInt32 len, CNWSFrame *out);

/* XOR-unmask len bytes of data in place with a 4-byte masking key. */
void CN_WSUnmask(unsigned char *data, UInt32 len, const UInt8 maskKey[4]);

/*
 * Compute the Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key
 * (RFC 6455 4.2.2): base64(SHA1(key + magic GUID)).  out must hold >= 29 bytes
 * (28 base64 chars + NUL).
 */
OSStatus CN_WSAcceptKey(const char *clientKey, char out[29]);

#ifdef __cplusplus
}
#endif

#endif /* CLASSICNET_CN_WS_H */
