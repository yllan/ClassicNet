#ifndef CLASSICNET_CN_TRANSPORT_H
#define CLASSICNET_CN_TRANSPORT_H

#include "cn_types.h"

/*
 * Non-blocking byte-stream transport interface.
 *
 * This is the seam between the protocol state machines and the outside world.
 * On Classic Mac OS the implementation wraps Open Transport (and, layered above
 * it, TLS); on the host a fake/loopback implementation lets the entire async
 * flow be unit-tested under sanitizers.  All operations are non-blocking and
 * must return promptly -- a pump (CN_RequestPump) drives them from the app's
 * cooperative event loop.
 *
 * An implementation embeds CNTransport as its first member and downcasts.
 */
typedef struct CNTransport CNTransport;

struct CNTransport {
    /* Connection progress: noErr = connected, kCNErrWouldBlock = still pending,
       any other value = failure. */
    OSStatus (*poll)(CNTransport *t);

    /* Send up to len bytes. *sent receives the count accepted (0 = would block). */
    OSStatus (*send)(CNTransport *t, const void *data, UInt32 len, UInt32 *sent);

    /* Receive up to cap bytes. *got receives the count (0 = would block, or EOF
       when *eof is set true). */
    OSStatus (*recv)(CNTransport *t, void *buf, UInt32 cap, UInt32 *got, Boolean *eof);

    /* Release the connection. */
    void (*close)(CNTransport *t);
};

#endif /* CLASSICNET_CN_TRANSPORT_H */
