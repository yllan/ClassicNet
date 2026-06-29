#ifndef CLASSICNET_CN_REQUEST_H
#define CLASSICNET_CN_REQUEST_H

#include "cn_types.h"
#include "cn_http.h"
#include "cn_transport.h"

#define CN_REQ_BUF 4096

typedef struct CNRequest CNRequest;

/*
 * Async callbacks, fired from within CN_RequestPump (i.e. at "system task" time
 * in the Mac model -- never at interrupt time).  onData may be called multiple
 * times as the body streams in; returning false signals back-pressure (reserved
 * for future use).  onComplete fires exactly once with the final result.
 */
typedef struct {
    void    (*onResponse)(CNRequest *r, const CNHttpResponse *resp, void *ud);
    Boolean (*onData)    (CNRequest *r, const void *bytes, UInt32 len, void *ud);
    void    (*onComplete)(CNRequest *r, OSStatus result, void *ud);
} CNRequestCallbacks;

struct CNRequest {
    CNTransport       *t;
    int                state;
    char               buf[CN_REQ_BUF];   /* request bytes, then response accumulation */
    UInt32             sendLen, sendOff;
    UInt32             bufLen;            /* response bytes accumulated in buf */
    UInt32             bodyOff;           /* offset of the first body byte in buf */
    CNHttpResponse     resp;
    int                bodyMode;
    int                isHead;            /* response to HEAD has no body, even with Content-Length */
    UInt32             contentRemaining;  /* for Content-Length bodies */
    CNChunked          chunkDec;          /* streaming decoder for chunked bodies */
    CNRequestCallbacks cb;
    void              *ud;
    OSStatus           result;
};

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize a request over an already-constructed (connecting) transport and
 * serialize its head.  The transport owns the host/port; the state machine just
 * drives it.  Returns noErr or a build error.
 */
OSStatus CN_RequestStart(CNRequest *r, CNTransport *t,
                         const char *method, const char *path, const char *host,
                         const CNHeaderKV *headers, UInt32 headerCount,
                         const CNRequestCallbacks *cb, void *ud);

/* Advance the request as far as the transport currently allows, firing
   callbacks.  Returns promptly; call repeatedly until CN_RequestDone. */
OSStatus CN_RequestPump(CNRequest *r);

/* True once the request has completed (successfully or with an error). */
Boolean  CN_RequestDone(const CNRequest *r);

#ifdef __cplusplus
}
#endif

#endif /* CLASSICNET_CN_REQUEST_H */
