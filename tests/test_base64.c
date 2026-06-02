#include "classicnet/cn_base64.h"
#include "classicnet/cn_errors.h"
#include "cn_test.h"

#include <string.h>

static void test_encode_vectors(void)
{
    /* RFC 4648 test vectors. */
    char out[16];
    UInt32 ol;
    CN_CHECK(CN_Base64Encode("", 0, out, sizeof(out), &ol) == noErr && strcmp(out, "") == 0);
    CN_CHECK(CN_Base64Encode("f", 1, out, sizeof(out), &ol) == noErr && strcmp(out, "Zg==") == 0);
    CN_CHECK(CN_Base64Encode("fo", 2, out, sizeof(out), &ol) == noErr && strcmp(out, "Zm8=") == 0);
    CN_CHECK(CN_Base64Encode("foo", 3, out, sizeof(out), &ol) == noErr && strcmp(out, "Zm9v") == 0);
    CN_CHECK(CN_Base64Encode("foob", 4, out, sizeof(out), &ol) == noErr && strcmp(out, "Zm9vYg==") == 0);
    CN_CHECK(CN_Base64Encode("fooba", 5, out, sizeof(out), &ol) == noErr && strcmp(out, "Zm9vYmE=") == 0);
    CN_CHECK(CN_Base64Encode("foobar", 6, out, sizeof(out), &ol) == noErr && strcmp(out, "Zm9vYmFy") == 0);
}

static void test_encode_overflow(void)
{
    char out[4];   /* too small for "Zg==" + NUL */
    UInt32 ol;
    CN_CHECK(CN_Base64Encode("f", 1, out, sizeof(out), &ol) == kCNErrBase64Overflow);
}

static void test_decode_roundtrip(void)
{
    const char *msg = "Hello, ClassicNet!";
    char enc[64];
    UInt8 dec[64];
    UInt32 el, dl;
    CN_CHECK(CN_Base64Encode(msg, (UInt32)strlen(msg), enc, sizeof(enc), &el) == noErr);
    CN_CHECK(CN_Base64Decode(enc, el, dec, sizeof(dec), &dl) == noErr);
    CN_CHECK(dl == (UInt32)strlen(msg));
    CN_CHECK(memcmp(dec, msg, dl) == 0);
}

static void test_decode_rejects(void)
{
    UInt8 dec[64];
    UInt32 dl;
    CN_CHECK(CN_Base64Decode("Zg=", 3, dec, sizeof(dec), &dl) == kCNErrBadBase64);   /* len % 4 */
    CN_CHECK(CN_Base64Decode("Zg.=", 4, dec, sizeof(dec), &dl) == kCNErrBadBase64);  /* bad char */
    CN_CHECK(CN_Base64Decode("Z=g=", 4, dec, sizeof(dec), &dl) == kCNErrBadBase64);  /* misplaced pad */
    CN_CHECK(CN_Base64Decode("Zm9vYg==Zm9v", 12, dec, sizeof(dec), &dl) == kCNErrBadBase64); /* pad mid-stream */
}

int main(void)
{
    CN_RUN(test_encode_vectors);
    CN_RUN(test_encode_overflow);
    CN_RUN(test_decode_roundtrip);
    CN_RUN(test_decode_rejects);
    return CN_SUMMARY();
}
