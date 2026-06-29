#ifndef CLASSICNET_CN_H2CONN_H
#define CLASSICNET_CN_H2CONN_H

#include "cn_types.h"
#include "cn_http.h"        /* CNHeaderField, CNHeaderKV, CN_HTTP_MAX_* */
#include "cn_hpack.h"
#include "cn_transport.h"

/*
 * HTTP/2 connection layer with N-way multiplexing: run several GETs
 * concurrently over one stream-multiplexed connection on top of any
 * CNTransport (TLS-over-OT on the target, a loopback on the host). Same
 * cooperative pump/callback shape as CNRequest -- CN_H2Pump returns promptly
 * and is called from the app's event loop; each request has its own callbacks.
 *
 * Memory note: HTTP/2 forbids interleaving HEADERS/CONTINUATION of different
 * streams (a header block must finish before any other frame), so only one
 * header block is ever being reassembled at a time. The reassembly buffer, the
 * HPACK decoder, and the response scratch are therefore single connection-wide
 * instances; each stream stores only small per-request state.
 */

#define CN_H2_RECV_CAP    (16384u + 9u)  /* one frame at the default max frame size */
#define CN_H2_HDRBLK_CAP   16384u
#define CN_H2_HENC_CAP    4096u        /* outbound HEADERS block scratch (off the stack) */
#define CN_H2_OUT_CAP   (16384u + 256u)  /* a full max-size DATA frame + preface/SETTINGS/HEADERS */
#define CN_H2_MAX_STREAMS      8u        /* concurrent requests per connection */

typedef struct {
    UInt16        status;                       /* from the :status pseudo-header */
    UInt8         headerCount;
    CNHeaderField headers[CN_HTTP_MAX_HEADERS]; /* regular (non-pseudo) headers */
} CNH2Response;

typedef struct CNH2Conn CNH2Conn;

/*
 * Per-request callbacks. onResponse/onData/onComplete fire for the stream this
 * request opened; `ud` distinguishes concurrent requests. The CNH2Response and
 * data pointers are valid only for the duration of the call.
 */
typedef struct {
    void    (*onResponse)(CNH2Conn *c, const CNH2Response *resp, void *ud);
    Boolean (*onData)    (CNH2Conn *c, const void *bytes, UInt32 len, void *ud);
    void    (*onComplete)(CNH2Conn *c, OSStatus result, void *ud);
} CNH2Callbacks;

typedef struct {
    UInt32        id;            /* stream id; 0 = free slot */
    Boolean       gotResponse;   /* response headers delivered */
    Boolean       closed;        /* END_STREAM seen (server done sending) */
    Boolean       done;          /* onComplete fired */
    CNH2Callbacks cb;
    void         *ud;
    const unsigned char *body;   /* pending request body, borrowed until complete (0 = none/done) */
    UInt32        bodyLen;        /* total request-body length */
    UInt32        bodyOff;        /* request-body bytes already queued */
    SInt32        sendWindow;     /* per-stream send flow-control window */
} CNH2Stream;

struct CNH2Conn {
    CNTransport  *t;
    int           state;

    unsigned char out[CN_H2_OUT_CAP];           /* pending bytes to send */
    UInt32        outOff, outLen;

    unsigned char rbuf[CN_H2_RECV_CAP];         /* frame reassembly */
    UInt32        rOff, rLen;

    unsigned char hblk[CN_H2_HDRBLK_CAP];       /* HEADERS+CONTINUATION fragment */
    UInt32        hblkLen;
    UInt32        hdrStream;                     /* stream of the in-progress block (0=none) */
    Boolean       inHeaders;                     /* between HEADERS and END_HEADERS */

    CNHpackDec    hpack;
    CNH2Response  resp;                          /* shared decode scratch */
    unsigned char hdrScratch[CN_H2_HENC_CAP];   /* open_stream HPACK encode buffer (kept off the cooperative stack) */

    CNH2Stream    streams[CN_H2_MAX_STREAMS];
    UInt32        nextStreamId;                  /* next odd id to assign (1,3,5,...) */
    UInt32        openCount;                     /* streams not yet completed */
    SInt32        connSendWindow;                /* connection-level send flow-control window */
    UInt32        peerMaxFrame;                  /* peer SETTINGS_MAX_FRAME_SIZE (default 16384) */
    UInt32        peerInitWindow;                /* peer SETTINGS_INITIAL_WINDOW_SIZE (default 65535) */

    OSStatus      result;
};

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize a connection over `t` and queue the preface + our SETTINGS.
 * Call CN_H2Request one or more times to open requests, then pump.
 */
OSStatus CN_H2ConnStart(CNH2Conn *c, CNTransport *t);

/*
 * Open a GET on a new multiplexed stream. `scheme` is "https"/"http",
 * `authority` is the host (optionally host:port). Returns the assigned stream
 * id in *streamId (may be 0). Up to CN_H2_MAX_STREAMS may be open at once.
 */
OSStatus CN_H2Request(CNH2Conn *c,
                      const char *scheme, const char *authority, const char *path,
                      const CNHeaderKV *headers, UInt32 headerCount,
                      const CNH2Callbacks *cb, void *ud, UInt32 *streamId);

/*
 * Convenience: start a connection and open a single GET in one call (the common
 * case). Equivalent to CN_H2ConnStart + CN_H2Request.
 */
OSStatus CN_H2Get(CNH2Conn *c, CNTransport *t,
                  const char *scheme, const char *authority, const char *path,
                  const CNHeaderKV *headers, UInt32 headerCount,
                  const CNH2Callbacks *cb, void *ud);

/*
 * General form: open a request with an explicit method and an optional request
 * body. When bodyLen > 0 a `content-length` header is added and the body is sent
 * as DATA frames, chunked under HTTP/2 flow control and the peer's max frame
 * size (CN_H2Get/CN_H2Request are the body-less GET special case). There is no
 * fixed body-size cap; a larger body just spans more CN_H2Pump calls.
 *
 * Body lifetime: `body` is BORROWED, not copied. It is streamed from the pump
 * across multiple CN_H2Pump calls (flow control can defer it), so the caller
 * MUST keep `body` valid and unchanged until the request completes (onComplete
 * fires) or the connection is disposed. Passing stack or temporary memory will
 * corrupt the upload.
 */
OSStatus CN_H2RequestEx(CNH2Conn *c, const char *method,
                        const char *scheme, const char *authority, const char *path,
                        const CNHeaderKV *headers, UInt32 headerCount,
                        const void *body, UInt32 bodyLen,
                        const CNH2Callbacks *cb, void *ud, UInt32 *streamId);

/*
 * Convenience: start a connection and open a single POST in one call.
 * Equivalent to CN_H2ConnStart + CN_H2RequestEx with method "POST".
 */
OSStatus CN_H2Post(CNH2Conn *c, CNTransport *t,
                   const char *scheme, const char *authority, const char *path,
                   const CNHeaderKV *headers, UInt32 headerCount,
                   const void *body, UInt32 bodyLen,
                   const CNH2Callbacks *cb, void *ud);

/* Advance all streams as far as the transport allows, firing callbacks. */
OSStatus CN_H2Pump(CNH2Conn *c);

/* True once every opened stream has completed (or the connection failed). */
Boolean  CN_H2Done(const CNH2Conn *c);

#ifdef __cplusplus
}
#endif

#endif /* CLASSICNET_CN_H2CONN_H */
