#include "classicnet/cn_hpack.h"
#include "classicnet/cn_errors.h"

#include <string.h>

#include "cn_hpack_tables.h"   /* cn_huff_code/len[257], cn_hpack_static[61] */

/* ------------------------------------------------------------------ *
 * Huffman decoding tree (RFC 7541 Appendix B), built once on demand.
 * Each node has two child slots. A non-negative child is an internal node
 * index; a value <= -1 encodes a leaf symbol as (-1 - symbol). 0 means "no
 * child yet" during build (root is node 0, never a child target).
 * ------------------------------------------------------------------ */
#define CN_HUFF_NODES 513
static short cn_huff_tree[CN_HUFF_NODES][2];
static int   cn_huff_nodes = 0;
static int   cn_huff_built = 0;

static void cn_huff_build(void)
{
    int sym;
    if (cn_huff_built)
        return;
    /* node 0 is the root; children start as 0 ("absent"). */
    cn_huff_tree[0][0] = 0;
    cn_huff_tree[0][1] = 0;
    cn_huff_nodes = 1;

    for (sym = 0; sym < 257; sym++) {
        UInt32 code = cn_huff_code[sym];
        int    len  = cn_huff_len[sym];
        int    node = 0;
        int    k;
        for (k = len - 1; k >= 0; k--) {
            int bit = (int)((code >> k) & 1u);
            if (k == 0) {
                cn_huff_tree[node][bit] = (short)(-1 - sym);   /* leaf */
            } else {
                short next = cn_huff_tree[node][bit];
                if (next <= 0) {                                /* need a child */
                    next = (short)cn_huff_nodes++;
                    cn_huff_tree[next][0] = 0;
                    cn_huff_tree[next][1] = 0;
                    cn_huff_tree[node][bit] = next;
                }
                node = next;
            }
        }
    }
    cn_huff_built = 1;
}

/* Decode `len` Huffman bytes from src into out[0,outCap). */
static OSStatus cn_huff_decode(const unsigned char *src, UInt32 len,
                               char *out, UInt32 outCap, UInt32 *outLen)
{
    UInt32 i, n = 0;
    int node = 0;
    int bitsAtRoot = 0;   /* trailing bits consumed since last leaf, for padding check */

    cn_huff_build();
    for (i = 0; i < len; i++) {
        int b;
        for (b = 7; b >= 0; b--) {
            int bit = (src[i] >> b) & 1;
            short next = cn_huff_tree[node][bit];
            bitsAtRoot++;
            if (next == 0)                          /* no such edge: invalid code */
                return kCNErrHpackBadHuffman;
            if (next < 0) {                         /* leaf */
                int sym = -1 - next;
                if (sym == 256)                     /* EOS must never be emitted */
                    return kCNErrHpackBadHuffman;
                if (n >= outCap)
                    return kCNErrHpackOverflow;
                out[n++] = (char)sym;
                node = 0;
                bitsAtRoot = 0;
            } else {
                node = next;
            }
        }
    }
    /* Valid padding: we ended at the root, with <=7 leftover bits that were all
       ones. If node != 0 we stopped mid-code; the leftover-bit count is how many
       bits we descended past the last leaf. */
    if (node != 0) {
        int depth = bitsAtRoot;
        UInt32 mask, val;
        if (depth > 7)
            return kCNErrHpackBadHuffman;           /* padding too long */
        /* the path we took must be the all-ones prefix (EOS prefix) */
        mask = (1u << depth) - 1u;
        val  = cn_huff_code[256] >> (cn_huff_len[256] - depth);  /* top `depth` EOS bits */
        (void)val;
        /* The all-ones requirement: every bit we descended on must be 1. We can
           verify by walking the EOS code's top bits, which are all ones, so the
           padding path equals mask only if each step took bit 1. Reconstruct: */
        {
            /* Rebuild the bits taken from the last partial byte path is complex;
               instead require the descended path to be all-ones by re-deriving:
               a node reached only via 1-bits is exactly the EOS prefix node. */
            int t = 0, j;
            for (j = 0; j < depth; j++)
                t = cn_huff_tree[t][1];
            if (t != node)
                return kCNErrHpackBadHuffman;       /* padding bits were not all ones */
        }
        (void)mask;
    }
    if (outLen)
        *outLen = n;
    return noErr;
}

