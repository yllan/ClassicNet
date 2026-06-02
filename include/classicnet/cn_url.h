#ifndef CLASSICNET_CN_URL_H
#define CLASSICNET_CN_URL_H

#include "cn_types.h"

#define CN_URL_MAX_HOST 256
#define CN_URL_MAX_PATH 1024

typedef enum {
    kCNSchemeHTTP = 0,
    kCNSchemeHTTPS,
    kCNSchemeWS,
    kCNSchemeWSS
} CNScheme;

typedef struct {
    CNScheme scheme;
    Boolean  secure;                 /* true for https / wss */
    UInt16   port;                   /* defaulted from scheme when absent */
    char     host[CN_URL_MAX_HOST];  /* NUL-terminated, no port */
    char     path[CN_URL_MAX_PATH];  /* path + query; defaults to "/" */
} CNUrl;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse an absolute http/https/ws/wss URL into *out.
 * Fixed-size buffers, no allocation -- safe to call at any heap state.
 * Returns noErr, or a kCNErr* code from cn_errors.h.
 */
OSStatus CN_ParseURL(const char *url, CNUrl *out);

#ifdef __cplusplus
}
#endif

#endif /* CLASSICNET_CN_URL_H */
