#include "classicnet/cn_http.h"
#include "classicnet/cn_errors.h"
#include "cn_test.h"

#include <string.h>

/* Parse a C string literal as a buffer of exactly its byte length (no NUL). */
#define PARSE_LIT(lit, out) CN_ParseHttpResponse((lit), (UInt32)(sizeof(lit) - 1), (out))

static int has_header(const CNHttpResponse *r, const char *name)
{
    UInt32 i;
    for (i = 0; i < r->headerCount; i++)
        if (strcmp(r->headers[i].name, name) == 0)
            return 1;
    return 0;
}

static void test_basic_200(void)
{
    CNHttpResponse r;
    const char msg[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    CN_CHECK(PARSE_LIT(msg, &r) == noErr);
    CN_CHECK(r.status == 200);
    CN_CHECK(r.httpMinor == 1);
    CN_CHECK(r.headerCount == 2);
    CN_CHECK(strcmp(r.headers[0].name, "Content-Type") == 0);
    CN_CHECK(strcmp(r.headers[0].value, "text/html") == 0);
    CN_CHECK(strcmp(r.headers[1].value, "5") == 0);
    /* body begins right after the blank line */
    CN_CHECK(strcmp(msg + r.bodyOffset, "hello") == 0);
}

static void test_no_headers_204(void)
{
    CNHttpResponse r;
    const char msg[] = "HTTP/1.0 204 No Content\r\n\r\n";
    CN_CHECK(PARSE_LIT(msg, &r) == noErr);
    CN_CHECK(r.status == 204);
    CN_CHECK(r.httpMinor == 0);
    CN_CHECK(r.headerCount == 0);
    CN_CHECK(r.bodyOffset == sizeof(msg) - 1);
}

static void test_value_ows_trimmed(void)
{
    CNHttpResponse r;
    const char msg[] = "HTTP/1.1 200 OK\r\nX-Pad:   spaced   \r\n\r\n";
    CN_CHECK(PARSE_LIT(msg, &r) == noErr);
    CN_CHECK(r.headerCount == 1);
    CN_CHECK(strcmp(r.headers[0].value, "spaced") == 0);  /* both sides trimmed */
}

static void test_empty_value(void)
{
    CNHttpResponse r;
    const char msg[] = "HTTP/1.1 301 \r\nLocation:\r\n\r\n";
    CN_CHECK(PARSE_LIT(msg, &r) == noErr);
    CN_CHECK(r.status == 301);
    CN_CHECK(r.headerCount == 1);
    CN_CHECK(strcmp(r.headers[0].name, "Location") == 0);
    CN_CHECK(strcmp(r.headers[0].value, "") == 0);
}

static void test_incomplete_needs_more(void)
{
    CNHttpResponse r;
    /* truncated: no terminating blank line yet */
    const char a[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n";
    const char b[] = "HTTP/1.1 2";
    const char c[] = "HTTP/1.1 200 OK\r\nContent-Type: text/ht";
    CN_CHECK(PARSE_LIT(a, &r) == kCNErrHeadersIncomplete);
    CN_CHECK(PARSE_LIT(b, &r) == kCNErrHeadersIncomplete);
    CN_CHECK(PARSE_LIT(c, &r) == kCNErrHeadersIncomplete);
}

static void test_malformed_rejected(void)
{
    CNHttpResponse r;
    const char bad_proto[]  = "HTTX/1.1 200 OK\r\n\r\n";
    const char bad_status[] = "HTTP/1.1 99 OK\r\n\r\n";    /* not 3 digits */
    const char bad_class[]  = "HTTP/1.1 700 OK\r\n\r\n";   /* class 7 invalid */
    const char no_colon[]   = "HTTP/1.1 200 OK\r\nBadHeaderLine\r\n\r\n";
    const char empty_name[] = "HTTP/1.1 200 OK\r\n: value\r\n\r\n";
    CN_CHECK(PARSE_LIT(bad_proto,  &r) == kCNErrBadStatusLine);
    CN_CHECK(PARSE_LIT(bad_status, &r) == kCNErrBadStatusLine);
    CN_CHECK(PARSE_LIT(bad_class,  &r) == kCNErrBadStatusLine);
    CN_CHECK(PARSE_LIT(no_colon,   &r) == kCNErrBadHeader);
    CN_CHECK(PARSE_LIT(empty_name, &r) == kCNErrBadHeader);
}

static void test_too_many_headers(void)
{
    /* Surplus and oversized fields are DROPPED, not errors: real sites ship
     * dozens of headers and multi-KB CSP values; failing the response over
     * an unstorable field would make the whole page unreachable. */
    char buf[8192];
    UInt32 i = 0;
    int n;
    CNHttpResponse r;
    const char *sl = "HTTP/1.1 200 OK\r\n";
    memcpy(buf, sl, strlen(sl));
    i = (UInt32)strlen(sl);
    for (n = 0; n <= CN_HTTP_MAX_HEADERS; n++) {
        buf[i++] = 'A'; buf[i++] = ':'; buf[i++] = 'x';
        buf[i++] = '\r'; buf[i++] = '\n';
    }
    buf[i++] = '\r'; buf[i++] = '\n';
    CN_CHECK(CN_ParseHttpResponse(buf, i, &r) == noErr);
    CN_CHECK(r.headerCount == CN_HTTP_MAX_HEADERS);
    CN_CHECK(r.bodyOffset == i);

    /* an oversized value is skipped; the following header still lands */
    i = (UInt32)strlen(sl);
    memcpy(buf, sl, i);
    {
        const char *nm = "Content-Security-Policy: ";
        memcpy(buf + i, nm, strlen(nm)); i += (UInt32)strlen(nm);
        for (n = 0; n < CN_HTTP_MAX_VALUE + 100; n++) buf[i++] = 'p';
        buf[i++] = '\r'; buf[i++] = '\n';
    }
    {
        const char *ct = "Content-Type: text/html\r\n\r\n";
        memcpy(buf + i, ct, strlen(ct)); i += (UInt32)strlen(ct);
    }
    CN_CHECK(CN_ParseHttpResponse(buf, i, &r) == noErr);
    CN_CHECK(r.headerCount == 1);
    CN_CHECK(strcmp(r.headers[0].name, "Content-Type") == 0);

    /* body-framing fields are kept even if they arrive after the storage cap */
    i = (UInt32)strlen(sl);
    memcpy(buf, sl, i);
    for (n = 0; n < CN_HTTP_MAX_HEADERS; n++) {
        const char *x = "X-Ignored: y\r\n";
        memcpy(buf + i, x, strlen(x)); i += (UInt32)strlen(x);
    }
    {
        const char *cl = "Content-Length: 5\r\n\r\n";
        memcpy(buf + i, cl, strlen(cl)); i += (UInt32)strlen(cl);
    }
    CN_CHECK(CN_ParseHttpResponse(buf, i, &r) == noErr);
    CN_CHECK(r.headerCount == CN_HTTP_MAX_HEADERS);
    CN_CHECK(has_header(&r, "Content-Length"));

    /* but an oversized framing field is unsafe to ignore */
    i = (UInt32)strlen(sl);
    memcpy(buf, sl, i);
    {
        const char *cl = "Content-Length: ";
        memcpy(buf + i, cl, strlen(cl)); i += (UInt32)strlen(cl);
        for (n = 0; n < CN_HTTP_MAX_VALUE + 100; n++) buf[i++] = '9';
        buf[i++] = '\r'; buf[i++] = '\n';
        buf[i++] = '\r'; buf[i++] = '\n';
    }
    CN_CHECK(CN_ParseHttpResponse(buf, i, &r) == kCNErrHeaderTooLong);
}

static void test_nul_safe(void)
{
    /* Embedded NUL inside a header value must not truncate parsing or crash. */
    CNHttpResponse r;
    const char msg[] = "HTTP/1.1 200 OK\r\nX-Bin: a\0b\r\n\r\n";
    UInt32 len = (UInt32)(sizeof(msg) - 1);   /* includes the embedded NUL */
    CN_CHECK(CN_ParseHttpResponse(msg, len, &r) == noErr);
    CN_CHECK(r.headerCount == 1);
    CN_CHECK(strcmp(r.headers[0].name, "X-Bin") == 0);
    /* value's first byte is 'a'; the embedded NUL is preserved in the buffer */
    CN_CHECK(r.headers[0].value[0] == 'a');
    CN_CHECK(r.headers[0].value[1] == '\0');
    CN_CHECK(r.headers[0].value[2] == 'b');
}

int main(void)
{
    CN_RUN(test_basic_200);
    CN_RUN(test_no_headers_204);
    CN_RUN(test_value_ows_trimmed);
    CN_RUN(test_empty_value);
    CN_RUN(test_incomplete_needs_more);
    CN_RUN(test_malformed_rejected);
    CN_RUN(test_too_many_headers);
    CN_RUN(test_nul_safe);
    return CN_SUMMARY();
}
