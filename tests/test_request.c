#include "classicnet/cn_http.h"
#include "classicnet/cn_errors.h"
#include "cn_test.h"

#include <string.h>

static void test_get_with_headers(void)
{
    char out[256];
    UInt32 ol = 0;
    CNHeaderKV h[2];
    const char *expected =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Accept: text/html\r\n"
        "User-Agent: ClassicNet\r\n"
        "\r\n";
    h[0].name = "Accept";     h[0].value = "text/html";
    h[1].name = "User-Agent"; h[1].value = "ClassicNet";
    CN_CHECK(CN_BuildRequest("GET", "/index.html", "example.com", h, 2,
                             out, sizeof(out), &ol) == noErr);
    CN_CHECK(strcmp(out, expected) == 0);
    CN_CHECK(ol == (UInt32)strlen(expected));
}

static void test_no_extra_headers(void)
{
    char out[128];
    UInt32 ol = 0;
    CN_CHECK(CN_BuildRequest("HEAD", "/", "h.example", 0, 0,
                             out, sizeof(out), &ol) == noErr);
    CN_CHECK(strcmp(out, "HEAD / HTTP/1.1\r\nHost: h.example\r\n\r\n") == 0);
}

static void test_overflow(void)
{
    char out[16];   /* far too small */
    UInt32 ol = 0;
    CN_CHECK(CN_BuildRequest("GET", "/index.html", "example.com", 0, 0,
                             out, sizeof(out), &ol) == kCNErrBufferOverflow);
}

static void test_rejects_injection(void)
{
    char out[256];
    UInt32 ol = 0;
    CNHeaderKV h[1];
    /* CRLF in the request target must not inject a second request line */
    CN_CHECK(CN_BuildRequest("GET", "/x\r\nX-Evil: 1", "example.com", 0, 0,
                             out, sizeof(out), &ol) == kCNErrBadParam);
    /* CRLF in the host */
    CN_CHECK(CN_BuildRequest("GET", "/", "h\r\nX-Evil: 1", 0, 0,
                             out, sizeof(out), &ol) == kCNErrBadParam);
    /* CRLF in a header value must not inject another header */
    h[0].name = "X-Test"; h[0].value = "ok\r\nX-Evil: 1";
    CN_CHECK(CN_BuildRequest("GET", "/", "example.com", h, 1,
                             out, sizeof(out), &ol) == kCNErrBadParam);
    /* space / control char in a header name */
    h[0].name = "X Bad"; h[0].value = "v";
    CN_CHECK(CN_BuildRequest("GET", "/", "example.com", h, 1,
                             out, sizeof(out), &ol) == kCNErrBadParam);
    /* ':' in a header name */
    h[0].name = "X:Bad"; h[0].value = "v";
    CN_CHECK(CN_BuildRequest("GET", "/", "example.com", h, 1,
                             out, sizeof(out), &ol) == kCNErrBadParam);
}

int main(void)
{
    CN_RUN(test_get_with_headers);
    CN_RUN(test_no_extra_headers);
    CN_RUN(test_overflow);
    CN_RUN(test_rejects_injection);
    return CN_SUMMARY();
}
