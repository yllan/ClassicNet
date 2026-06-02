#include "classicnet/cn_base64.h"
#include "classicnet/cn_errors.h"

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

OSStatus CN_Base64Encode(const void *data, UInt32 len,
                         char *out, UInt32 outCap, UInt32 *outLen)
{
    const UInt8 *p = (const UInt8 *)data;
    UInt32 need = ((len + 2) / 3) * 4;
    UInt32 i = 0, o = 0;

    if (data == 0 || out == 0)
        return kCNErrBadParam;
    if (outCap < need + 1)
        return kCNErrBase64Overflow;

    while (i + 3 <= len) {
        UInt32 v = ((UInt32)p[i] << 16) | ((UInt32)p[i + 1] << 8) | (UInt32)p[i + 2];
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = B64[(v >> 6) & 0x3F];
        out[o++] = B64[v & 0x3F];
        i += 3;
    }
    if (len - i == 1) {
        UInt32 v = (UInt32)p[i] << 16;
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (len - i == 2) {
        UInt32 v = ((UInt32)p[i] << 16) | ((UInt32)p[i + 1] << 8);
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = B64[(v >> 6) & 0x3F];
        out[o++] = '=';
    }
    out[o] = '\0';
    if (outLen) *outLen = o;
    return noErr;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

OSStatus CN_Base64Decode(const char *in, UInt32 inLen,
                         UInt8 *out, UInt32 outCap, UInt32 *outLen)
{
    UInt32 i = 0, o = 0;

    if (in == 0 || out == 0 || outLen == 0)
        return kCNErrBadParam;
    if (inLen % 4 != 0)
        return kCNErrBadBase64;

    while (i < inLen) {
        char c2 = in[i + 2], c3 = in[i + 3];
        int v0 = b64val(in[i]), v1 = b64val(in[i + 1]), v2, v3;
        UInt32 trip;
        int nout;

        if (v0 < 0 || v1 < 0)
            return kCNErrBadBase64;

        if (c3 == '=') {
            nout = 2; v3 = 0;
            if (c2 == '=') { nout = 1; v2 = 0; }
            else { v2 = b64val(c2); if (v2 < 0) return kCNErrBadBase64; }
        } else {
            v2 = b64val(c2); v3 = b64val(c3);
            if (v2 < 0 || v3 < 0) return kCNErrBadBase64;
            nout = 3;
        }
        /* padding is only legal in the final quad */
        if ((c2 == '=' || c3 == '=') && i + 4 != inLen)
            return kCNErrBadBase64;

        trip = ((UInt32)v0 << 18) | ((UInt32)v1 << 12) |
               ((UInt32)v2 << 6) | (UInt32)v3;

        if ((UInt32)nout > outCap - o)
            return kCNErrBase64Overflow;
        if (nout >= 1) out[o++] = (UInt8)(trip >> 16);
        if (nout >= 2) out[o++] = (UInt8)(trip >> 8);
        if (nout >= 3) out[o++] = (UInt8)(trip);
        i += 4;
    }
    *outLen = o;
    return noErr;
}
