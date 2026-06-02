#include "host_tcp.h"
#include "classicnet/cn_errors.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <string.h>

static OSStatus htcp_poll(CNTransport *t)
{
    HostTcp *h = (HostTcp *)t;
    struct pollfd pfd;
    int so_err = 0;
    socklen_t sl = sizeof(so_err);

    if (h->connected) return noErr;

    pfd.fd = h->fd; pfd.events = POLLOUT; pfd.revents = 0;
    if (poll(&pfd, 1, 0) <= 0) return kCNErrWouldBlock;
    if (getsockopt(h->fd, SOL_SOCKET, SO_ERROR, &so_err, &sl) != 0 || so_err != 0)
        return kCNErrNetIo;
    h->connected = 1;
    return noErr;
}

static OSStatus htcp_send(CNTransport *t, const void *data, UInt32 len, UInt32 *sent)
{
    HostTcp *h = (HostTcp *)t;
    ssize_t n = send(h->fd, data, len, 0);
    if (n >= 0) { *sent = (UInt32)n; return noErr; }
    if (errno == EAGAIN || errno == EWOULDBLOCK) { *sent = 0; return noErr; }
    return kCNErrNetIo;
}

static OSStatus htcp_recv(CNTransport *t, void *buf, UInt32 cap, UInt32 *got, Boolean *eof)
{
    HostTcp *h = (HostTcp *)t;
    ssize_t n = recv(h->fd, buf, cap, 0);
    *eof = false;
    if (n > 0) { *got = (UInt32)n; return noErr; }
    if (n == 0) { *got = 0; *eof = true; return noErr; }
    if (errno == EAGAIN || errno == EWOULDBLOCK) { *got = 0; return noErr; }
    return kCNErrNetIo;
}

static void htcp_close(CNTransport *t)
{
    HostTcp *h = (HostTcp *)t;
    if (h->fd >= 0) { close(h->fd); h->fd = -1; }
}

OSStatus HostTcpConnect(HostTcp *h, const char *ip, UInt16 port, CNTransport **out)
{
    struct sockaddr_in addr;
    int flags;

    h->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (h->fd < 0) return kCNErrTlsIo;
    flags = fcntl(h->fd, F_GETFL, 0);
    fcntl(h->fd, F_SETFL, flags | O_NONBLOCK);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) { close(h->fd); return kCNErrBadParam; }

    h->connected = 0;
    if (connect(h->fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        h->connected = 1;
    } else if (errno != EINPROGRESS) {
        close(h->fd);
        return kCNErrNetIo;
    }

    h->base.poll = htcp_poll;
    h->base.send = htcp_send;
    h->base.recv = htcp_recv;
    h->base.close = htcp_close;
    *out = &h->base;
    return noErr;
}
