/*
 * Unit tests for HPACK (cn_hpack), driven by the authoritative test vectors in
 * RFC 7541 Appendix C -- request series C.3 (non-Huffman, cumulative dynamic
 * table), C.4 (Huffman), and response series C.5 (eviction at table size 256).
 */
#include "classicnet/cn_hpack.h"
#include "classicnet/cn_errors.h"
#include "cn_test.h"

#include <string.h>

/* ---- collected output ---- */
#define MAXF 32
static char g_name[MAXF][128];
static char g_val[MAXF][512];
static int  g_count;

static OSStatus collect(void *ctx, const char *name, UInt32 nameLen,
                        const char *value, UInt32 valueLen)
{
    (void)ctx;
    if (g_count >= MAXF) return kCNErrHpackOverflow;
    if (nameLen >= sizeof(g_name[0]) || valueLen >= sizeof(g_val[0]))
        return kCNErrHpackOverflow;
    memcpy(g_name[g_count], name, nameLen);  g_name[g_count][nameLen] = 0;
    memcpy(g_val[g_count],  value, valueLen); g_val[g_count][valueLen] = 0;
    g_count++;
    return noErr;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* decode a compact hex string into buf; returns byte length */
static UInt32 unhex(const char *hex, unsigned char *buf, UInt32 cap)
{
    UInt32 n = 0;
    while (hex[0] && hex[1] && n < cap) {
        buf[n++] = (unsigned char)((hexval(hex[0]) << 4) | hexval(hex[1]));
        hex += 2;
    }
    return n;
}

static OSStatus decode_hex(CNHpackDec *d, const char *hex)
{
    unsigned char buf[512];
    UInt32 n = unhex(hex, buf, sizeof(buf));
    g_count = 0;
    return CN_HpackDecode(d, buf, n, collect, 0);
}

static int field_eq(int i, const char *name, const char *value)
{
    return strcmp(g_name[i], name) == 0 && strcmp(g_val[i], value) == 0;
}

/* RFC 7541 C.3 -- one decoder, three cumulative requests, no Huffman. */
static void test_c3_requests(void)
{
    CNHpackDec d;
    CN_HpackDecInit(&d, 4096);

    CN_CHECK(decode_hex(&d, "828684410f7777772e6578616d706c652e636f6d") == noErr);
    CN_CHECK(g_count == 4);
    CN_CHECK(field_eq(0, ":method", "GET"));
    CN_CHECK(field_eq(1, ":scheme", "http"));
    CN_CHECK(field_eq(2, ":path", "/"));
    CN_CHECK(field_eq(3, ":authority", "www.example.com"));

    CN_CHECK(decode_hex(&d, "828684be58086e6f2d6361636865") == noErr);
    CN_CHECK(g_count == 5);
    CN_CHECK(field_eq(3, ":authority", "www.example.com"));  /* from dyn table (be) */
    CN_CHECK(field_eq(4, "cache-control", "no-cache"));

    CN_CHECK(decode_hex(&d, "828785bf400a637573746f6d2d6b65790c637573746f6d2d76616c7565") == noErr);
    CN_CHECK(g_count == 5);
    CN_CHECK(field_eq(1, ":scheme", "https"));
    CN_CHECK(field_eq(2, ":path", "/index.html"));
    CN_CHECK(field_eq(3, ":authority", "www.example.com"));  /* bf */
    CN_CHECK(field_eq(4, "custom-key", "custom-value"));
}

/* RFC 7541 C.4 -- same requests, Huffman-encoded literals. */
static void test_c4_huffman(void)
{
    CNHpackDec d;
    CN_HpackDecInit(&d, 4096);

    CN_CHECK(decode_hex(&d, "828684418cf1e3c2e5f23a6ba0ab90f4ff") == noErr);
    CN_CHECK(g_count == 4);
    CN_CHECK(field_eq(3, ":authority", "www.example.com"));   /* Huffman-decoded */

    CN_CHECK(decode_hex(&d, "828684be5886a8eb10649cbf") == noErr);
    CN_CHECK(g_count == 5);
    CN_CHECK(field_eq(4, "cache-control", "no-cache"));

    CN_CHECK(decode_hex(&d, "828785bf408825a849e95ba97d7f8925a849e95bb8e8b4bf") == noErr);
    CN_CHECK(g_count == 5);
    CN_CHECK(field_eq(4, "custom-key", "custom-value"));
}

/* RFC 7541 C.5 -- responses with the dynamic table capped at 256, forcing
   eviction.  Exercises name-from-static-index literals + indexed-from-dynamic. */
static void test_c5_eviction(void)
{
    CNHpackDec d;
    CN_HpackDecInit(&d, 256);

    CN_CHECK(decode_hex(&d,
        "48033330325807707269766174"
        "65611d4d6f6e2c203231204f637420323031332032303a31333a323120474d54"
        "6e1768747470733a2f2f7777772e6578616d706c652e636f6d") == noErr);
    CN_CHECK(g_count == 4);
    CN_CHECK(field_eq(0, ":status", "302"));
    CN_CHECK(field_eq(1, "cache-control", "private"));
    CN_CHECK(field_eq(2, "date", "Mon, 21 Oct 2013 20:13:21 GMT"));
    CN_CHECK(field_eq(3, "location", "https://www.example.com"));

    CN_CHECK(decode_hex(&d, "4803333037c1c0bf") == noErr);
    CN_CHECK(g_count == 4);
    CN_CHECK(field_eq(0, ":status", "307"));               /* evicts ":status 302" */
    CN_CHECK(field_eq(1, "cache-control", "private"));     /* c1 -> dyn */
    CN_CHECK(field_eq(3, "location", "https://www.example.com"));

    CN_CHECK(decode_hex(&d,
        "88c1611d4d6f6e2c203231204f637420323031332032313a31333a323220474d54"
        "c05a04677a69707738"
        "666f6f3d4153444a4b48514b425a584f5157454f50495541585157454f4955"
        "3b206d61782d6167653d333630303b2076657273696f6e3d31") == noErr);
    CN_CHECK(g_count == 6);
    CN_CHECK(field_eq(0, ":status", "200"));
    CN_CHECK(field_eq(2, "date", "Mon, 21 Oct 2013 21:13:22 GMT"));
    CN_CHECK(field_eq(4, "content-encoding", "gzip"));
    CN_CHECK(field_eq(5, "set-cookie",
        "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"));
}

/* Malformed inputs must be rejected, never overrun. */
static void test_errors(void)
{
    CNHpackDec d;
    unsigned char buf[8];

    CN_HpackDecInit(&d, 4096);
    /* indexed header field with index 0 is illegal */
    buf[0] = 0x80;
    CN_CHECK(CN_HpackDecode(&d, buf, 1, collect, 0) == kCNErrHpackBadIndex);

    /* truncated literal: 0x40 (incr, new name) then a length but no bytes */
    buf[0] = 0x40; buf[1] = 0x05;
    CN_CHECK(CN_HpackDecode(&d, buf, 2, collect, 0) == kCNErrHpackIncomplete);

    /* indexed reference past the (empty) dynamic table */
    CN_HpackDecInit(&d, 4096);
    buf[0] = 0xBF;  /* index 63 -> dyn pos 1, table empty */
    CN_CHECK(CN_HpackDecode(&d, buf, 1, collect, 0) == kCNErrHpackBadIndex);

    /* dynamic table size update above the advertised limit */
    CN_HpackDecInit(&d, 256);
    buf[0] = 0x3F; buf[1] = 0xE1; buf[2] = 0x1F; /* 0x20 prefix, value 4321 > 256 */
    CN_CHECK(CN_HpackDecode(&d, buf, 3, collect, 0) == kCNErrHpackOverflow);
}

/* The minimal encoder round-trips through our own decoder. */
static void test_encode_roundtrip(void)
{
    CNHpackDec d;
    unsigned char out[256];
    UInt32 len = 0;

    CN_CHECK(CN_HpackEncodeField(":method", 7, "GET", 3, out, sizeof(out), &len) == noErr);
    CN_CHECK(CN_HpackEncodeField(":path", 5, "/index.html", 11, out, sizeof(out), &len) == noErr);
    CN_CHECK(CN_HpackEncodeField("x-custom", 8, "hello", 5, out, sizeof(out), &len) == noErr);

    CN_HpackDecInit(&d, 4096);
    g_count = 0;
    CN_CHECK(CN_HpackDecode(&d, out, len, collect, 0) == noErr);
    CN_CHECK(g_count == 3);
    CN_CHECK(field_eq(0, ":method", "GET"));
    CN_CHECK(field_eq(1, ":path", "/index.html"));
    CN_CHECK(field_eq(2, "x-custom", "hello"));
}

int main(void)
{
    CN_RUN(test_c3_requests);
    CN_RUN(test_c4_huffman);
    CN_RUN(test_c5_eviction);
    CN_RUN(test_errors);
    CN_RUN(test_encode_roundtrip);
    return CN_SUMMARY();
}
