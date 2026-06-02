#include "classicnet/cn_ws.h"
#include "classicnet/cn_errors.h"
#include "cn_test.h"

#include <string.h>

static void test_small_unmasked_text(void)
{
    /* FIN + text(0x1), unmasked, len 5 */
    const unsigned char f[] = { 0x81, 0x05, 'h','e','l','l','o' };
    CNWSFrame fr;
    CN_CHECK(CN_WSParseFrame(f, sizeof(f), &fr) == noErr);
    CN_CHECK(fr.fin == true);
    CN_CHECK(fr.opcode == kCNWSText);
    CN_CHECK(fr.masked == false);
    CN_CHECK(fr.payloadLen == 5);
    CN_CHECK(fr.headerLen == 2);
}

static void test_masked_roundtrip(void)
{
    /* FIN + binary(0x2), masked, len 3, key {0x37,0xfa,0x21,0x3d} */
    unsigned char f[] = {
        0x82, 0x83, 0x37, 0xfa, 0x21, 0x3d,
        0x37 ^ 'A', 0xfa ^ 'B', 0x21 ^ 'C'
    };
    CNWSFrame fr;
    unsigned char *payload;
    CN_CHECK(CN_WSParseFrame(f, sizeof(f), &fr) == noErr);
    CN_CHECK(fr.masked == true);
    CN_CHECK(fr.payloadLen == 3);
    CN_CHECK(fr.headerLen == 6);
    CN_CHECK(fr.maskKey[0] == 0x37 && fr.maskKey[3] == 0x3d);

    payload = f + fr.headerLen;
    CN_WSUnmask(payload, fr.payloadLen, fr.maskKey);
    CN_CHECK(payload[0] == 'A' && payload[1] == 'B' && payload[2] == 'C');
}

static void test_extended_len_126(void)
{
    unsigned char f[4 + 200];
    CNWSFrame fr;
    int k;
    f[0] = 0x82;            /* FIN + binary */
    f[1] = 126;            /* 16-bit length follows */
    f[2] = 0x00; f[3] = 0xC8;   /* 200 */
    for (k = 0; k < 200; k++) f[4 + k] = (unsigned char)k;
    CN_CHECK(CN_WSParseFrame(f, sizeof(f), &fr) == noErr);
    CN_CHECK(fr.payloadLen == 200);
    CN_CHECK(fr.headerLen == 4);
}

static void test_extended_len_127_ok(void)
{
    /* 64-bit length = 0x0000000000010000 (65536) -- fits in 32 bits */
    const unsigned char f[10] = {
        0x82, 127, 0x00,0x00,0x00,0x00, 0x00,0x01,0x00,0x00
    };
    CNWSFrame fr;
    CN_CHECK(CN_WSParseFrame(f, sizeof(f), &fr) == noErr);
    CN_CHECK(fr.payloadLen == 65536);
    CN_CHECK(fr.headerLen == 10);
}

static void test_frame_too_large(void)
{
    /* 64-bit length with a nonzero high word -> reject on 32-bit target */
    const unsigned char f[10] = {
        0x82, 127, 0x00,0x00,0x00,0x01, 0x00,0x00,0x00,0x00
    };
    CNWSFrame fr;
    CN_CHECK(CN_WSParseFrame(f, sizeof(f), &fr) == kCNErrFrameTooLarge);
}

static void test_rejects_bad_frames(void)
{
    CNWSFrame fr;
    const unsigned char rsv[]        = { 0xC1, 0x00 };          /* RSV1 set */
    const unsigned char reserved_op[] = { 0x83, 0x00 };         /* opcode 0x3 */
    const unsigned char big_control[] = { 0x89, 126, 0x00,0x80 };/* ping len 128 */
    const unsigned char frag_control[] = { 0x09, 0x00 };        /* ping, FIN=0 */
    CN_CHECK(CN_WSParseFrame(rsv,         sizeof(rsv),         &fr) == kCNErrBadFrame);
    CN_CHECK(CN_WSParseFrame(reserved_op, sizeof(reserved_op), &fr) == kCNErrBadFrame);
    CN_CHECK(CN_WSParseFrame(big_control, sizeof(big_control), &fr) == kCNErrBadFrame);
    CN_CHECK(CN_WSParseFrame(frag_control,sizeof(frag_control),&fr) == kCNErrBadFrame);
}

static void test_incomplete_headers(void)
{
    CNWSFrame fr;
    const unsigned char one[]      = { 0x81 };                  /* < 2 bytes */
    const unsigned char need16[]   = { 0x82, 126, 0x00 };       /* need 2 len bytes */
    const unsigned char need_mask[] = { 0x82, 0x80, 0x37 };     /* masked, need 4 key */
    CN_CHECK(CN_WSParseFrame(one,       sizeof(one),       &fr) == kCNErrFrameIncomplete);
    CN_CHECK(CN_WSParseFrame(need16,    sizeof(need16),    &fr) == kCNErrFrameIncomplete);
    CN_CHECK(CN_WSParseFrame(need_mask, sizeof(need_mask), &fr) == kCNErrFrameIncomplete);
}

static void test_accept_key(void)
{
    /* RFC 6455 4.2.2 worked example. */
    char accept[29];
    CN_CHECK(CN_WSAcceptKey("dGhlIHNhbXBsZSBub25jZQ==", accept) == noErr);
    CN_CHECK(strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
}

int main(void)
{
    CN_RUN(test_small_unmasked_text);
    CN_RUN(test_masked_roundtrip);
    CN_RUN(test_extended_len_126);
    CN_RUN(test_extended_len_127_ok);
    CN_RUN(test_frame_too_large);
    CN_RUN(test_rejects_bad_frames);
    CN_RUN(test_incomplete_headers);
    CN_RUN(test_accept_key);
    return CN_SUMMARY();
}
