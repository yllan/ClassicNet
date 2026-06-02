#include "classicnet/cn_url.h"
#include "classicnet/cn_errors.h"
#include "cn_test.h"

#include <string.h>

static void test_https_basic(void)
{
    CNUrl u;
    CN_CHECK(CN_ParseURL("https://example.com/path", &u) == noErr);
    CN_CHECK(u.scheme == kCNSchemeHTTPS);
    CN_CHECK(u.secure == true);
    CN_CHECK(u.port == 443);
    CN_CHECK(strcmp(u.host, "example.com") == 0);
    CN_CHECK(strcmp(u.path, "/path") == 0);
}

static void test_http_default_port_and_path(void)
{
    CNUrl u;
    CN_CHECK(CN_ParseURL("http://example.com", &u) == noErr);
    CN_CHECK(u.scheme == kCNSchemeHTTP);
    CN_CHECK(u.secure == false);
    CN_CHECK(u.port == 80);
    CN_CHECK(strcmp(u.host, "example.com") == 0);
    CN_CHECK(strcmp(u.path, "/") == 0);    /* missing path defaults to "/" */
}

static void test_explicit_port(void)
{
    CNUrl u;
    CN_CHECK(CN_ParseURL("https://example.com:8443/x", &u) == noErr);
    CN_CHECK(u.port == 8443);
    CN_CHECK(strcmp(u.host, "example.com") == 0);
    CN_CHECK(strcmp(u.path, "/x") == 0);
}

static void test_websocket_schemes(void)
{
    CNUrl u;
    CN_CHECK(CN_ParseURL("ws://h.example/sock", &u) == noErr);
    CN_CHECK(u.scheme == kCNSchemeWS);
    CN_CHECK(u.secure == false);
    CN_CHECK(u.port == 80);

    CN_CHECK(CN_ParseURL("wss://h.example/sock", &u) == noErr);
    CN_CHECK(u.scheme == kCNSchemeWSS);
    CN_CHECK(u.secure == true);
    CN_CHECK(u.port == 443);
}

static void test_scheme_case_insensitive(void)
{
    CNUrl u;
    CN_CHECK(CN_ParseURL("HTTPS://Example.COM/p", &u) == noErr);
    CN_CHECK(u.scheme == kCNSchemeHTTPS);
    /* host case is preserved verbatim (DNS is case-insensitive anyway) */
    CN_CHECK(strcmp(u.host, "Example.COM") == 0);
}

static void test_query_only(void)
{
    CNUrl u;
    CN_CHECK(CN_ParseURL("https://h.example?a=1&b=2", &u) == noErr);
    CN_CHECK(strcmp(u.host, "h.example") == 0);
    CN_CHECK(strcmp(u.path, "/?a=1&b=2") == 0);   /* leading '/' inserted */
}

static void test_path_with_query(void)
{
    CNUrl u;
    CN_CHECK(CN_ParseURL("https://h.example/a/b?q=1", &u) == noErr);
    CN_CHECK(strcmp(u.path, "/a/b?q=1") == 0);
}

/* --- negative cases: must fail, never crash (the point of host fuzz/ASan) --- */

static void test_rejects_bad_input(void)
{
    CNUrl u;
    CN_CHECK(CN_ParseURL(0, &u) == kCNErrBadURL);
    CN_CHECK(CN_ParseURL("example.com/no-scheme", &u) == kCNErrBadURL);
    CN_CHECK(CN_ParseURL("ftp://example.com", &u) == kCNErrUnsupportedScheme);
    CN_CHECK(CN_ParseURL("https:///empty-host", &u) == kCNErrBadURL);
    CN_CHECK(CN_ParseURL("https://h:/x", &u) == kCNErrBadPort);
    CN_CHECK(CN_ParseURL("https://h:0/x", &u) == kCNErrBadPort);
    CN_CHECK(CN_ParseURL("https://h:abc/x", &u) == kCNErrBadPort);
    CN_CHECK(CN_ParseURL("https://h:99999/x", &u) == kCNErrBadPort);
}

static void test_rejects_overlong_host(void)
{
    char buf[CN_URL_MAX_HOST + 64];
    CNUrl u;
    int i;
    memcpy(buf, "https://", 8);
    for (i = 0; i < CN_URL_MAX_HOST + 8; i++)
        buf[8 + i] = 'a';
    memcpy(buf + 8 + (CN_URL_MAX_HOST + 8), "/p", 3);   /* incl NUL */
    CN_CHECK(CN_ParseURL(buf, &u) == kCNErrHostTooLong);
}

int main(void)
{
    CN_RUN(test_https_basic);
    CN_RUN(test_http_default_port_and_path);
    CN_RUN(test_explicit_port);
    CN_RUN(test_websocket_schemes);
    CN_RUN(test_scheme_case_insensitive);
    CN_RUN(test_query_only);
    CN_RUN(test_path_with_query);
    CN_RUN(test_rejects_bad_input);
    CN_RUN(test_rejects_overlong_host);
    return CN_SUMMARY();
}
