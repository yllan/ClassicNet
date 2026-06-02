#ifndef CLASSICNET_CN_BASE64_H
#define CLASSICNET_CN_BASE64_H

#include "cn_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Standard base64 (RFC 4648) with '=' padding.
 * Encode: out receives a NUL-terminated string; outCap must be at least
 *         ((len + 2) / 3) * 4 + 1.  *outLen is the string length (no NUL).
 * Decode: length-bounded and binary-safe; rejects bad chars, wrong length,
 *         and misplaced padding.  Returns kCNErrBadBase64 / kCNErrBase64Overflow.
 */
OSStatus CN_Base64Encode(const void *data, UInt32 len,
                         char *out, UInt32 outCap, UInt32 *outLen);
OSStatus CN_Base64Decode(const char *in, UInt32 inLen,
                         UInt8 *out, UInt32 outCap, UInt32 *outLen);

#ifdef __cplusplus
}
#endif

#endif /* CLASSICNET_CN_BASE64_H */
