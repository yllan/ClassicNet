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

int main(void)
{
    CN_RUN(test_get_with_headers);
    CN_RUN(test_no_extra_headers);
    CN_RUN(test_overflow);
    return CN_SUMMARY();
}
