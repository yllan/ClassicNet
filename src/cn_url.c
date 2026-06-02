#include "classicnet/cn_url.h"
#include "classicnet/cn_errors.h"

#include <string.h>

static char cn_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

/* Case-insensitively compare the first `len` bytes of s to NUL-terminated lit. */
static int cn_scheme_eq(const char *s, unsigned long len, const char *lit)
{
    unsigned long i;
    for (i = 0; i < len; i++) {
        if (lit[i] == '\0')
            return 0;
        if (cn_lower(s[i]) != lit[i])
            return 0;
    }
    return lit[len] == '\0';
}

OSStatus CN_ParseURL(const char *url, CNUrl *out)
{
    const char *sep;
    const char *authority;
    const char *host_end;
    const char *colon;
    const char *p;
    unsigned long scheme_len;
    unsigned long host_len;
    unsigned long path_len;

    if (url == 0 || out == 0)
        return kCNErrBadURL;

    sep = strstr(url, "://");
    if (sep == 0)
        return kCNErrBadURL;

    scheme_len = (unsigned long)(sep - url);
    if (cn_scheme_eq(url, scheme_len, "https")) {
        out->scheme = kCNSchemeHTTPS; out->secure = true;  out->port = 443;
    } else if (cn_scheme_eq(url, scheme_len, "http")) {
        out->scheme = kCNSchemeHTTP;  out->secure = false; out->port = 80;
    } else if (cn_scheme_eq(url, scheme_len, "wss")) {
        out->scheme = kCNSchemeWSS;   out->secure = true;  out->port = 443;
    } else if (cn_scheme_eq(url, scheme_len, "ws")) {
        out->scheme = kCNSchemeWS;    out->secure = false; out->port = 80;
    } else {
        return kCNErrUnsupportedScheme;
    }

    authority = sep + 3;

    /* The authority runs until the first '/', '?' or end of string. */
    host_end = authority;
    while (*host_end != '\0' && *host_end != '/' && *host_end != '?')
        host_end++;

    /* Look for an optional ":port" inside the authority. */
    colon = 0;
    for (p = authority; p < host_end; p++) {
        if (*p == ':') { colon = p; break; }
    }

    if (colon != 0) {
        unsigned long port_val = 0;
        const char *q;
        if (colon + 1 == host_end)          /* "host:" with no digits */
            return kCNErrBadPort;
        for (q = colon + 1; q < host_end; q++) {
            if (*q < '0' || *q > '9')
                return kCNErrBadPort;
            port_val = port_val * 10 + (unsigned long)(*q - '0');
            if (port_val > 65535)
                return kCNErrBadPort;
        }
        if (port_val == 0)
            return kCNErrBadPort;
        out->port = (UInt16)port_val;
        host_len = (unsigned long)(colon - authority);
    } else {
        host_len = (unsigned long)(host_end - authority);
    }

    if (host_len == 0)
        return kCNErrBadURL;
    if (host_len >= CN_URL_MAX_HOST)
        return kCNErrHostTooLong;

    memcpy(out->host, authority, host_len);
    out->host[host_len] = '\0';

    /* Build the request path; default to "/", and keep a leading '/' before a query. */
    if (*host_end == '\0') {
        out->path[0] = '/';
        out->path[1] = '\0';
    } else if (*host_end == '?') {
        path_len = strlen(host_end);
        if (path_len + 1 >= CN_URL_MAX_PATH)
            return kCNErrPathTooLong;
        out->path[0] = '/';
        memcpy(out->path + 1, host_end, path_len + 1);
    } else {
        path_len = strlen(host_end);
        if (path_len >= CN_URL_MAX_PATH)
            return kCNErrPathTooLong;
        memcpy(out->path, host_end, path_len + 1);
    }

    return noErr;
}
