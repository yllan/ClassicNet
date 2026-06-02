#include "classicnet/cn_tls.h"

#ifdef CN_WITH_MBEDTLS

#include "classicnet/cn_errors.h"

/* --- BIO glue: mbedTLS <-> inner CNTransport (non-blocking) --------------- */

static int tls_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    CNTlsTransport *tls = (CNTlsTransport *)ctx;
    UInt32 sent = 0;
    OSStatus s = tls->inner->send(tls->inner, buf, (UInt32)len, &sent);
    if (s != noErr) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (sent == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return (int)sent;
}

static int tls_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    CNTlsTransport *tls = (CNTlsTransport *)ctx;
    UInt32 got = 0;
    Boolean eof = false;
    OSStatus s = tls->inner->recv(tls->inner, buf, (UInt32)len, &got, &eof);
    if (s != noErr) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (got == 0) {
        if (eof) return 0;                 /* clean EOF */
        return MBEDTLS_ERR_SSL_WANT_READ;  /* would block */
    }
    return (int)got;
}

/* --- CNTransport vtable --------------------------------------------------- */

static OSStatus tls_poll(CNTransport *t)
{
    CNTlsTransport *tls = (CNTlsTransport *)t;
    int rc;
    OSStatus s = tls->inner->poll(tls->inner);   /* TCP must connect first */
    if (s != noErr) return s;                     /* would-block or error */
    if (tls->handshakeDone) return noErr;

    rc = mbedtls_ssl_handshake(&tls->ssl);
    if (rc == 0) { tls->handshakeDone = 1; return noErr; }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
        return kCNErrWouldBlock;
    return kCNErrTlsHandshake;
}

static OSStatus tls_send(CNTransport *t, const void *data, UInt32 len, UInt32 *sent)
{
    CNTlsTransport *tls = (CNTlsTransport *)t;
    int rc = mbedtls_ssl_write(&tls->ssl, (const unsigned char *)data, len);
    if (rc >= 0) { *sent = (UInt32)rc; return noErr; }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        *sent = 0;
        return noErr;
    }
    return kCNErrTlsIo;
}

static OSStatus tls_recv(CNTransport *t, void *buf, UInt32 cap, UInt32 *got, Boolean *eof)
{
    CNTlsTransport *tls = (CNTlsTransport *)t;
    int rc = mbedtls_ssl_read(&tls->ssl, (unsigned char *)buf, cap);
    *eof = false;
    if (rc > 0) { *got = (UInt32)rc; return noErr; }
    if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        *got = 0; *eof = true; return noErr;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        *got = 0;
        return noErr;
    }
    return kCNErrTlsIo;
}

static void tls_close(CNTransport *t)
{
    CNTlsTransport *tls = (CNTlsTransport *)t;
    mbedtls_ssl_close_notify(&tls->ssl);
    if (tls->inner && tls->inner->close)
        tls->inner->close(tls->inner);
}

/* --- lifecycle ------------------------------------------------------------ */

OSStatus CN_TlsCreate(CNTlsTransport *tls, CNTransport *inner, const char *hostname,
                      const char *caPem, UInt32 caLen, CNTransport **out)
{
    int rc;

    if (tls == 0 || inner == 0 || out == 0)
        return kCNErrBadParam;

    tls->inner = inner;
    tls->handshakeDone = 0;
    mbedtls_ssl_init(&tls->ssl);
    mbedtls_ssl_config_init(&tls->conf);
    mbedtls_ctr_drbg_init(&tls->drbg);
    mbedtls_entropy_init(&tls->entropy);
    mbedtls_x509_crt_init(&tls->cacert);

    rc = mbedtls_ctr_drbg_seed(&tls->drbg, mbedtls_entropy_func, &tls->entropy, 0, 0);
    if (rc != 0) return kCNErrTlsInit;

    rc = mbedtls_ssl_config_defaults(&tls->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) return kCNErrTlsInit;
    mbedtls_ssl_conf_rng(&tls->conf, mbedtls_ctr_drbg_random, &tls->drbg);

    if (caPem != 0 && caLen != 0) {
        rc = mbedtls_x509_crt_parse(&tls->cacert,
                                    (const unsigned char *)caPem, caLen);
        if (rc < 0) return kCNErrTlsInit;
        mbedtls_ssl_conf_ca_chain(&tls->conf, &tls->cacert, 0);
        mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        /* INSECURE: no certificate verification (development only). */
        mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_NONE);
    }

    rc = mbedtls_ssl_setup(&tls->ssl, &tls->conf);
    if (rc != 0) return kCNErrTlsInit;
    if (hostname != 0)
        mbedtls_ssl_set_hostname(&tls->ssl, hostname);
    mbedtls_ssl_set_bio(&tls->ssl, tls, tls_bio_send, tls_bio_recv, 0);

    tls->base.poll  = tls_poll;
    tls->base.send  = tls_send;
    tls->base.recv  = tls_recv;
    tls->base.close = tls_close;
    *out = &tls->base;
    return noErr;
}

void CN_TlsDispose(CNTlsTransport *tls)
{
    if (tls == 0) return;
    mbedtls_x509_crt_free(&tls->cacert);
    mbedtls_ssl_free(&tls->ssl);
    mbedtls_ssl_config_free(&tls->conf);
    mbedtls_ctr_drbg_free(&tls->drbg);
    mbedtls_entropy_free(&tls->entropy);
}

#endif /* CN_WITH_MBEDTLS */
