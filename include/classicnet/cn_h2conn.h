#ifndef CLASSICNET_CN_H2CONN_H
#define CLASSICNET_CN_H2CONN_H

#include "cn_types.h"
#include "cn_http.h"        /* CNHeaderField, CNHeaderKV, CN_HTTP_MAX_* */
#include "cn_hpack.h"
#include "cn_transport.h"

/*
 * HTTP/2 connection layer: drive a single GET over one stream, on top of any
 * CNTransport (TLS-over-OT on the target, a loopback on the host).  Same
 * cooperative pump/callback shape as CNRequest -- CN_H2Pump returns promptly
 * and is called from the app's event loop.
 *
 * Step 1 scope: one active stream (stream 1).  The frame reader, HPACK
 * reassembly, flow-control top-ups, and SETTINGS/PING handshakes are all here;
 * true N-way multiplexing layers on later by turning the single-stream fields
 * into an array.
 */

/* Buffer sizes.  RECV must hold one frame at the default 16384 max frame size;
   tune down on RAM-tight targets only if you also advertise a smaller cap. */
#define CN_H2_RECV_CAP   (16384u + 9u)
#define CN_H2_HDRBLK_CAP  16384u
#define CN_H2_OUT_CAP      2048u   /* preface + our SETTINGS + request + control */

typedef struct {
    UInt16        status;                       /* from the :status pseudo-header */
    UInt8         headerCount;
    CNHeaderField headers[CN_HTTP_MAX_HEADERS]; /* regular (non-pseudo) headers */
} CNH2Response;

typedef struct CNH2Conn CNH2Conn;

typedef struct {
    void    (*onResponse)(CNH2Conn *c, const CNH2Response *resp, void *ud);
    Boolean (*onData)    (CNH2Conn *c, const void *bytes, UInt32 len, void *ud);
    void    (*onComplete)(CNH2Conn *c, OSStatus result, void *ud);
} CNH2Callbacks;

struct CNH2Conn {
    CNTransport  *t;
    int           state;

    unsigned char out[CN_H2_OUT_CAP];           /* pending bytes to send */
    UInt32        outOff, outLen;

    unsigned char rbuf[CN_H2_RECV_CAP];         /* frame reassembly */
    UInt32        rOff, rLen;

    unsigned char hblk[CN_H2_HDRBLK_CAP];       /* HEADERS+CONTINUATION fragment */
    UInt32        hblkLen;
    Boolean       inHeaders;                    /* between HEADERS and END_HEADERS */

    CNHpackDec    hpack;
    CNH2Response  resp;
    Boolean       gotResponse;

    UInt32        streamId;                      /* our one stream (1) */
    Boolean       streamClosed;                 /* END_STREAM seen */

    CNH2Callbacks cb;
    void         *ud;
    OSStatus      result;
};

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start a GET on a fresh HTTP/2 connection. `scheme` is "https" or "http",
 * `authority` is the host (optionally host:port). Extra request headers are
 * optional. Queues the preface, our SETTINGS, and the request HEADERS frame.
 */
OSStatus CN_H2Get(CNH2Conn *c, CNTransport *t,
                  const char *scheme, const char *authority, const char *path,
                  const CNHeaderKV *headers, UInt32 headerCount,
                  const CNH2Callbacks *cb, void *ud);

/* Advance as far as the transport allows, firing callbacks. Call until done. */
OSStatus CN_H2Pump(CNH2Conn *c);

/* True once the exchange has completed (success or error). */
Boolean  CN_H2Done(const CNH2Conn *c);

#ifdef __cplusplus
}
#endif

#endif /* CLASSICNET_CN_H2CONN_H */
