/*
 * End-to-end test for the HTTP/2 connection layer (cn_h2conn), driven by a mock
 * transport that plays the server: it captures what the client sends and feeds
 * back a hand-built h2 response (SETTINGS, PING, HEADERS, DATA) -- in tiny recv
 * chunks, to exercise frame reassembly and buffer compaction.
 */
#include "classicnet/cn_h2conn.h"
#include "classicnet/cn_h2.h"
#include "classicnet/cn_hpack.h"
#include "classicnet/cn_errors.h"
#include "cn_test.h"

#include <string.h>

/* ---- mock transport ---- */
typedef struct {
    CNTransport   base;
    unsigned char sent[120000];
    UInt32        sentLen;
    unsigned char in[8192];
    UInt32        inLen, inOff;
    UInt32        chunk;          /* max bytes returned per recv */
} MockT;

static OSStatus m_poll(CNTransport *t) { (void)t; return noErr; }

static OSStatus m_send(CNTransport *t, const void *data, UInt32 len, UInt32 *sent)
{
    MockT *m = (MockT *)t;
    if (m->sentLen + len > sizeof(m->sent)) len = sizeof(m->sent) - m->sentLen;
    memcpy(m->sent + m->sentLen, data, len);
    m->sentLen += len;
    *sent = len;
    return noErr;
}

static OSStatus m_recv(CNTransport *t, void *buf, UInt32 cap, UInt32 *got, Boolean *eof)
{
    MockT *m = (MockT *)t;
    UInt32 avail = m->inLen - m->inOff;
    UInt32 n;
    *eof = false;
    if (avail == 0) { *got = 0; *eof = false; return noErr; }   /* empty now != closed */
    n = avail;
    if (m->chunk && n > m->chunk) n = m->chunk;
    if (n > cap) n = cap;
    memcpy(buf, m->in + m->inOff, n);
    m->inOff += n;
    *got = n;
    return noErr;
}

static void m_close(CNTransport *t) { (void)t; }

static void mock_init(MockT *m, UInt32 chunk)
{
    memset(m, 0, sizeof(*m));
    m->base.poll = m_poll; m->base.send = m_send;
    m->base.recv = m_recv; m->base.close = m_close;
    m->chunk = chunk;
}

/* append raw bytes to the server->client stream */
static void srv_raw(MockT *m, const unsigned char *p, UInt32 n)
{
    memcpy(m->in + m->inLen, p, n);
    m->inLen += n;
}

/* ---- collected client-side results ---- */
static UInt16 g_status;
static char   g_ctype[128];
static char   g_body[256];
static UInt32 g_bodyLen;
static OSStatus g_result;
static int    g_completed;

static void on_resp(CNH2Conn *c, const CNH2Response *r, void *ud)
{
    UInt8 i;
    (void)c; (void)ud;
    g_status = r->status;
    for (i = 0; i < r->headerCount; i++)
        if (strcmp(r->headers[i].name, "content-type") == 0)
            strcpy(g_ctype, r->headers[i].value);
}
static Boolean on_data(CNH2Conn *c, const void *b, UInt32 len, void *ud)
{
    (void)c; (void)ud;
    if (g_bodyLen + len < sizeof(g_body)) {
        memcpy(g_body + g_bodyLen, b, len);
        g_bodyLen += len;
        g_body[g_bodyLen] = 0;
    }
    return true;
}
static void on_done(CNH2Conn *c, OSStatus result, void *ud)
{
    (void)c; (void)ud;
    g_result = result;
    g_completed++;
}

/* Build a server HEADERS frame (END_HEADERS) carrying :status + content-type. */
static UInt32 build_headers(unsigned char *out, UInt32 cap, Boolean endStream)
{
    unsigned char hp[256];
    UInt32 hl = 0, fl = 0;
    UInt8 flags = kCNH2FlagEndHeaders | (endStream ? kCNH2FlagEndStream : 0);
    CN_HpackEncodeField(":status", 7, "200", 3, hp, sizeof(hp), &hl);
    CN_HpackEncodeField("content-type", 12, "text/plain", 10, hp, sizeof(hp), &hl);
    CN_H2BuildFrame(kCNH2Headers, flags, 1, hp, hl, out, cap, &fl);
    return fl;
}

