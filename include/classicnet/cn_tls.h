#ifndef CLASSICNET_CN_TLS_H
#define CLASSICNET_CN_TLS_H

#include "cn_types.h"
#include "cn_transport.h"

/*
 * TLS transport: wraps an inner (TCP) CNTransport with TLS, and is itself a
 * CNTransport you drive with the same non-blocking pump.  This works because
 * mbedTLS's BIO callbacks are non-blocking (WANT_READ/WANT_WRITE), which maps
 * directly onto CNTransport's would-block semantics.
 *
 * This whole layer is compiled only when CN_WITH_MBEDTLS is defined.
 */
#ifdef CN_WITH_MBEDTLS

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"

typedef struct {
    CNTransport               base;     /* our vtable -- must be first */
    CNTransport              *inner;    /* underlying transport (e.g. TCP) */
    mbedtls_ssl_context       ssl;
    mbedtls_ssl_config        conf;
    mbedtls_ctr_drbg_context  drbg;
    mbedtls_entropy_context   entropy;
    mbedtls_x509_crt          cacert;
    int                       handshakeDone;
    int                       lastError;   /* last mbedTLS rc, for error reporting */
} CNTlsTransport;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Wrap `inner` with TLS.  hostname drives SNI and certificate verification.
 *
 * caPem/caLen: a PEM CA bundle used to verify the server certificate.  For PEM,
 * caLen MUST include the terminating NUL byte (an mbedTLS requirement).  Pass
 * caPem == 0 to DISABLE verification -- INSECURE, development only.
 *
 * On success *out receives a CNTransport* (== &tls->base) to drive normally.
 *
 * NOTE: this uses mbedtls_entropy_func, which on the host reads the platform
 * RNG.  On Classic Mac OS there is no such source -- a custom entropy source
 * must be registered before this is used on-target (DESIGN.md hard problem #1).
 */
OSStatus CN_TlsCreate(CNTlsTransport *tls, CNTransport *inner, const char *hostname,
                      const char *caPem, UInt32 caLen, CNTransport **out);

void CN_TlsDispose(CNTlsTransport *tls);

#ifdef __cplusplus
}
#endif

#endif /* CN_WITH_MBEDTLS */
#endif /* CLASSICNET_CN_TLS_H */