/* ------------------------------------------------------------------ *
 * A small cursor over the encoded block.
 * ------------------------------------------------------------------ */
typedef struct {
    const unsigned char *p;
    UInt32 len;
    UInt32 pos;
} CNHpackIn;

/* HPACK integer with an N-bit prefix (RFC 7541 §5.1). */
static OSStatus cn_hpack_int(CNHpackIn *in, int prefixBits, UInt32 *out)
{
    UInt32 maxPrefix = (1u << prefixBits) - 1u;
    UInt32 value;
    int shift = 0;

    if (in->pos >= in->len)
        return kCNErrHpackIncomplete;
    value = in->p[in->pos++] & maxPrefix;
    if (value < maxPrefix) {
        *out = value;
        return noErr;
    }
    for (;;) {
        UInt32 b;
        if (in->pos >= in->len)
            return kCNErrHpackIncomplete;
        b = in->p[in->pos++];
        if (shift > 21)                              /* >4 octets: reject (overflow guard) */
            return kCNErrHpackBadInt;
        value += (b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0)
            break;
        shift += 7;
    }
    *out = value;
    return noErr;
}

/* HPACK string literal (RFC 7541 §5.2) into out[0,outCap). */
static OSStatus cn_hpack_str(CNHpackIn *in, char *out, UInt32 outCap, UInt32 *outLen)
{
    int huff;
    UInt32 slen;
    OSStatus err;

    if (in->pos >= in->len)
        return kCNErrHpackIncomplete;
    huff = (in->p[in->pos] & 0x80u) ? 1 : 0;
    err = cn_hpack_int(in, 7, &slen);
    if (err != noErr)
        return err;
    if (slen > in->len - in->pos)
        return kCNErrHpackIncomplete;

    if (huff) {
        err = cn_huff_decode(in->p + in->pos, slen, out, outCap, outLen);
        if (err != noErr)
            return err;
    } else {
        if (slen > outCap)
            return kCNErrHpackOverflow;
        if (slen)
            memcpy(out, in->p + in->pos, slen);
        if (outLen)
            *outLen = slen;
    }
    in->pos += slen;
    return noErr;
}

/* ------------------------------------------------------------------ *
 * Dynamic table (RFC 7541 §2.3, §4). Newest entry is index 0; bytes packed
 * newest-first in `arena`. Insert prepends (memmove existing forward); eviction
 * drops the oldest (highest index, tail of arena).
 * ------------------------------------------------------------------ */
void CN_HpackDecInit(CNHpackDec *d, UInt32 maxTableSize)
{
    if (maxTableSize > CN_HPACK_DYN_CAP)
        maxTableSize = CN_HPACK_DYN_CAP;
    d->count = 0;
    d->used  = 0;
    d->size  = 0;
    d->maxSize = maxTableSize;
    d->limit   = maxTableSize;
}

static void cn_dyn_evict_to(CNHpackDec *d, UInt32 target)
{
    while (d->count > 0 && d->size > target) {
        UInt32 last = d->count - 1;                  /* oldest */
        UInt32 esz  = d->nameLen[last] + d->valueLen[last] + 32u;
        d->used -= (d->nameLen[last] + d->valueLen[last]);
        d->size -= esz;
        d->count = last;
    }
}

static void cn_dyn_insert(CNHpackDec *d, const char *name, UInt32 nlen,
                          const char *value, UInt32 vlen)
{
    UInt32 esz  = nlen + vlen + 32u;
    UInt32 need = nlen + vlen;
    UInt32 i;

    /* Evict to make room for the new entry (RFC 7541 §4.4). */
    if (esz > d->maxSize) {
        cn_dyn_evict_to(d, 0);                       /* entry too big: empties table */
        return;
    }
    cn_dyn_evict_to(d, d->maxSize - esz);
    if (d->count >= CN_HPACK_DYN_ENTRIES || need > d->maxSize)
        return;                                      /* defensive: shouldn't happen */

    /* Prepend: shift existing bytes and metadata forward by one slot. */
    memmove(d->arena + need, d->arena, d->used);
    for (i = d->count; i > 0; i--) {
        d->nameLen[i]  = d->nameLen[i - 1];
        d->valueLen[i] = d->valueLen[i - 1];
        d->off[i]      = d->off[i - 1] + need;
    }
    memcpy(d->arena, name, nlen);
    memcpy(d->arena + nlen, value, vlen);
    d->off[0]      = 0;
    d->nameLen[0]  = (UInt16)nlen;
    d->valueLen[0] = (UInt16)vlen;
    d->count++;
    d->used += need;
    d->size += esz;
}