static void run(MockT *m, CNH2Conn *c)
{
    CNH2Callbacks cb;
    int guard = 0;
    cb.onResponse = on_resp; cb.onData = on_data; cb.onComplete = on_done;
    g_status = 0; g_ctype[0] = 0; g_body[0] = 0; g_bodyLen = 0;
    g_result = 999; g_completed = 0;

    CN_CHECK(CN_H2Get(c, &m->base, "https", "example.com", "/",
                      0, 0, &cb, 0) == noErr);
    while (!CN_H2Done(c) && guard++ < 10000)
        CN_H2Pump(c);
}

static void test_basic_get(void)
{
    MockT m;
    CNH2Conn c;
    unsigned char buf[512];
    UInt32 n;

    mock_init(&m, 5);                       /* 5 bytes per recv: force reassembly */

    /* server: SETTINGS, HEADERS(:status 200), DATA "hello h2" + END_STREAM */
    n = 0;
    { CNH2Setting s; s.id = kCNH2SettingMaxConcurrentStreams; s.value = 100;
      CN_H2BuildSettings(&s, 1, buf, sizeof(buf), &n); srv_raw(&m, buf, n); }
    n = build_headers(buf, sizeof(buf), false);
    srv_raw(&m, buf, n);
    CN_H2BuildFrame(kCNH2Data, kCNH2FlagEndStream, 1,
                    (const unsigned char *)"hello h2", 8, buf, sizeof(buf), &n);
    srv_raw(&m, buf, n);

    run(&m, &c);

    CN_CHECK(g_completed == 1);
    CN_CHECK(g_result == noErr);
    CN_CHECK(g_status == 200);
    CN_CHECK(strcmp(g_ctype, "text/plain") == 0);
    CN_CHECK(g_bodyLen == 8);
    CN_CHECK(memcmp(g_body, "hello h2", 8) == 0);

    /* the client must have opened with the 24-byte connection preface */
    CN_CHECK(m.sentLen >= CN_H2_PREFACE_LEN);
    CN_CHECK(memcmp(m.sent, CN_H2_PREFACE, CN_H2_PREFACE_LEN) == 0);
}

/* A scan over m.sent for a frame of the given type+flags. */
static int sent_has_frame(MockT *m, UInt8 type, UInt8 flags)
{
    UInt32 off = CN_H2_PREFACE_LEN;          /* skip the preface */
    while (off + CN_H2_FRAME_HDR_LEN <= m->sentLen) {
        CNH2FrameHeader h;
        CN_H2ParseFrameHeader(m->sent + off, m->sentLen - off, &h);
        if (off + CN_H2_FRAME_HDR_LEN + h.length > m->sentLen) break;
        if (h.type == type && (h.flags & flags) == flags) return 1;
        off += CN_H2_FRAME_HDR_LEN + h.length;
    }
    return 0;
}

static void test_settings_ack_and_ping(void)
{
    MockT m;
    CNH2Conn c;
    unsigned char buf[512], ping[CN_H2_FRAME_HDR_LEN + 8];
    UInt32 n;

    mock_init(&m, 0);                        /* deliver all at once */

    n = 0;
    CN_H2BuildSettings(0, 0, buf, sizeof(buf), &n); srv_raw(&m, buf, n);  /* server SETTINGS */
    CN_H2BuildFrame(kCNH2Ping, 0, 0, (const unsigned char *)"\x01\x02\x03\x04\x05\x06\x07\x08",
                    8, ping, sizeof(ping), &n); srv_raw(&m, ping, n);     /* server PING */
    n = build_headers(buf, sizeof(buf), true);                            /* HEADERS + END_STREAM */
    srv_raw(&m, buf, n);

    run(&m, &c);

    CN_CHECK(g_result == noErr);
    CN_CHECK(g_status == 200);
    /* client must have ACK'd the server SETTINGS and the PING */
    CN_CHECK(sent_has_frame(&m, kCNH2Settings, kCNH2FlagAck));
    CN_CHECK(sent_has_frame(&m, kCNH2Ping, kCNH2FlagAck));
}

