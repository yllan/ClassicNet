#include "classicnet/cn_sha1.h"

#include <string.h>

static UInt32 rol(UInt32 v, int n)
{
    return (UInt32)((v << n) | (v >> (32 - n)));
}

static void sha1_block(CNSha1 *ctx, const UInt8 *p)
{
    UInt32 w[80];
    UInt32 a, b, cc, d, e, t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((UInt32)p[i * 4] << 24) | ((UInt32)p[i * 4 + 1] << 16) |
               ((UInt32)p[i * 4 + 2] << 8) | (UInt32)p[i * 4 + 3];
    for (i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = ctx->h[0]; b = ctx->h[1]; cc = ctx->h[2]; d = ctx->h[3]; e = ctx->h[4];

    for (i = 0; i < 80; i++) {
        UInt32 f, k;
        if (i < 20)      { f = (b & cc) | ((~b) & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ cc ^ d;                     k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ cc ^ d;                     k = 0xCA62C1D6u; }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += cc; ctx->h[3] += d; ctx->h[4] += e;
}

void CN_Sha1Init(CNSha1 *c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->lenHi = 0; c->lenLo = 0; c->blockLen = 0;
}

void CN_Sha1Update(CNSha1 *c, const void *data, UInt32 len)
{
    const UInt8 *p = (const UInt8 *)data;
    UInt32 addLo = len << 3;
    UInt32 addHi = len >> 29;

    c->lenLo += addLo;
    if (c->lenLo < addLo) c->lenHi++;
    c->lenHi += addHi;

    while (len > 0) {
        UInt32 n = 64 - c->blockLen;
        if (n > len) n = len;
        memcpy(c->block + c->blockLen, p, n);
        c->blockLen += n;
        p += n;
        len -= n;
        if (c->blockLen == 64) {
            sha1_block(c, c->block);
            c->blockLen = 0;
        }
    }
}

void CN_Sha1Final(CNSha1 *c, UInt8 digest[CN_SHA1_DIGEST_LEN])
{
    UInt8 lenBytes[8];
    UInt8 pad = 0x80;
    UInt8 zero = 0;
    UInt32 hi = c->lenHi, lo = c->lenLo;   /* capture before padding mutates them */
    int i;

    lenBytes[0] = (UInt8)(hi >> 24); lenBytes[1] = (UInt8)(hi >> 16);
    lenBytes[2] = (UInt8)(hi >> 8);  lenBytes[3] = (UInt8)hi;
    lenBytes[4] = (UInt8)(lo >> 24); lenBytes[5] = (UInt8)(lo >> 16);
    lenBytes[6] = (UInt8)(lo >> 8);  lenBytes[7] = (UInt8)lo;

    CN_Sha1Update(c, &pad, 1);
    while (c->blockLen != 56)
        CN_Sha1Update(c, &zero, 1);
    CN_Sha1Update(c, lenBytes, 8);   /* now blockLen == 0 */

    for (i = 0; i < 5; i++) {
        digest[i * 4]     = (UInt8)(c->h[i] >> 24);
        digest[i * 4 + 1] = (UInt8)(c->h[i] >> 16);
        digest[i * 4 + 2] = (UInt8)(c->h[i] >> 8);
        digest[i * 4 + 3] = (UInt8)(c->h[i]);
    }
}

void CN_Sha1(const void *data, UInt32 len, UInt8 digest[CN_SHA1_DIGEST_LEN])
{
    CNSha1 c;
    CN_Sha1Init(&c);
    CN_Sha1Update(&c, data, len);
    CN_Sha1Final(&c, digest);
}
