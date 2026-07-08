/*
 * Large multi-frame HTTP/2 download against a flow-control-ENFORCING mock server.
 *
 * Reproduces the field report "hangs while downloading images, loads first 16 KB
 * only": a real CDN only sends DATA up to the receiver's advertised window and
 * waits for WINDOW_UPDATE before sending more. If the client fails to credit the
 * window (or the one-frame receive buffer drops/duplicates bytes across frame
 * boundaries), a body larger than one frame / one window stalls or corrupts.
 */
#include "classicnet/cn_h2conn.h"
#include "classicnet/cn_h2.h"
#include "classicnet/cn_hpack.h"
#include "classicnet/cn_errors.h"
#include "cn_test.h"
#include <string.h>
#include <stdio.h>

/* ---- flow-control-enforcing mock transport ---- */
typedef struct {
    CNTransport   base;
    unsigned char sent[262144];      /* client -> server (we parse WINDOW_UPDATEs) */
    UInt32        sentLen, sentSeen;  /* sentSeen = how much we've credited already */
    unsigned char in[262144];        /* server -> client (bytes the client may read) */
    UInt32        inLen, inOff;
    UInt32        chunk;              /* max bytes returned per recv (0 = unlimited) */

    /* server-side flow-control + body state */
    SInt32        connWin, streamWin; /* credit the client has granted us (bytes) */
    const unsigned char *body;
    UInt32        bodyLen, bodyOff;
    int           headersSent;
    UInt32        maxFrame;
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
    UInt32 avail = m->inLen - m->inOff, n;
    *eof = false;
    if (avail == 0) { *got = 0; return noErr; }   /* would-block, not closed */
    n = avail;
    if (m->chunk && n > m->chunk) n = m->chunk;
    if (n > cap) n = cap;
    memcpy(buf, m->in + m->inOff, n);
    m->inOff += n;
    *got = n;
    return noErr;
}

static void m_close(CNTransport *t) { (void)t; }

/* Credit windows from any WINDOW_UPDATE frames the client has newly sent. */
static void server_learn_windows(MockT *m)
{
    UInt32 off = m->sentSeen;
    /* skip the 24-byte client preface at the very start */
    if (off == 0 && m->sentLen >= CN_H2_PREFACE_LEN) off = CN_H2_PREFACE_LEN;
    while (off + CN_H2_FRAME_HDR_LEN <= m->sentLen) {
        CNH2FrameHeader h;
        CN_H2ParseFrameHeader(m->sent + off, m->sentLen - off, &h);
        if (off + CN_H2_FRAME_HDR_LEN + h.length > m->sentLen) break;   /* partial */
        if (h.type == kCNH2WindowUpdate && h.length == 4) {
            const unsigned char *p = m->sent + off + CN_H2_FRAME_HDR_LEN;
            UInt32 inc = ((UInt32)(p[0] & 0x7F) << 24) | ((UInt32)p[1] << 16)
                       | ((UInt32)p[2] << 8) | (UInt32)p[3];
            if (h.streamId == 0) m->connWin += (SInt32)inc;
            else                 m->streamWin += (SInt32)inc;
        }
        off += CN_H2_FRAME_HDR_LEN + h.length;
    }
    m->sentSeen = off;
}

/* Emit HEADERS (once) then as many DATA frames as the flow-control windows
 * currently allow. Mirrors a real server: never exceed min(conn,stream) window. */