static void test_goaway_fails(void)
{
    MockT m;
    CNH2Conn c;
    unsigned char buf[512];
    UInt32 n;

    mock_init(&m, 0);
    n = 0;
    CN_H2BuildSettings(0, 0, buf, sizeof(buf), &n); srv_raw(&m, buf, n);
    /* GOAWAY (8-byte payload: last-stream-id + error code) ends the connection */
    CN_H2BuildFrame(kCNH2Goaway, 0, 0,
                    (const unsigned char *)"\0\0\0\0\0\0\0\0", 8, buf, sizeof(buf), &n);
    srv_raw(&m, buf, n);

    run(&m, &c);
    CN_CHECK(g_completed == 1);
    CN_CHECK(g_result == kCNErrH2StreamError);
}

/* ---- N-way multiplexing ---- */
typedef struct {
    UInt32   id;
    UInt16   status;
    char     body[64];
    UInt32   bodyLen;
    int      done;
    OSStatus result;
} Req;

static void mx_resp(CNH2Conn *c, const CNH2Response *r, void *ud)
{ Req *q = (Req *)ud; (void)c; q->status = r->status; }
static Boolean mx_data(CNH2Conn *c, const void *b, UInt32 len, void *ud)
{
    Req *q = (Req *)ud; (void)c;
    if (q->bodyLen + len < sizeof(q->body)) {
        memcpy(q->body + q->bodyLen, b, len);
        q->bodyLen += len; q->body[q->bodyLen] = 0;
    }
    return true;
}
static void mx_done(CNH2Conn *c, OSStatus result, void *ud)
{ Req *q = (Req *)ud; (void)c; q->done = 1; q->result = result; }

/* server HEADERS (no END_STREAM; body follows) for a given stream */
static UInt32 srv_headers(unsigned char *out, UInt32 cap, UInt32 sid)
{
    unsigned char hp[128]; UInt32 hl = 0, fl = 0;
    CN_HpackEncodeField(":status", 7, "200", 3, hp, sizeof(hp), &hl);
    CN_HpackEncodeField("content-type", 12, "text/plain", 10, hp, sizeof(hp), &hl);
    CN_H2BuildFrame(kCNH2Headers, kCNH2FlagEndHeaders, sid, hp, hl, out, cap, &fl);
    return fl;
}
static UInt32 srv_data(unsigned char *out, UInt32 cap, UInt32 sid,
                       const char *body, UInt32 len)
{
    UInt32 fl = 0;
    CN_H2BuildFrame(kCNH2Data, kCNH2FlagEndStream, sid,
                    (const unsigned char *)body, len, out, cap, &fl);
    return fl;
}

static void test_multiplex(void)
{
    MockT m;
    CNH2Conn c;
    CNH2Callbacks cb;
    Req r1, r3, r5;
    unsigned char buf[256];
    UInt32 n, id;
    int guard = 0;

    mock_init(&m, 7);                        /* 7-byte recv chunks: interleave + reassembly */
    cb.onResponse = mx_resp; cb.onData = mx_data; cb.onComplete = mx_done;
    memset(&r1, 0, sizeof(r1)); memset(&r3, 0, sizeof(r3)); memset(&r5, 0, sizeof(r5));

    /* server SETTINGS, then ALL three response header blocks, then bodies
       OUT OF ORDER (5, 1, 3) -- proves the client routes by stream id. */
    n = 0; CN_H2BuildSettings(0, 0, buf, sizeof(buf), &n); srv_raw(&m, buf, n);
    srv_raw(&m, buf, srv_headers(buf, sizeof(buf), 1));
    srv_raw(&m, buf, srv_headers(buf, sizeof(buf), 3));
    srv_raw(&m, buf, srv_headers(buf, sizeof(buf), 5));
    srv_raw(&m, buf, srv_data(buf, sizeof(buf), 5, "body-five", 9));
    srv_raw(&m, buf, srv_data(buf, sizeof(buf), 1, "body-one", 8));
    srv_raw(&m, buf, srv_data(buf, sizeof(buf), 3, "body-three", 10));

    CN_CHECK(CN_H2ConnStart(&c, &m.base) == noErr);
    CN_CHECK(CN_H2Request(&c, "https", "h", "/1", 0, 0, &cb, &r1, &id) == noErr);
    CN_CHECK(id == 1);
    CN_CHECK(CN_H2Request(&c, "https", "h", "/3", 0, 0, &cb, &r3, &id) == noErr);
    CN_CHECK(id == 3);
    CN_CHECK(CN_H2Request(&c, "https", "h", "/5", 0, 0, &cb, &r5, &id) == noErr);
    CN_CHECK(id == 5);

    while (!CN_H2Done(&c) && guard++ < 100000)
        CN_H2Pump(&c);

    /* every request completed with its own status + body, correctly routed */
    CN_CHECK(r1.done && r1.result == noErr && r1.status == 200);
    CN_CHECK(strcmp(r1.body, "body-one") == 0);
    CN_CHECK(r3.done && r3.result == noErr && r3.status == 200);
    CN_CHECK(strcmp(r3.body, "body-three") == 0);
    CN_CHECK(r5.done && r5.result == noErr && r5.status == 200);
    CN_CHECK(strcmp(r5.body, "body-five") == 0);

    /* the client must have sent three HEADERS frames (one per stream) */
    {
        int heads = 0;
        UInt32 off = CN_H2_PREFACE_LEN;
        while (off + CN_H2_FRAME_HDR_LEN <= m.sentLen) {
            CNH2FrameHeader h;
            CN_H2ParseFrameHeader(m.sent + off, m.sentLen - off, &h);
            if (off + CN_H2_FRAME_HDR_LEN + h.length > m.sentLen) break;
            if (h.type == kCNH2Headers) heads++;
            off += CN_H2_FRAME_HDR_LEN + h.length;
        }
        CN_CHECK(heads == 3);
    }
}