/* ------------------------------------------------------------------ *
 * Resolve a table index to a name/value field and feed it to the sink.
 * ------------------------------------------------------------------ */
static OSStatus cn_emit_indexed(CNHpackDec *d, UInt32 idx,
                                CNHpackSink sink, void *ctx)
{
    if (idx == 0)
        return kCNErrHpackBadIndex;
    if (idx <= 61) {
        const CNHpackStatic *e = &cn_hpack_static[idx - 1];
        return sink(ctx, e->name, (UInt32)strlen(e->name),
                    e->value, (UInt32)strlen(e->value));
    }
    {
        UInt32 dynPos = idx - 62;                     /* 0 == newest */
        const unsigned char *base;
        if (dynPos >= d->count)
            return kCNErrHpackBadIndex;
        base = d->arena + d->off[dynPos];
        return sink(ctx, (const char *)base, d->nameLen[dynPos],
                    (const char *)base + d->nameLen[dynPos], d->valueLen[dynPos]);
    }
}

/* Copy the *name* of a table index into out[0,outCap). */
static OSStatus cn_name_of(CNHpackDec *d, UInt32 idx, char *out, UInt32 outCap, UInt32 *outLen)
{
    if (idx == 0)
        return kCNErrHpackBadIndex;
    if (idx <= 61) {
        UInt32 n = (UInt32)strlen(cn_hpack_static[idx - 1].name);
        if (n > outCap) return kCNErrHpackOverflow;
        memcpy(out, cn_hpack_static[idx - 1].name, n);
        *outLen = n;
        return noErr;
    }
    {
        UInt32 dynPos = idx - 62;
        if (dynPos >= d->count) return kCNErrHpackBadIndex;
        if (d->nameLen[dynPos] > outCap) return kCNErrHpackOverflow;
        memcpy(out, d->arena + d->off[dynPos], d->nameLen[dynPos]);
        *outLen = d->nameLen[dynPos];
        return noErr;
    }
}

OSStatus CN_HpackDecode(CNHpackDec *d, const unsigned char *block, UInt32 len,
                        CNHpackSink sink, void *ctx)
{
    CNHpackIn in;
    static char nameBuf[CN_HPACK_STR_MAX];
    static char valBuf[CN_HPACK_STR_MAX];

    if (d == 0 || (block == 0 && len != 0) || sink == 0)
        return kCNErrBadParam;
    in.p = block; in.len = len; in.pos = 0;

    while (in.pos < in.len) {
        unsigned char b = in.p[in.pos];
        OSStatus err;

        if (b & 0x80u) {                              /* §6.1 Indexed Header Field */
            UInt32 idx;
            err = cn_hpack_int(&in, 7, &idx);
            if (err != noErr) return err;
            err = cn_emit_indexed(d, idx, sink, ctx);
            if (err != noErr) return err;

        } else if (b & 0x40u) {                       /* §6.2.1 Literal, incr. indexing */
            UInt32 idx, nlen, vlen;
            err = cn_hpack_int(&in, 6, &idx);
            if (err != noErr) return err;
            if (idx == 0) {
                err = cn_hpack_str(&in, nameBuf, sizeof(nameBuf), &nlen);
                if (err != noErr) return err;
            } else {
                err = cn_name_of(d, idx, nameBuf, sizeof(nameBuf), &nlen);
                if (err != noErr) return err;
            }
            err = cn_hpack_str(&in, valBuf, sizeof(valBuf), &vlen);
            if (err != noErr) return err;
            err = sink(ctx, nameBuf, nlen, valBuf, vlen);
            if (err != noErr) return err;
            cn_dyn_insert(d, nameBuf, nlen, valBuf, vlen);

        } else if ((b & 0x20u) == 0) {                /* §6.2.2/6.2.3 Literal, no index */
            UInt32 idx, nlen, vlen;
            err = cn_hpack_int(&in, 4, &idx);         /* same prefix for 0x00 and 0x10 */
            if (err != noErr) return err;
            if (idx == 0) {
                err = cn_hpack_str(&in, nameBuf, sizeof(nameBuf), &nlen);
                if (err != noErr) return err;
            } else {
                err = cn_name_of(d, idx, nameBuf, sizeof(nameBuf), &nlen);
                if (err != noErr) return err;
            }
            err = cn_hpack_str(&in, valBuf, sizeof(valBuf), &vlen);
            if (err != noErr) return err;
            err = sink(ctx, nameBuf, nlen, valBuf, vlen);
            if (err != noErr) return err;

        } else {                                      /* §6.3 Dynamic table size update */
            UInt32 newMax;
            err = cn_hpack_int(&in, 5, &newMax);
            if (err != noErr) return err;
            if (newMax > d->limit)
                return kCNErrHpackOverflow;           /* exceeds advertised cap */
            d->maxSize = newMax;
            cn_dyn_evict_to(d, d->maxSize);
        }
    }
    return noErr;
}

