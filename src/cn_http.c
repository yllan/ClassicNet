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

static int cn_wr(char *out, UInt32 cap, UInt32 *len, const char *s)
{
    while (*s) {
        if (*len >= cap) return 0;
        out[(*len)++] = *s++;
    }
    return 1;
}

OSStatus CN_BuildRequest(const char *method, const char *path, const char *host,
                         const CNHeaderKV *headers, UInt32 headerCount,
                         char *out, UInt32 outCap, UInt32 *outLen)
{
    UInt32 len = 0;
    UInt32 i;
    int ok = 1;

    if (method == 0 || path == 0 || host == 0 || out == 0)
        return kCNErrBadParam;
    if (headerCount > 0 && headers == 0)
        return kCNErrBadParam;

    ok &= cn_wr(out, outCap, &len, method);
    ok &= cn_wr(out, outCap, &len, " ");
    ok &= cn_wr(out, outCap, &len, path);
    ok &= cn_wr(out, outCap, &len, " HTTP/1.1\r\nHost: ");
    ok &= cn_wr(out, outCap, &len, host);
    ok &= cn_wr(out, outCap, &len, "\r\n");
    for (i = 0; i < headerCount; i++) {
        if (headers[i].name == 0 || headers[i].value == 0)
            return kCNErrBadParam;
        ok &= cn_wr(out, outCap, &len, headers[i].name);
        ok &= cn_wr(out, outCap, &len, ": ");
        ok &= cn_wr(out, outCap, &len, headers[i].value);
        ok &= cn_wr(out, outCap, &len, "\r\n");
    }
    ok &= cn_wr(out, outCap, &len, "\r\n");

    if (!ok || len >= outCap)     /* need one more byte for the NUL */
        return kCNErrBufferOverflow;
    out[len] = '\0';
    if (outLen) *outLen = len;
    return noErr;
}

static int cn_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Byte-at-a-time chunked-body state machine (robust to arbitrary input splits). */
enum {
    CKS_SIZE,        /* reading hex size digits */
    CKS_EXT,         /* skipping chunk-extension until CR */
    CKS_SIZE_LF,     /* expecting LF after the size-line CR */
    CKS_DATA,        /* copying chunkLeft data bytes */
    CKS_DATA_CR,     /* expecting CR after chunk data */
    CKS_DATA_LF,     /* expecting LF after that CR */
    CKS_TR,          /* trailer section, at the start of a line */
    CKS_TR_SKIP,     /* skipping a trailer header line until CR */
    CKS_TR_SKIP_LF,  /* expecting LF after a trailer-line CR */
    CKS_END_LF,      /* expecting the final LF of the terminating blank line */
    CKS_DONE
};

void CN_ChunkedInit(CNChunked *d)
{
    d->state = CKS_SIZE;
    d->chunkLeft = 0;
    d->sizeDigits = 0;
}

OSStatus CN_ChunkedFeed(CNChunked *d, const char *in, UInt32 inLen, UInt32 *consumed,
                        void (*sink)(void *ctx, const char *bytes, UInt32 len), void *ctx)
{
    UInt32 i = 0;

    while (i < inLen) {
        char c = in[i];
        switch (d->state) {
        case CKS_SIZE: {
            int hv = cn_hexval(c);
            if (hv >= 0) {
                if (d->chunkLeft > (0xFFFFFFFFul >> 4)) { *consumed = i; return kCNErrBadChunk; }
                d->chunkLeft = (d->chunkLeft << 4) | (UInt32)hv;
                d->sizeDigits++;
                i++;
            } else if (c == ';') {
                if (d->sizeDigits == 0) { *consumed = i; return kCNErrBadChunk; }
                d->state = CKS_EXT; i++;
            } else if (c == '\r') {
                if (d->sizeDigits == 0) { *consumed = i; return kCNErrBadChunk; }
                d->state = CKS_SIZE_LF; i++;
            } else {
                *consumed = i; return kCNErrBadChunk;
            }
            break;
        }
        case CKS_EXT:
            if (c == '\r') d->state = CKS_SIZE_LF;
            i++;
            break;
        case CKS_SIZE_LF:
            if (c != '\n') { *consumed = i; return kCNErrBadChunk; }
            i++;
            d->state = (d->chunkLeft == 0) ? CKS_TR : CKS_DATA;
            break;
        case CKS_DATA: {
            UInt32 avail = inLen - i;
            UInt32 take = d->chunkLeft < avail ? d->chunkLeft : avail;
            if (take > 0) { sink(ctx, in + i, take); i += take; d->chunkLeft -= take; }
            if (d->chunkLeft == 0) d->state = CKS_DATA_CR;
            break;
        }
        case CKS_DATA_CR:
            if (c != '\r') { *consumed = i; return kCNErrBadChunk; }
            d->state = CKS_DATA_LF; i++;
            break;
        case CKS_DATA_LF:
            if (c != '\n') { *consumed = i; return kCNErrBadChunk; }
            d->state = CKS_SIZE; d->sizeDigits = 0; i++;
            break;
        case CKS_TR:
            d->state = (c == '\r') ? CKS_END_LF : CKS_TR_SKIP;
            i++;
            break;
        case CKS_TR_SKIP:
            if (c == '\r') d->state = CKS_TR_SKIP_LF;
            i++;
            break;
        case CKS_TR_SKIP_LF:
            if (c != '\n') { *consumed = i; return kCNErrBadChunk; }
            d->state = CKS_TR; i++;
            break;
        case CKS_END_LF:
            if (c != '\n') { *consumed = i; return kCNErrBadChunk; }
            i++;
            d->state = CKS_DONE;
            *consumed = i;
            return noErr;
        default:
            *consumed = i;
            return kCNErrBadChunk;
        }
    }

    *consumed = i;
    return (d->state == CKS_DONE) ? noErr : kCNErrChunkIncomplete;
}

/* Single-shot decode into a caller buffer, built on the streaming decoder. */
typedef struct { char *out; UInt32 cap; UInt32 len; int overflow; } CNChunkCollect;

static void cn_chunk_collect(void *ctx, const char *bytes, UInt32 len)
{
    CNChunkCollect *cc = (CNChunkCollect *)ctx;
    UInt32 k;
    for (k = 0; k < len; k++) {
        if (cc->len >= cc->cap) { cc->overflow = 1; return; }
        cc->out[cc->len++] = bytes[k];
    }
}

OSStatus CN_DecodeChunked(const char *in, UInt32 inLen,
                          char *out, UInt32 outCap,
                          UInt32 *outLen, UInt32 *consumed)
{
    CNChunked d;
    CNChunkCollect cc;
    UInt32 used = 0;
    OSStatus s;

    if (in == 0 || out == 0 || outLen == 0 || consumed == 0)
        return kCNErrBadChunk;

    CN_ChunkedInit(&d);
    cc.out = out; cc.cap = outCap; cc.len = 0; cc.overflow = 0;
    s = CN_ChunkedFeed(&d, in, inLen, &used, cn_chunk_collect, &cc);
    if (cc.overflow) return kCNErrChunkOverflow;
    if (s != noErr) return s;
    *outLen = cc.len;
    *consumed = used;
    return noErr;
}
