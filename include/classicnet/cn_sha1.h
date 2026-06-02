#ifndef CLASSICNET_CN_SHA1_H
#define CLASSICNET_CN_SHA1_H

#include "cn_types.h"

#define CN_SHA1_DIGEST_LEN 20

typedef struct {
    UInt32 h[5];
    UInt32 lenHi;          /* message length in bits, high 32 */
    UInt32 lenLo;          /* message length in bits, low 32 */
    UInt8  block[64];
    UInt32 blockLen;
} CNSha1;

#ifdef __cplusplus
extern "C" {
#endif

void CN_Sha1Init(CNSha1 *c);
void CN_Sha1Update(CNSha1 *c, const void *data, UInt32 len);
void CN_Sha1Final(CNSha1 *c, UInt8 digest[CN_SHA1_DIGEST_LEN]);

/* One-shot convenience wrapper. */
void CN_Sha1(const void *data, UInt32 len, UInt8 digest[CN_SHA1_DIGEST_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* CLASSICNET_CN_SHA1_H */
