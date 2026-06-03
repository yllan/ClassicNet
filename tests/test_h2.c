/* Unit tests for the HTTP/2 framing layer (cn_h2). */
#include "classicnet/cn_h2.h"
#include "classicnet/cn_errors.h"
#include "cn_test.h"

#include <string.h>

static void test_parse_frame_header(void)
{
    /* length=0x4000 type=HEADERS(1) flags=END_HEADERS|END_STREAM(0x05) stream=1,
       with the reserved high bit set on the stream id to prove it is masked. */
    unsigned char buf[9] = { 0x00, 0x40, 0x00, 0x01, 0x05, 0x80, 0x00, 0x00, 0x01 };
    CNH2FrameHeader h;

    CN_CHECK(CN_H2ParseFrameHeader(buf, 9, &h) == noErr);
    CN_CHECK(h.length == 0x4000);
    CN_CHECK(h.type == kCNH2Headers);
    CN_CHECK(h.flags == (kCNH2FlagEndHeaders | kCNH2FlagEndStream));
    CN_CHECK(h.streamId == 1);                      /* reserved bit dropped */

    CN_CHECK(CN_H2ParseFrameHeader(buf, 8, &h) == kCNErrH2FrameIncomplete);
}

static void test_roundtrip_header(void)
{
    CNH2FrameHeader h, h2;
    unsigned char out[9];
    UInt32 n = 0;

    h.length = 0x123456; h.type = kCNH2Data; h.flags = kCNH2FlagEndStream;
    h.streamId = 0x7FFFFFFF;
    CN_CHECK(CN_H2WriteFrameHeader(&h, out, sizeof(out), &n) == noErr);
    CN_CHECK(n == 9);
    CN_CHECK(CN_H2ParseFrameHeader(out, 9, &h2) == noErr);
    CN_CHECK(h2.length == 0x123456);
    CN_CHECK(h2.type == kCNH2Data);
    CN_CHECK(h2.flags == kCNH2FlagEndStream);
    CN_CHECK(h2.streamId == 0x7FFFFFFF);

    h.length = CN_H2_MAX_FRAME_LEN + 1;
    CN_CHECK(CN_H2WriteFrameHeader(&h, out, sizeof(out), &n) == kCNErrH2FrameTooLarge);
    h.length = 1;
    CN_CHECK(CN_H2WriteFrameHeader(&h, out, 8, &n) == kCNErrBufferOverflow);
}

static void test_build_frame(void)
{
    unsigned char payload[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    unsigned char out[32];
    UInt32 n = 0;
    CNH2FrameHeader h;

    CN_CHECK(CN_H2BuildFrame(kCNH2WindowUpdate, 0, 0, payload, 4,
                             out, sizeof(out), &n) == noErr);
    CN_CHECK(n == 9 + 4);
    CN_CHECK(CN_H2ParseFrameHeader(out, n, &h) == noErr);
    CN_CHECK(h.length == 4 && h.type == kCNH2WindowUpdate);
    CN_CHECK(memcmp(out + 9, payload, 4) == 0);

    /* too small a buffer is rejected, not overrun */
    CN_CHECK(CN_H2BuildFrame(kCNH2Data, 0, 1, payload, 4, out, 10, &n)
             == kCNErrBufferOverflow);
}

static int g_settings_seen;
static UInt32 g_initial_window;
static void on_setting(void *ctx, UInt16 id, UInt32 value)
{
    (void)ctx;
    g_settings_seen++;
    if (id == kCNH2SettingInitialWindowSize)
        g_initial_window = value;
}

static void test_settings_roundtrip(void)
{
    CNH2Setting s[2];
    unsigned char out[32];
    UInt32 n = 0;
    CNH2FrameHeader h;

    s[0].id = kCNH2SettingMaxConcurrentStreams; s[0].value = 100;
    s[1].id = kCNH2SettingInitialWindowSize;    s[1].value = 65535;

    CN_CHECK(CN_H2BuildSettings(s, 2, out, sizeof(out), &n) == noErr);
    CN_CHECK(n == 9 + 12);
    CN_CHECK(CN_H2ParseFrameHeader(out, n, &h) == noErr);
    CN_CHECK(h.type == kCNH2Settings && h.streamId == 0 && h.flags == 0);
    CN_CHECK(h.length == 12);

    g_settings_seen = 0; g_initial_window = 0;
    CN_CHECK(CN_H2ParseSettings(out + 9, h.length, on_setting, 0) == noErr);
    CN_CHECK(g_settings_seen == 2);
    CN_CHECK(g_initial_window == 65535);

    /* a payload whose length is not a multiple of 6 is malformed */
    CN_CHECK(CN_H2ParseSettings(out + 9, 5, on_setting, 0) == kCNErrH2BadFrame);

    /* empty SETTINGS (e.g. the ACK uses 0-length) builds cleanly */
    CN_CHECK(CN_H2BuildSettings(0, 0, out, sizeof(out), &n) == noErr);
    CN_CHECK(n == 9);
}

int main(void)
{
    CN_RUN(test_parse_frame_header);
    CN_RUN(test_roundtrip_header);
    CN_RUN(test_build_frame);
    CN_RUN(test_settings_roundtrip);
    return CN_SUMMARY();
}
