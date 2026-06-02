#include "classicnet/cn_ws.h"
#include "classicnet/cn_errors.h"
#include "classicnet/cn_sha1.h"
#include "classicnet/cn_base64.h"

#include <string.h>

OSStatus CN_WSParseFrame(const unsigned char *buf, UInt32 len, CNWSFrame *out)
{
    UInt32 i = 2;          /* smallest possible header is 2 bytes */
    unsigned char b0, b1;
    UInt32 n7;

    if (buf == 0 || out == 0)
        return kCNErrBadFrame;
    if (len < 2)
        return kCNErrFrameIncomplete;

    b0 = buf[0];
    b1 = buf[1];

    if (b0 & 0x70)                       /* RSV1-3 must be zero (no extensions) */
        return kCNErrBadFrame;
    out->fin = (Boolean)((b0 & 0x80) ? 1 : 0);

    {
        unsigned char op = (unsigned char)(b0 & 0x0F);
        switch (op) {
            case 0x0: case 0x1: case 0x2:        /* continuation / text / binary */
            case 0x8: case 0x9: case 0xA:        /* close / ping / pong */
                out->opcode = (CNWSOpcode)op;
                break;
            default:
                return kCNErrBadFrame;           /* reserved opcode */
        }
    }

    out->masked = (Boolean)((b1 & 0x80) ? 1 : 0);
    n7 = (UInt32)(b1 & 0x7F);

    /* Control frames must carry <=125 bytes and may not be fragmented. */
    if (out->opcode & 0x08) {
        if (n7 > 125) return kCNErrBadFrame;
        if (!out->fin) return kCNErrBadFrame;
    }

    if (n7 < 126) {
        out->payloadLen = n7;
    } else if (n7 == 126) {
        if (len < 4) return kCNErrFrameIncomplete;
        out->payloadLen = (UInt32)(((UInt32)buf[2] << 8) | (UInt32)buf[3]);
        i = 4;
    } else {                              /* n7 == 127: 64-bit length */
        UInt32 hi, lo;
        if (len < 10) return kCNErrFrameIncomplete;
        if (buf[2] & 0x80) return kCNErrBadFrame;   /* MSB must be 0 */
        hi = ((UInt32)buf[2] << 24) | ((UInt32)buf[3] << 16) |
             ((UInt32)buf[4] <<  8) |  (UInt32)buf[5];
        lo = ((UInt32)buf[6] << 24) | ((UInt32)buf[7] << 16) |
             ((UInt32)buf[8] <<  8) |  (UInt32)buf[9];
        if (hi != 0)                       /* > 4 GiB: refuse on a 32-bit target */
            return kCNErrFrameTooLarge;
        out->payloadLen = lo;
        i = 10;
    }

    if (out->masked) {
        if (len < i + 4) return kCNErrFrameIncomplete;
        out->maskKey[0] = buf[i];
        out->maskKey[1] = buf[i + 1];
        out->maskKey[2] = buf[i + 2];
        out->maskKey[3] = buf[i + 3];
        i += 4;
    } else {
        out->maskKey[0] = 0;
        out->maskKey[1] = 0;
        out->maskKey[2] = 0;
        out->maskKey[3] = 0;
    }

    out->headerLen = i;
    return noErr;
}

void CN_WSUnmask(unsigned char *data, UInt32 len, const UInt8 maskKey[4])
{
    UInt32 k;
    for (k = 0; k < len; k++)
        data[k] = (unsigned char)(data[k] ^ maskKey[k & 3]);
}

OSStatus CN_WSAcceptKey(const char *clientKey, char out[29])
{
    static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    CNSha1 c;
    UInt8  digest[CN_SHA1_DIGEST_LEN];
    UInt32 outLen;

    if (clientKey == 0 || out == 0)
        return kCNErrBadParam;

    CN_Sha1Init(&c);
    CN_Sha1Update(&c, clientKey, (UInt32)strlen(clientKey));
    CN_Sha1Update(&c, kGuid, (UInt32)(sizeof(kGuid) - 1));
    CN_Sha1Final(&c, digest);

    return CN_Base64Encode(digest, CN_SHA1_DIGEST_LEN, out, 29, &outLen);
}

OSStatus CN_WSBuildUpgrade(const char *host, const char *path,
                           const UInt8 nonce[16],
                           char *out, UInt32 outCap, UInt32 *outLen,
                           char acceptOut[29])
{
    char key[25];                /* base64 of 16 bytes = 24 chars + NUL */
    UInt32 keyLen;
    OSStatus s;
    CNHeaderKV hdrs[4];

    if (host == 0 || path == 0 || nonce == 0 || out == 0 || acceptOut == 0)
        return kCNErrBadParam;

    s = CN_Base64Encode(nonce, 16, key, sizeof(key), &keyLen);
    if (s != noErr) return s;
    s = CN_WSAcceptKey(key, acceptOut);
    if (s != noErr) return s;

    hdrs[0].name = "Upgrade";               hdrs[0].value = "websocket";
    hdrs[1].name = "Connection";            hdrs[1].value = "Upgrade";
    hdrs[2].name = "Sec-WebSocket-Key";     hdrs[2].value = key;
    hdrs[3].name = "Sec-WebSocket-Version"; hdrs[3].value = "13";

    return CN_BuildRequest("GET", path, host, hdrs, 4, out, outCap, outLen);
}

static int cn_ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

OSStatus CN_WSCheckUpgradeResponse(const CNHttpResponse *resp,
                                   const char *expectedAccept)
{
    UInt32 i;
    if (resp == 0 || expectedAccept == 0)
        return kCNErrBadParam;
    if (resp->status != 101)
        return kCNErrHandshakeFailed;
    for (i = 0; i < resp->headerCount; i++) {
        if (cn_ci_eq(resp->headers[i].name, "Sec-WebSocket-Accept")) {
            if (strcmp(resp->headers[i].value, expectedAccept) == 0)
                return noErr;
            return kCNErrHandshakeFailed;
        }
    }
    return kCNErrHandshakeFailed;
}