/* A >64KB request body must be split into <=16KB DATA frames, gated by the send
 * flow-control window until the server credits it with WINDOW_UPDATEs, with
 * END_STREAM only on the final frame. */
static void test_large_post(void)
{
    static MockT m;                          /* static: the big sent buffer is off the stack */
    static unsigned char body[100000];
    CNH2Conn c;
    CNH2Callbacks cb;
    unsigned char buf[64];
    UInt32 n, i;
    int guard = 0, credited = 0, responded = 0;

    for (i = 0; i < sizeof(body); i++) body[i] = (unsigned char)(i & 0x7F);
    mock_init(&m, 0);
    cb.onResponse = on_resp; cb.onData = on_data; cb.onComplete = on_done;
    g_status = 0; g_ctype[0] = 0; g_body[0] = 0; g_bodyLen = 0; g_result = 999; g_completed = 0;

    n = 0; CN_H2BuildSettings(0, 0, buf, sizeof(buf), &n); srv_raw(&m, buf, n);

    CN_CHECK(CN_H2ConnStart(&c, &m.base) == noErr);
    CN_CHECK(CN_H2RequestEx(&c, "POST", "https", "example.com", "/up",
                            0, 0, body, sizeof(body), &cb, 0, 0) == noErr);

    while (!CN_H2Done(&c) && guard++ < 200000) {
        CN_H2Pump(&c);
        if (!credited && m.sentLen > 60000) {           /* initial 64KB window spent: credit more */
            unsigned char inc[4];
            inc[0] = 0; inc[1] = 0x03; inc[2] = 0; inc[3] = 0;   /* +196608 */
            n = 0; CN_H2BuildFrame(kCNH2WindowUpdate, 0, 0, inc, 4, buf, sizeof(buf), &n); srv_raw(&m, buf, n);
            n = 0; CN_H2BuildFrame(kCNH2WindowUpdate, 0, 1, inc, 4, buf, sizeof(buf), &n); srv_raw(&m, buf, n);
            credited = 1;
        }
        if (!responded && m.sentLen > 100000) {         /* whole body out: send the response */
            srv_raw(&m, buf, build_headers(buf, sizeof(buf), true));
            responded = 1;
        }
    }

    CN_CHECK(g_completed == 1 && g_result == noErr && g_status == 200);
    {
        UInt32 off = CN_H2_PREFACE_LEN, total = 0;
        int dataFrames = 0, lastEnd = 0, oversize = 0;
        while (off + CN_H2_FRAME_HDR_LEN <= m.sentLen) {
            CNH2FrameHeader h;
            CN_H2ParseFrameHeader(m.sent + off, m.sentLen - off, &h);
            if (off + CN_H2_FRAME_HDR_LEN + h.length > m.sentLen) break;
            if (h.type == kCNH2Data && h.streamId == 1) {
                dataFrames++;
                total += h.length;
                if (h.length > 16384) oversize = 1;
                lastEnd = (h.flags & kCNH2FlagEndStream) ? 1 : 0;
            }
            off += CN_H2_FRAME_HDR_LEN + h.length;
        }
        CN_CHECK(total == sizeof(body));    /* whole body sent */
        CN_CHECK(dataFrames >= 7);          /* 100000 / 16384 -> chunked */
        CN_CHECK(oversize == 0);            /* every frame within the max frame size */
        CN_CHECK(lastEnd == 1);             /* END_STREAM only on the final DATA frame */
    }
}