static void server_produce_stream(MockT *m, UInt32 sid)
{
    unsigned char frame[CN_H2_FRAME_HDR_LEN + 16384];
    server_learn_windows(m);
    if (m->inOff > 0) {                    /* drop bytes the client already read */
        memmove(m->in, m->in + m->inOff, m->inLen - m->inOff);
        m->inLen -= m->inOff; m->inOff = 0;
    }

    if (!m->headersSent) {
        unsigned char hp[256]; UInt32 hl = 0, fl = 0;
        CN_HpackEncodeField(":status", 7, "200", 3, hp, sizeof(hp), &hl);
        CN_HpackEncodeField("content-type", 12, "image/jpeg", 10, hp, sizeof(hp), &hl);
        CN_H2BuildFrame(kCNH2Headers, kCNH2FlagEndHeaders, sid, hp, hl,
                        frame, sizeof(frame), &fl);
        memcpy(m->in + m->inLen, frame, fl); m->inLen += fl;
        m->headersSent = 1;
    }
    for (;;) {
        UInt32 remain = m->bodyLen - m->bodyOff, chunk, fl = 0;
        SInt32 win = m->connWin < m->streamWin ? m->connWin : m->streamWin;
        Boolean last;
        if (remain == 0) break;
        if (win <= 0) break;                       /* stalled: await WINDOW_UPDATE */
        chunk = remain;
        if (chunk > m->maxFrame)    chunk = m->maxFrame;
        if (chunk > (UInt32)win)    chunk = (UInt32)win;
        last = (Boolean)(m->bodyOff + chunk >= m->bodyLen);
        CN_H2BuildFrame(kCNH2Data, last ? kCNH2FlagEndStream : 0, sid,
                        m->body + m->bodyOff, chunk, frame, sizeof(frame), &fl);
        memcpy(m->in + m->inLen, frame, fl); m->inLen += fl;
        m->bodyOff += chunk;
        m->connWin -= (SInt32)chunk;
        m->streamWin -= (SInt32)chunk;
    }
}

/* ---- client-side collector: verify EVERY byte, not just the count ---- */
static unsigned char g_expect[262144];
static UInt32 g_recv;
static int    g_mismatch;
static int    g_done;
static OSStatus g_result;

static void d_resp(CNH2Conn *c, const CNH2Response *r, void *ud)
{ (void)c; (void)ud; CN_CHECK(r->status == 200); }
static Boolean d_data(CNH2Conn *c, const void *b, UInt32 len, void *ud)
{
    (void)c; (void)ud;
    if (memcmp(b, g_expect + g_recv, len) != 0) g_mismatch = 1;
    g_recv += len;
    return true;
}
static void d_done(CNH2Conn *c, OSStatus result, void *ud)
{ (void)c; (void)ud; g_done = 1; g_result = result; }

static void download_case(UInt32 bodyLen, UInt32 recvChunk, UInt32 serverMaxFrame)
{
    MockT m;
    CNH2Conn c;
    CNH2Callbacks cb;
    UInt32 i, id, guard = 0;
    unsigned char sbuf[64];
    UInt32 sn = 0;

    memset(&m, 0, sizeof(m));
    m.base.poll = m_poll; m.base.send = m_send; m.base.recv = m_recv; m.base.close = m_close;
    m.chunk = recvChunk;
    m.connWin = 65535; m.streamWin = 65535;     /* HTTP/2 defaults (client sets no override) */
    m.maxFrame = serverMaxFrame;

    for (i = 0; i < bodyLen; i++) g_expect[i] = (unsigned char)((i * 31u + 7u) & 0xFF);
    m.body = g_expect; m.bodyLen = bodyLen; m.bodyOff = 0; m.headersSent = 0;

    g_recv = 0; g_mismatch = 0; g_done = 0; g_result = 999;
    cb.onResponse = d_resp; cb.onData = d_data; cb.onComplete = d_done;

    CN_CHECK(CN_H2ConnStart(&c, &m.base) == noErr);
    /* server SETTINGS first (so the client's SETTINGS handshake completes) */
    CN_H2BuildSettings(0, 0, sbuf, sizeof(sbuf), &sn);
    memcpy(m.in + m.inLen, sbuf, sn); m.inLen += sn;

    CN_CHECK(CN_H2Request(&c, "https", "cdn.example", "/img.jpg", 0, 0, &cb, 0, &id) == noErr);

    /* Drive client and server in lockstep: each turn the server credits itself
     * from the client's newly-sent WINDOW_UPDATEs and produces more DATA. */
    while (!g_done && guard++ < 2000000) {
        server_produce_stream(&m, 1);
        CN_H2Pump(&c);
    }

    printf("    body=%lu chunk=%lu srvFrame=%lu -> recv=%lu done=%d result=%ld mismatch=%d bodyOff=%lu\n",
           (unsigned long)bodyLen, (unsigned long)recvChunk, (unsigned long)serverMaxFrame,
           (unsigned long)g_recv, g_done, (long)g_result, g_mismatch, (unsigned long)m.bodyOff);

    CN_CHECK(g_done == 1);
    CN_CHECK(g_result == noErr);
    CN_CHECK(g_recv == bodyLen);       /* every byte arrived */
    CN_CHECK(g_mismatch == 0);         /* and in the right order/content */
}

