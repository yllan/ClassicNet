#include "classicnet/cn_ot.h"

#ifdef CN_WITH_OT

#include "classicnet/cn_errors.h"

enum { OT_ST_CONNECT, OT_ST_CONNECTING, OT_ST_CONNECTED, OT_ST_ERR };

/* Reference-counted so several connections (e.g. a background sync thread + the
 * main thread) can each open/close without CloseOpenTransport tearing down OT --
 * and every endpoint -- out from under the others. Only the first Startup inits
 * and the last Shutdown closes. */
static long g_ot_refs = 0;
OSStatus CN_OTStartup(void)
{
    OSStatus e;
    if (g_ot_refs > 0) { g_ot_refs++; return noErr; }
    e = InitOpenTransport();
    if (e == noErr) g_ot_refs = 1;
    return e;
}
void CN_OTShutdown(void)
{
    if (g_ot_refs <= 0) return;
    if (--g_ot_refs == 0) CloseOpenTransport();
}

/* Build "host:port" without stdio (OTInitDNSAddress wants this form). */
static void make_hostport(char *dst, UInt32 cap, const char *host, UInt16 port)
{
    UInt32 i = 0;
    const char *p = host;
    char tmp[6];
    int n = 0;
    UInt16 v = port;

    while (*p != '\0' && i + 7 < cap) dst[i++] = *p++;
    if (i + 1 < cap) dst[i++] = ':';
    if (v == 0) tmp[n++] = '0';
    while (v != 0 && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v = (UInt16)(v / 10); }
    while (n > 0 && i + 1 < cap) dst[i++] = tmp[--n];
    dst[i] = '\0';
}

static OSStatus ot_poll(CNTransport *bt)
{
    CNOTTransport *t = (CNOTTransport *)bt;

    switch (t->state) {
    case OT_ST_CONNECT: {
        TCall sndCall;
        DNSAddress dns;
        OTResult res;
        OTMemzero(&sndCall, sizeof(sndCall));
        sndCall.addr.buf = (UInt8 *)&dns;
        sndCall.addr.len = OTInitDNSAddress(&dns, t->hostport);
        res = OTConnect(t->ep, &sndCall, NULL);
        if (res == noErr) { t->state = OT_ST_CONNECTED; return noErr; }
        if (res == kOTNoDataErr) { t->state = OT_ST_CONNECTING; return kCNErrWouldBlock; }
        t->state = OT_ST_ERR;
        return kCNErrNetIo;
    }
    case OT_ST_CONNECTING: {
        OTResult look = OTLook(t->ep);
        if (look == T_CONNECT) {
            OTRcvConnect(t->ep, NULL);
            t->state = OT_ST_CONNECTED;
            return noErr;
        }
        if (look == T_DISCONNECT) {
            OTRcvDisconnect(t->ep, NULL);
            t->state = OT_ST_ERR;
            return kCNErrNetIo;
        }
        return kCNErrWouldBlock;
    }
    case OT_ST_CONNECTED:
        return noErr;
    default:
        return kCNErrNetIo;
    }
}

static OSStatus ot_send(CNTransport *bt, const void *data, UInt32 len, UInt32 *sent)
{
    CNOTTransport *t = (CNOTTransport *)bt;
    OTResult r = OTSnd(t->ep, (void *)data, len, 0);
    if (r >= 0) { *sent = (UInt32)r; return noErr; }
    if (r == kOTFlowErr || r == kOTNoDataErr || r == kOTLookErr) { *sent = 0; return noErr; }
    return kCNErrNetIo;
}

static OSStatus ot_recv(CNTransport *bt, void *buf, UInt32 cap, UInt32 *got, Boolean *eof)
{
    CNOTTransport *t = (CNOTTransport *)bt;
    OTFlags flags = 0;
    OTResult r;

    *eof = false;
    r = OTRcv(t->ep, buf, cap, &flags);
    if (r > 0) { *got = (UInt32)r; return noErr; }
    if (r == kOTNoDataErr) { *got = 0; return noErr; }
    if (r == kOTLookErr) {
        OTResult look = OTLook(t->ep);
        *got = 0;
        if (look == T_ORDREL) { OTRcvOrderlyDisconnect(t->ep); *eof = true; }
        else if (look == T_DISCONNECT) { OTRcvDisconnect(t->ep, NULL); *eof = true; }
        return noErr;
    }
    if (r == 0) { *got = 0; *eof = true; return noErr; }
    return kCNErrNetIo;
}

static void ot_close(CNTransport *bt)
{
    CNOTTransport *t = (CNOTTransport *)bt;
    if (t->ep != NULL && t->ep != kOTInvalidEndpointRef) {
        OTSndOrderlyDisconnect(t->ep);
        OTUnbind(t->ep);
        OTCloseProvider(t->ep);
        t->ep = kOTInvalidEndpointRef;
    }
}

OSStatus CN_OTCreate(CNOTTransport *t, const char *host, UInt16 port,
                     CNTransport **out)
{
    OSStatus err = noErr;

    if (t == NULL || host == NULL || out == NULL)
        return kCNErrBadParam;

    t->ep = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, NULL, &err);
    if (err != noErr || t->ep == kOTInvalidEndpointRef)
        return kCNErrNetIo;

    OTSetSynchronous(t->ep);
    OTSetNonBlocking(t->ep);
    OTUseSyncIdleEvents(t->ep, false);
    if (OTBind(t->ep, NULL, NULL) != noErr) {
        OTCloseProvider(t->ep);
        return kCNErrNetIo;
    }

    make_hostport(t->hostport, (UInt32)sizeof(t->hostport), host, port);
    t->state = OT_ST_CONNECT;
    t->base.poll  = ot_poll;
    t->base.send  = ot_send;
    t->base.recv  = ot_recv;
    t->base.close = ot_close;
    *out = &t->base;
    return noErr;
}

void CN_OTDispose(CNOTTransport *t)
{
    if (t != NULL)
        ot_close(&t->base);
}

#endif /* CN_WITH_OT */