/* A WINDOW_UPDATE with a zero increment is a stream error (RFC 7540 §6.9). */
static void test_rejects_zero_window_update(void)
{
    MockT m;
    CNH2Conn c;
    unsigned char buf[512];
    UInt32 n;

    mock_init(&m, 0);
    n = 0; CN_H2BuildSettings(0, 0, buf, sizeof(buf), &n); srv_raw(&m, buf, n);
    n = build_headers(buf, sizeof(buf), false);            /* :status 200, stream stays open */
    srv_raw(&m, buf, n);
    CN_H2BuildFrame(kCNH2WindowUpdate, 0, 1,
                    (const unsigned char *)"\0\0\0\0", 4, buf, sizeof(buf), &n);
    srv_raw(&m, buf, n);

    run(&m, &c);
    CN_CHECK(g_completed == 1);
    CN_CHECK(g_result == kCNErrH2StreamError);            /* zero increment killed the stream */
}

/* A SETTINGS payload whose length is not a multiple of 6 is a connection error
 * (RFC 7540 §6.5) -- previously the trailing bytes were silently ignored. */
static void test_rejects_bad_settings_length(void)
{
    MockT m;
    CNH2Conn c;
    unsigned char buf[512];
    UInt32 n;

    mock_init(&m, 0);
    CN_H2BuildFrame(kCNH2Settings, 0, 0,
                    (const unsigned char *)"\0\0\0\0\0", 5, buf, sizeof(buf), &n);
    srv_raw(&m, buf, n);

    run(&m, &c);
    CN_CHECK(g_completed == 1);
    CN_CHECK(g_result == kCNErrH2BadFrame);
}

/* Bad CN_H2Request params must be rejected, not crash: null headers array with a
 * non-zero count, or a header with a null name/value. */
static void test_rejects_bad_headers_param(void)
{
    MockT m;
    CNH2Conn c;
    CNH2Callbacks cb;
    CNHeaderKV bad;
    UInt32 id;

    mock_init(&m, 0);
    cb.onResponse = on_resp; cb.onData = on_data; cb.onComplete = on_done;
    CN_CHECK(CN_H2ConnStart(&c, &m.base) == noErr);
    CN_CHECK(CN_H2Request(&c, "https", "h", "/", 0, 1, &cb, 0, &id) == kCNErrBadParam);
    bad.name = 0; bad.value = "v";
    CN_CHECK(CN_H2Request(&c, "https", "h", "/", &bad, 1, &cb, 0, &id) == kCNErrBadParam);
    bad.name = "x"; bad.value = 0;
    CN_CHECK(CN_H2Request(&c, "https", "h", "/", &bad, 1, &cb, 0, &id) == kCNErrBadParam);
}

/* If the HEADERS frame can't fit the outbound queue, open_stream must queue
 * NOTHING (no dangling frame header) and report overflow. */
static void test_headers_overflow_atomic(void)
{
    MockT m;
    CNH2Conn c;
    CNH2Callbacks cb;
    UInt32 id, before;

    mock_init(&m, 0);
    cb.onResponse = on_resp; cb.onData = on_data; cb.onComplete = on_done;
    CN_CHECK(CN_H2ConnStart(&c, &m.base) == noErr);
    c.outLen = CN_H2_OUT_CAP - 4;            /* leave only 4 bytes free */
    before = c.outLen;
    CN_CHECK(CN_H2Request(&c, "https", "example.com", "/", 0, 0, &cb, 0, &id)
             == kCNErrBufferOverflow);
    CN_CHECK(c.outLen == before);            /* nothing partially queued */
}

/* A SETTINGS_INITIAL_WINDOW_SIZE delta that pushes an open stream's send window
 * over 2^31-1 is a connection error (RFC 7540 §6.9.2). */
