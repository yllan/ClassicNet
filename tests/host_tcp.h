#ifndef CN_HOST_TCP_H
#define CN_HOST_TCP_H

/* Host-only (POSIX) non-blocking TCP CNTransport, for integration tests. */
#include "classicnet/cn_transport.h"

typedef struct {
    CNTransport base;
    int fd;
    int connected;
} HostTcp;

OSStatus HostTcpConnect(HostTcp *h, const char *ip, UInt16 port, CNTransport **out);

#endif
