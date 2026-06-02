#ifndef CLASSICNET_CN_OT_H
#define CLASSICNET_CN_OT_H

#include "cn_types.h"
#include "cn_transport.h"

/*
 * Open Transport transport: a CNTransport backed by OT TCP, for Classic Mac OS.
 * Non-blocking (OTSetNonBlocking) so it composes with the cooperative pump --
 * OT's kOTNoDataErr maps onto our would-block semantics.
 *
 * Compiled only when CN_WITH_OT is defined (i.e. the Retro68/PPC target build
 * with Apple's Universal Interfaces). Link against OpenTransportAppPPC,
 * OpenTransportLib and OpenTptInternetLib.
 */
#ifdef CN_WITH_OT

#include <OpenTransport.h>
#include <OpenTptInternet.h>

typedef struct {
    CNTransport base;          /* must be first */
    EndpointRef ep;
    int         state;
    char        hostport[256]; /* "host:port" for OTInitDNSAddress */
} CNOTTransport;

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize/!shut down Open Transport for the process (call once each). */
OSStatus CN_OTStartup(void);
void     CN_OTShutdown(void);

/* Create a TCP transport to host:port. *out is a CNTransport* to drive. */
OSStatus CN_OTCreate(CNOTTransport *t, const char *host, UInt16 port,
                     CNTransport **out);
void     CN_OTDispose(CNOTTransport *t);

#ifdef __cplusplus
}
#endif

#endif /* CN_WITH_OT */
#endif /* CLASSICNET_CN_OT_H */