static void test_rejects_initial_window_overflow(void)
{
    MockT m;
    CNH2Conn c;
    unsigned char buf[512];
    UInt32 n;

    mock_init(&m, 0);
    n = 0; CN_H2BuildSettings(0, 0, buf, sizeof(buf), &n); srv_raw(&m, buf, n);
    n = build_headers(buf, sizeof(buf), false);            /* stream 1 stays open */
    srv_raw(&m, buf, n);
    /* raise stream 1's window to exactly 2^31-1 (65535 + 0x7FFF0000) */
    CN_H2BuildFrame(kCNH2WindowUpdate, 0, 1,
                    (const unsigned char *)"\x7f\xff\x00\x00", 4, buf, sizeof(buf), &n);
    srv_raw(&m, buf, n);
    /* then grow the initial window: delta applied to stream 1 exceeds the max */
    { CNH2Setting s; s.id = kCNH2SettingInitialWindowSize; s.value = 100000;
      n = 0; CN_H2BuildSettings(&s, 1, buf, sizeof(buf), &n); srv_raw(&m, buf, n); }

    run(&m, &c);
    CN_CHECK(g_completed == 1);
    CN_CHECK(g_result == kCNErrH2BadFrame);
}

/* DATA on stream 0 is a connection error (RFC 7540 §6.1). */
static void test_rejects_data_on_stream0(void)
{
    MockT m;
    CNH2Conn c;
    unsigned char buf[512];
    UInt32 n;

    mock_init(&m, 0);
    n = 0; CN_H2BuildSettings(0, 0, buf, sizeof(buf), &n); srv_raw(&m, buf, n);
    n = build_headers(buf, sizeof(buf), false); srv_raw(&m, buf, n);
    CN_H2BuildFrame(kCNH2Data, 0, 0, (const unsigned char *)"x", 1, buf, sizeof(buf), &n);
    srv_raw(&m, buf, n);

    run(&m, &c);
    CN_CHECK(g_completed == 1);
    CN_CHECK(g_result == kCNErrH2BadFrame);
}

/* A PRIORITY frame whose length is not 5 is a frame-size error (RFC 7540 §6.3). */
static void test_rejects_bad_priority_length(void)
{
    MockT m;
    CNH2Conn c;
    unsigned char buf[512];
    UInt32 n;

    mock_init(&m, 0);
    n = 0; CN_H2BuildSettings(0, 0, buf, sizeof(buf), &n); srv_raw(&m, buf, n);
    CN_H2BuildFrame(kCNH2Priority, 0, 1, (const unsigned char *)"\0\0\0", 3, buf, sizeof(buf), &n);
    srv_raw(&m, buf, n);

    run(&m, &c);
    CN_CHECK(g_completed == 1);
    CN_CHECK(g_result == kCNErrH2BadFrame);
}

/* Response HEADERS without a :status pseudo-header is malformed: the stream must
   fail, not complete as success with status 0 (RFC 7540 §8.1.2.4). */
static void test_rejects_missing_status(void)
{
    MockT m;
    CNH2Conn c;
    unsigned char buf[512], hp[64];
    UInt32 n, hl = 0;

    mock_init(&m, 0);
    n = 0; CN_H2BuildSettings(0, 0, buf, sizeof(buf), &n); srv_raw(&m, buf, n);
    CN_HpackEncodeField("x-foo", 5, "bar", 3, hp, sizeof(hp), &hl);   /* no :status */
    CN_H2BuildFrame(kCNH2Headers, kCNH2FlagEndHeaders | kCNH2FlagEndStream, 1,
                    hp, hl, buf, sizeof(buf), &n);
    srv_raw(&m, buf, n);

    run(&m, &c);
    CN_CHECK(g_completed == 1);
    CN_CHECK(g_result == kCNErrH2BadFrame);
    CN_CHECK(g_status == 0);            /* never delivered a bogus status-0 response */
}

int main(void)
{
    CN_RUN(test_basic_get);
    CN_RUN(test_settings_ack_and_ping);
    CN_RUN(test_goaway_fails);
    CN_RUN(test_multiplex);
    CN_RUN(test_large_post);
    CN_RUN(test_rejects_zero_window_update);
    CN_RUN(test_rejects_bad_settings_length);
    CN_RUN(test_rejects_bad_headers_param);
    CN_RUN(test_headers_overflow_atomic);
    CN_RUN(test_rejects_initial_window_overflow);
    CN_RUN(test_rejects_data_on_stream0);
    CN_RUN(test_rejects_bad_priority_length);
    CN_RUN(test_rejects_missing_status);
    return CN_SUMMARY();
}