static void test_download_flowcontrolled(void)
{
    /* one frame (baseline) */
    download_case(16384, 0, 16384);
    /* just over one frame */
    download_case(16385, 0, 16384);
    /* just over the initial 64 KB window: needs WINDOW_UPDATE to keep flowing */
    download_case(70000, 0, 16384);
    /* a real ~180 KB image, unlimited recv */
    download_case(180000, 0, 16384);
    /* same, but recv delivers one TLS record at a time */
    download_case(180000, 16384, 16384);
    /* same, but recv delivers TCP-MSS-sized chunks */
    download_case(180000, 1400, 16384);
    /* pathological tiny recv chunks (reassembly across every boundary) */
    download_case(70000, 61, 16384);
    /* server uses smaller frames */
    download_case(180000, 0, 4096);
}

/* Many sequential downloads on ONE reused connection, with the server tracking
 * the connection-level flow-control window PERSISTENTLY across streams (a real
 * server never resets it). This is the CDN side-channel's exact usage: one
 * connection, avatar/thumbnail/image after image. If the client under-credits
 * the connection window on any DATA frame (notably the END_STREAM one), the
 * connection window leaks a little per response and, after a handful of images,
 * min(conn,stream) hits 0 and every later download stalls -- "loads 16 KB, hangs".
 */
static void reuse_run_one(MockT *m, CNH2Conn *c, UInt32 bodyLen, UInt32 recvChunk)
{
    CNH2Callbacks cb;
    UInt32 i, id, guard = 0;
    m->chunk = recvChunk;
    m->streamWin = 65535;                 /* each NEW stream: fresh stream window */
    m->maxFrame = 16384;                  /* connWin persists across calls (NOT reset) */
    for (i = 0; i < bodyLen; i++) g_expect[i] = (unsigned char)((i * 31u + 7u) & 0xFF);
    m->body = g_expect; m->bodyLen = bodyLen; m->bodyOff = 0; m->headersSent = 0;
    g_recv = 0; g_mismatch = 0; g_done = 0; g_result = 999;
    cb.onResponse = d_resp; cb.onData = d_data; cb.onComplete = d_done;
    CN_CHECK(CN_H2Request(c, "https", "cdn.example", "/img.jpg", 0, 0, &cb, 0, &id) == noErr);
    while (!g_done && guard++ < 200000) {
        server_produce_stream(m, id);
        CN_H2Pump(c);
    }
    CN_CHECK(g_done == 1 && g_result == noErr && g_recv == bodyLen && g_mismatch == 0);
}

static void test_download_reuse_connwindow(void)
{
    MockT m;
    CNH2Conn c;
    unsigned char sbuf[64];
    UInt32 sn = 0, k;

    memset(&m, 0, sizeof(m));
    m.base.poll = m_poll; m.base.send = m_send; m.base.recv = m_recv; m.base.close = m_close;
    m.connWin = 65535;                    /* ONE persistent connection window */

    CN_CHECK(CN_H2ConnStart(&c, &m.base) == noErr);
    CN_H2BuildSettings(0, 0, sbuf, sizeof(sbuf), &sn);
    memcpy(m.in + m.inLen, sbuf, sn); m.inLen += sn;

    /* 15 small single-frame images (8 KB each) -- avatars/thumbnails, exactly
     * what a timeline loads first. Each response is ONE END_STREAM DATA frame, so
     * a client that skips the connection WINDOW_UPDATE on END_STREAM frames credits
     * NOTHING back and leaks the full 8 KB per image. The 64 KB connection window
     * drains in ~8 images; the next download then finds a zero window, receives a
     * partial body, and deadlocks forever (no data -> no WINDOW_UPDATE) -- the
     * exact "loads first N KB, then hangs" field report. */
    for (k = 0; k < 20; k++) {
        reuse_run_one(&m, &c, 8000, 0);
        printf("    reuse image %lu: recv=%lu done=%d connWin now %ld\n",
               (unsigned long)k, (unsigned long)g_recv, g_done, (long)m.connWin);
    }
}

int main(void)
{
    CN_RUN(test_download_flowcontrolled);
    CN_RUN(test_download_reuse_connwindow);
    return CN_SUMMARY();
}
