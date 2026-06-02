#ifndef CLASSICNET_CN_ERRORS_H
#define CLASSICNET_CN_ERRORS_H

#include "cn_types.h"

/*
 * ClassicNet reserves a private OSStatus range starting at kCNErrBase and
 * counting downward (see DESIGN.md, locked decision #2: errors are OSStatus).
 */
enum {
    kCNErrBase              = -30000,
    kCNErrBadURL            = -30001,
    kCNErrUnsupportedScheme = -30002,
    kCNErrHostTooLong       = -30003,
    kCNErrPathTooLong       = -30004,
    kCNErrBadPort           = -30005,

    /* HTTP/1.x response parser */
    kCNErrHeadersIncomplete = -30010,  /* need more bytes; not an error per se */
    kCNErrBadStatusLine     = -30011,
    kCNErrBadHeader         = -30012,
    kCNErrTooManyHeaders    = -30013,
    kCNErrHeaderTooLong     = -30014,

    /* WebSocket frame parser */
    kCNErrFrameIncomplete   = -30020,  /* need more bytes; not an error per se */
    kCNErrBadFrame          = -30021,
    kCNErrFrameTooLarge     = -30022,  /* 64-bit length exceeds 32-bit on target */

    /* HTTP chunked transfer-encoding decoder */
    kCNErrChunkIncomplete   = -30030,  /* need more bytes; not an error per se */
    kCNErrBadChunk          = -30031,
    kCNErrChunkOverflow     = -30032,  /* decoded data exceeds the output buffer */

    /* Base64 */
    kCNErrBadBase64         = -30040,  /* malformed base64 input */
    kCNErrBase64Overflow    = -30041,  /* result exceeds the output buffer */

    /* request serialization / handshake */
    kCNErrBufferOverflow    = -30060,  /* serialized output exceeds the buffer */
    kCNErrHandshakeFailed   = -30061,  /* WebSocket upgrade response invalid */

    /* async transport / request state machine */
    kCNErrWouldBlock        = -30070,  /* operation pending; pump again later */
    kCNErrResponseTooLarge  = -30071,  /* response head exceeds the buffer */
    kCNErrConnClosed        = -30072,  /* peer closed before the response completed */

    /* TLS */
    kCNErrTlsInit           = -30080,  /* TLS context / config setup failed */
    kCNErrTlsHandshake      = -30081,  /* handshake failed (incl. cert verify) */
    kCNErrTlsIo             = -30082,  /* TLS read/write error */

    /* generic */
    kCNErrBadParam          = -30050
};

#endif /* CLASSICNET_CN_ERRORS_H */
