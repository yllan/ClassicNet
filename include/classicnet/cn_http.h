#ifndef CLASSICNET_CN_HTTP_H
#define CLASSICNET_CN_HTTP_H

#include "cn_types.h"

#define CN_HTTP_MAX_HEADERS 32
#define CN_HTTP_MAX_NAME    64
#define CN_HTTP_MAX_VALUE   512

typedef struct {
    char name[CN_HTTP_MAX_NAME];
    char value[CN_HTTP_MAX_VALUE];
} CNHeaderField;

typedef struct {
    const char *name;
    const char *value;
} CNHeaderKV;

typedef struct {
    UInt16        status;       /* 100..599 */
    UInt8         httpMinor;    /* 0 or 1 (HTTP/1.x) */
    UInt8         headerCount;
    UInt32        bodyOffset;   /* index just past the CRLF CRLF terminator */
    CNHeaderField headers[CN_HTTP_MAX_HEADERS];
} CNHttpResponse;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse the status line + header block of an HTTP/1.x response from buf[0,len).
 *
 * Strictly length-bounded (never reads past len) and NUL-safe -- it ignores C
 * string termination entirely -- so it is the primary fuzz target for untrusted
 * network input.  Returns:
 *   noErr                    headers parsed; bodyOffset points at the body
 *   kCNErrHeadersIncomplete  more bytes needed (caller should read and retry)
 *   kCNErr*                  malformed input
 */
OSStatus CN_ParseHttpResponse(const char *buf, UInt32 len, CNHttpResponse *out);

/*
 * Decode an HTTP/1.1 chunked transfer-encoded body from in[0,inLen) into
 * out[0,outCap).  Single-shot and length-bounded.  Returns:
 *   noErr                  fully decoded; *outLen = decoded bytes,
 *                          *consumed = input bytes used (through the final CRLF)
 *   kCNErrChunkIncomplete  buffer lacks the terminating zero-size chunk yet
 *   kCNErrBadChunk         malformed size line, missing CRLF, etc.
 *   kCNErrChunkOverflow    decoded data would exceed outCap
 */
OSStatus CN_DecodeChunked(const char *in, UInt32 inLen,
                          char *out, UInt32 outCap,
                          UInt32 *outLen, UInt32 *consumed);

/*
 * Serialize an HTTP/1.1 request head (no body) into out[0,outCap):
 *   "METHOD path HTTP/1.1\r\nHost: host\r\n" + each header + "\r\n"
 * out is NUL-terminated; *outLen is the length without the NUL.
 * Returns kCNErrBufferOverflow if it does not fit.
 */
OSStatus CN_BuildRequest(const char *method, const char *path, const char *host,
                         const CNHeaderKV *headers, UInt32 headerCount,
                         char *out, UInt32 outCap, UInt32 *outLen);

#ifdef __cplusplus
}
#endif

#endif /* CLASSICNET_CN_HTTP_H */
