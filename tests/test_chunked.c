#include "classicnet/cn_http.h"
#include "classicnet/cn_errors.h"
#include "cn_test.h"

#include <string.h>

#define DECODE(lit, out, cap, ol, cons) \
    CN_DecodeChunked((lit), (UInt32)(sizeof(lit) - 1), (out), (cap), (ol), (cons))

static void test_basic(void)
{
    char out[64];
    UInt32 ol = 0, cons = 0;
    const char in[] = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    CN_CHECK(DECODE(in, out, sizeof(out), &ol, &cons) == noErr);
    CN_CHECK(ol == 9);
    CN_CHECK(memcmp(out, "Wikipedia", 9) == 0);
    CN_CHECK(cons == sizeof(in) - 1);
}

static void test_chunk_ext_ignored(void)
{
    char out[64];
    UInt32 ol = 0, cons = 0;
    const char in[] = "4;name=value\r\nWiki\r\n0\r\n\r\n";
    CN_CHECK(DECODE(in, out, sizeof(out), &ol, &cons) == noErr);
    CN_CHECK(ol == 4);
    CN_CHECK(memcmp(out, "Wiki", 4) == 0);
}

static void test_uppercase_multi_digit_size(void)
{
    char out[300];
    UInt32 ol = 0, cons = 0, i;
    char in[16 + 256 + 8];
    UInt32 n = 0;
    /* "100\r\n" + 256 bytes + "\r\n0\r\n\r\n" */
    memcpy(in + n, "100\r\n", 5); n += 5;
    for (i = 0; i < 256; i++) in[n++] = (char)i;
    memcpy(in + n, "\r\n0\r\n\r\n", 7); n += 7;
    CN_CHECK(CN_DecodeChunked(in, n, out, sizeof(out), &ol, &cons) == noErr);
    CN_CHECK(ol == 256);
    CN_CHECK((unsigned char)out[255] == 255);
}

static void test_trailer_then_blank(void)
{
    char out[64];
    UInt32 ol = 0, cons = 0;
    const char in[] = "3\r\nabc\r\n0\r\nX-Trailer: yes\r\n\r\n";
    CN_CHECK(DECODE(in, out, sizeof(out), &ol, &cons) == noErr);
    CN_CHECK(ol == 3);
    CN_CHECK(memcmp(out, "abc", 3) == 0);
}

static void test_incomplete(void)
{
    char out[64];
    UInt32 ol = 0, cons = 0;
    const char a[] = "4\r\nWiki\r\n5\r\npedia\r\n";   /* no terminating 0-chunk */
    const char b[] = "4\r\nWi";                       /* data truncated */
    const char c[] = "4\r\nWiki\r\n0\r\n";            /* missing final blank line */
    CN_CHECK(DECODE(a, out, sizeof(out), &ol, &cons) == kCNErrChunkIncomplete);
    CN_CHECK(DECODE(b, out, sizeof(out), &ol, &cons) == kCNErrChunkIncomplete);
    CN_CHECK(DECODE(c, out, sizeof(out), &ol, &cons) == kCNErrChunkIncomplete);
}

static void test_bad_chunk(void)
{
    char out[64];
    UInt32 ol = 0, cons = 0;
    const char no_hex[]   = "x\r\nabc\r\n0\r\n\r\n";
    const char bad_crlf[] = "3\r\nabcX0\r\n\r\n";   /* data not followed by CRLF */
    CN_CHECK(DECODE(no_hex,   out, sizeof(out), &ol, &cons) == kCNErrBadChunk);
    CN_CHECK(DECODE(bad_crlf, out, sizeof(out), &ol, &cons) == kCNErrBadChunk);
}

static void test_overflow(void)
{
    char out[4];
    UInt32 ol = 0, cons = 0;
    const char in[] = "8\r\noverflow\r\n0\r\n\r\n";   /* 8 bytes into a 4-byte buffer */
    CN_CHECK(DECODE(in, out, sizeof(out), &ol, &cons) == kCNErrChunkOverflow);
}

int main(void)
{
    CN_RUN(test_basic);
    CN_RUN(test_chunk_ext_ignored);
    CN_RUN(test_uppercase_multi_digit_size);
    CN_RUN(test_trailer_then_blank);
    CN_RUN(test_incomplete);
    CN_RUN(test_bad_chunk);
    CN_RUN(test_overflow);
    return CN_SUMMARY();
}