/* ------------------------------------------------------------------ *
 * Minimal stateless encoder: literal, without indexing (§6.2.2), no Huffman.
 * ------------------------------------------------------------------ */
static OSStatus cn_emit_int(UInt32 value, int prefixBits, unsigned char first,
                            unsigned char *out, UInt32 outCap, UInt32 *pos)
{
    UInt32 maxPrefix = (1u << prefixBits) - 1u;
    if (value < maxPrefix) {
        if (*pos >= outCap) return kCNErrBufferOverflow;
        out[(*pos)++] = (unsigned char)(first | value);
        return noErr;
    }
    if (*pos >= outCap) return kCNErrBufferOverflow;
    out[(*pos)++] = (unsigned char)(first | maxPrefix);
    value -= maxPrefix;
    while (value >= 0x80u) {
        if (*pos >= outCap) return kCNErrBufferOverflow;
        out[(*pos)++] = (unsigned char)((value & 0x7Fu) | 0x80u);
        value >>= 7;
    }
    if (*pos >= outCap) return kCNErrBufferOverflow;
    out[(*pos)++] = (unsigned char)value;
    return noErr;
}

static OSStatus cn_emit_str(const char *s, UInt32 slen,
                            unsigned char *out, UInt32 outCap, UInt32 *pos)
{
    OSStatus err = cn_emit_int(slen, 7, 0x00, out, outCap, pos);  /* H=0, raw */
    if (err != noErr) return err;
    if (*pos + slen > outCap) return kCNErrBufferOverflow;
    if (slen) memcpy(out + *pos, s, slen);
    *pos += slen;
    return noErr;
}

/* Find a static-table entry whose name matches; return its 1-based index or 0. */
static UInt32 cn_static_name_index(const char *name, UInt32 nlen)
{
    UInt32 i;
    for (i = 0; i < 61; i++) {
        const char *sn = cn_hpack_static[i].name;
        if (strlen(sn) == nlen && memcmp(sn, name, nlen) == 0)
            return i + 1;
    }
    return 0;
}

OSStatus CN_HpackEncodeField(const char *name, UInt32 nameLen,
                             const char *value, UInt32 valueLen,
                             unsigned char *out, UInt32 outCap, UInt32 *outLen)
{
    UInt32 pos = (outLen ? *outLen : 0);
    UInt32 idx;
    OSStatus err;

    if (name == 0 || out == 0 || (value == 0 && valueLen != 0))
        return kCNErrBadParam;

    idx = cn_static_name_index(name, nameLen);
    if (idx) {
        /* literal without indexing, name from static index (4-bit prefix, 0x00) */
        err = cn_emit_int(idx, 4, 0x00, out, outCap, &pos);
        if (err != noErr) return err;
    } else {
        if (pos >= outCap) return kCNErrBufferOverflow;
        out[pos++] = 0x00;                            /* literal, new name */
        err = cn_emit_str(name, nameLen, out, outCap, &pos);
        if (err != noErr) return err;
    }
    err = cn_emit_str(value, valueLen, out, outCap, &pos);
    if (err != noErr) return err;

    if (outLen) *outLen = pos;
    return noErr;
}
