#include "classicnet/cn_sha1.h"
#include "cn_test.h"

#include <string.h>

/* Compare a digest to a 40-char lowercase hex string. */
static int digest_is(const UInt8 *d, const char *hex)
{
    static const char *h = "0123456789abcdef";
    int i;
    for (i = 0; i < CN_SHA1_DIGEST_LEN; i++) {
        if (hex[i * 2]     != h[(d[i] >> 4) & 0xF]) return 0;
        if (hex[i * 2 + 1] != h[d[i] & 0xF])        return 0;
    }
    return 1;
}

static void test_known_vectors(void)
{
    UInt8 d[CN_SHA1_DIGEST_LEN];

    CN_Sha1("", 0, d);
    CN_CHECK(digest_is(d, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));

    CN_Sha1("abc", 3, d);
    CN_CHECK(digest_is(d, "a9993e364706816aba3e25717850c26c9cd0d89d"));

    CN_Sha1("The quick brown fox jumps over the lazy dog", 43, d);
    CN_CHECK(digest_is(d, "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12"));
}

static void test_multiblock(void)
{
    /* RFC 3174 vector: 448-bit message spanning two blocks. */
    UInt8 d[CN_SHA1_DIGEST_LEN];
    CN_Sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    CN_CHECK(digest_is(d, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
}

static void test_streaming_million_a(void)
{
    /* SHA1 of one million 'a' -- exercises the update/block path repeatedly. */
    UInt8 d[CN_SHA1_DIGEST_LEN];
    char chunk[1000];
    CNSha1 c;
    int i;
    memset(chunk, 'a', sizeof(chunk));
    CN_Sha1Init(&c);
    for (i = 0; i < 1000; i++)
        CN_Sha1Update(&c, chunk, sizeof(chunk));
    CN_Sha1Final(&c, d);
    CN_CHECK(digest_is(d, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
}

int main(void)
{
    CN_RUN(test_known_vectors);
    CN_RUN(test_multiblock);
    CN_RUN(test_streaming_million_a);
    return CN_SUMMARY();
}
