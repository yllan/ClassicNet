/*
 * libFuzzer harness for the HTTP/2 connection layer: the fuzz bytes are the
 * server's response stream, fed to the client pump through a mock transport.
 * Exercises the frame reader, HEADERS/CONTINUATION reassembly, padding math,
 * and HPACK decode. Build via scripts/run-fuzz.sh h2conn
 */
#include "classicnet/cn_h2conn.h"

#include <string.h>
#include <stddef.h>

typedef struct {
    CNTransport          base;
    const unsigned char *in;
    UInt32               len, off;
} FuzzT;

static OSStatus f_poll(CNTransport *t) { (void)t; return noErr; }
static OSStatus f_send(CNTransport *t, const void *d, UInt32 n, UInt32 *sent)
{ (void)t; (void)d; *sent = n; return noErr; }            /* accept and drop */
static OSStatus f_recv(CNTransport *t, void *buf, UInt32 cap, UInt32 *got, Boolean *eof)
{
    FuzzT *f = (FuzzT *)t;
    UInt32 n = f->len - f->off;
    *eof = false;
    if (n == 0) { *got = 0; *eof = true; return noErr; }
    if (n > cap) n = cap;
    if (n > 7) n = 7;                                     /* small chunks: reassembly */
    memcpy(buf, f->in + f->off, n);
    f->off += n; *got = n;
    return noErr;
}
static void f_close(CNTransport *t) { (void)t; }

static void sink_data_noop(void) {}
static void on_resp(CNH2Conn *c, const CNH2Response *r, void *ud)
{ (void)c; (void)r; (void)ud; }
static Boolean on_data(CNH2Conn *c, const void *b, UInt32 l, void *ud)
{ (void)c; (void)b; (void)l; (void)ud; return true; }
static void on_done(CNH2Conn *c, OSStatus s, void *ud)
{ (void)c; (void)s; (void)ud; }

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    FuzzT f;
    CNH2Conn c;
    CNH2Callbacks cb;
    int guard = 0;

    (void)sink_data_noop;
    memset(&f, 0, sizeof(f));
    f.base.poll = f_poll; f.base.send = f_send; f.base.recv = f_recv; f.base.close = f_close;
    f.in = data; f.len = (UInt32)size; f.off = 0;

    cb.onResponse = on_resp; cb.onData = on_data; cb.onComplete = on_done;
    if (CN_H2Get(&c, &f.base, "https", "h", "/", 0, 0, &cb, 0) != noErr)
        return 0;
    while (!CN_H2Done(&c) && guard++ < 200000)
        CN_H2Pump(&c);
    return 0;
}
