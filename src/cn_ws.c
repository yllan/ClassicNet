#include "classicnet/cn_ws.h"
#include "classicnet/cn_errors.h"

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
