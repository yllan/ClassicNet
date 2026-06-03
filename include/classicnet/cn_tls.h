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
    const char               *alpn[3];     /* NULL-terminated; persists for the config */
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

/*
 * Offer ALPN protocols (e.g. "h2", "http/1.1") during the handshake.  Call
 * after CN_TlsCreate and before driving the handshake (the first poll).  The
 * string pointers must outlive the transport (string literals are fine).  Pass
 * proto2 == 0 to offer just one.  Negotiating h2 is how an HTTP/2 client tells
 * the server it speaks h2 over TLS.
 */
OSStatus CN_TlsSetAlpn(CNTlsTransport *tls, const char *proto1, const char *proto2);

/*
 * The protocol the peer selected via ALPN, or 0 if none was negotiated.  Valid
 * only after the handshake completes.
 */
const char *CN_TlsGetAlpn(CNTlsTransport *tls);

/*
 * Mix application-collected entropy into the RNG before the handshake.
 *
 * Classic Mac OS has no hardware RNG; the built-in seed (mbedtls_hardware_poll
 * on target: Microseconds/TickCount/mouse) is mediocre. To harden it, collect
 * unpredictable bytes over the session -- inter-event timing jitter from mouse
 * and keyboard, TickCount/Microseconds deltas, uninitialised stack -- and feed
 * them here before driving the handshake. The bytes are stirred into the
 * CTR_DRBG as additional input via a reseed, so this can only add entropy,
 * never remove it. Call as many times as you like; passing data==0,len==0 just
 * pulls a fresh reseed from the configured source.
 *
 * This is a mixing hook, not a guarantee: it improves the seed quality but does
 * not by itself make the RNG cryptographically sound on a machine with no real
 * entropy source. Collect generously.
 */
OSStatus CN_TlsAddEntropy(CNTlsTransport *tls, const void *data, UInt32 len);

void CN_TlsDispose(CNTlsTransport *tls);

#ifdef __cplusplus
}
#endif

#endif /* CN_WITH_MBEDTLS */
#endif /* CLASSICNET_CN_TLS_H */
