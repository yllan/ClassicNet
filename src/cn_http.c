#include "classicnet/cn_http.h"
#include "classicnet/cn_errors.h"

static int cn_is_digit(char c) { return c >= '0' && c <= '9'; }
static int cn_is_ows(char c)   { return c == ' ' || c == '\t'; }

OSStatus CN_ParseHttpResponse(const char *buf, UInt32 len, CNHttpResponse *out)
{
    UInt32 i = 0;

    if (buf == 0 || out == 0)
        return kCNErrBadStatusLine;

    out->status = 0;
    out->httpMinor = 0;
    out->headerCount = 0;
    out->bodyOffset = 0;

    /* --- Status line: "HTTP/1." minor SP 3DIGIT [SP reason] CRLF --- */
    if (i + 7 > len) return kCNErrHeadersIncomplete;
    if (buf[0] != 'H' || buf[1] != 'T' || buf[2] != 'T' || buf[3] != 'P' ||
        buf[4] != '/' || buf[5] != '1' || buf[6] != '.')
        return kCNErrBadStatusLine;
    i = 7;

    if (i + 1 > len) return kCNErrHeadersIncomplete;
    if (buf[i] != '0' && buf[i] != '1') return kCNErrBadStatusLine;
    out->httpMinor = (UInt8)(buf[i] - '0');
    i++;

    if (i + 1 > len) return kCNErrHeadersIncomplete;
    if (buf[i] != ' ') return kCNErrBadStatusLine;
    i++;

    if (i + 3 > len) return kCNErrHeadersIncomplete;
    if (!cn_is_digit(buf[i]) || !cn_is_digit(buf[i + 1]) || !cn_is_digit(buf[i + 2]))
        return kCNErrBadStatusLine;
    if (buf[i] < '1' || buf[i] > '5')   /* status class must be 1xx..5xx */
        return kCNErrBadStatusLine;
    out->status = (UInt16)((buf[i] - '0') * 100 +
                           (buf[i + 1] - '0') * 10 +
                           (buf[i + 2] - '0'));
    i += 3;

    if (i + 1 > len) return kCNErrHeadersIncomplete;
    if (buf[i] == ' ')
        i++;                            /* skip the SP before the reason phrase */
    else if (buf[i] != '\r')
        return kCNErrBadStatusLine;     /* neither reason nor end-of-line */

    while (i + 1 < len && !(buf[i] == '\r' && buf[i + 1] == '\n'))
        i++;
    if (i + 1 >= len) return kCNErrHeadersIncomplete;
    i += 2;                             /* past status-line CRLF */

    /* --- Header fields, until the blank line --- */
    for (;;) {
        UInt32 ns, ne, vs, ve;

        if (i + 1 > len) return kCNErrHeadersIncomplete;
        if (buf[i] == '\r') {
            if (i + 1 >= len) return kCNErrHeadersIncomplete;
            if (buf[i + 1] == '\n') {   /* blank line: end of header block */
                i += 2;
                out->bodyOffset = i;
                return noErr;
            }
            return kCNErrBadHeader;      /* lone CR */
        }

        /* field-name up to ':' */
        ns = i;
        while (i < len && buf[i] != ':' && buf[i] != '\r' && buf[i] != '\n')
            i++;
        if (i >= len) return kCNErrHeadersIncomplete;
        if (buf[i] != ':') return kCNErrBadHeader;   /* CR/LF before colon */
        ne = i;
        if (ne == ns) return kCNErrBadHeader;        /* empty field-name */
        i++;                                         /* skip ':' */

        while (i < len && cn_is_ows(buf[i]))         /* leading OWS */
            i++;

        vs = i;
        while (i + 1 < len && !(buf[i] == '\r' && buf[i + 1] == '\n'))
            i++;
        if (i + 1 >= len) return kCNErrHeadersIncomplete;
        ve = i;
        while (ve > vs && cn_is_ows(buf[ve - 1]))    /* trailing OWS */
            ve--;
        i += 2;                                      /* past header CRLF */

        if (out->headerCount >= CN_HTTP_MAX_HEADERS) return kCNErrTooManyHeaders;
        if (ne - ns >= CN_HTTP_MAX_NAME)  return kCNErrHeaderTooLong;
        if (ve - vs >= CN_HTTP_MAX_VALUE) return kCNErrHeaderTooLong;

        {
            CNHeaderField *h = &out->headers[out->headerCount];
            UInt32 k;
            for (k = 0; k < ne - ns; k++) h->name[k] = buf[ns + k];
            h->name[ne - ns] = '\0';
            for (k = 0; k < ve - vs; k++) h->value[k] = buf[vs + k];
            h->value[ve - vs] = '\0';
            out->headerCount++;
        }
    }
}
