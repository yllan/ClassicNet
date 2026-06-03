#ifndef CLASSICNET_CN_HPACK_H
#define CLASSICNET_CN_HPACK_H

#include "cn_types.h"

/*
 * HPACK header compression for HTTP/2 (RFC 7541).
 *
 * Decoder: maintains a dynamic table across header blocks on one connection,
 * so a CNHpackDec is per-connection state the caller owns and reuses. The
 * decode is strictly length-bounded and NUL-safe -- the primary fuzz target,
 * since the header block is fully attacker-controlled.
 *
 * Encoder: stateless and minimal -- it never touches the dynamic table and
 * emits literal representations (Huffman-free), using static-table indices for
 * known names. Spec-compliant and small; the dynamic-table/Huffman encoder is
 * an optional size optimization we deliberately skip on the client side.
 */

#define CN_HPACK_DYN_CAP     4096u  /* dynamic table arena (bytes) */
#define CN_HPACK_DYN_ENTRIES  128u  /* max live dynamic entries (4096/32) */
#define CN_HPACK_STR_MAX     4096u  /* max decoded name or value length */

typedef struct {
    unsigned char arena[CN_HPACK_DYN_CAP];   /* entry bytes, newest-first, packed */
    UInt16 nameLen[CN_HPACK_DYN_ENTRIES];    /* index 0 == newest entry */
    UInt16 valueLen[CN_HPACK_DYN_ENTRIES];
    UInt32 off[CN_HPACK_DYN_ENTRIES];        /* byte offset of entry i in arena */
    UInt32 count;                            /* number of live entries */
    UInt32 used;                             /* bytes of arena in use */
    UInt32 size;                             /* HPACK size: sum(n+v+32) */
    UInt32 maxSize;                          /* current table max (<= limit) */
    UInt32 limit;                            /* hard cap we advertised */
} CNHpackDec;

/*
 * Callback for each decoded header field. The name/value pointers are valid
 * only for the duration of the call (scratch storage). Return noErr to
 * continue; any other status aborts the decode and is returned to the caller.
 */
typedef OSStatus (*CNHpackSink)(void *ctx, const char *name, UInt32 nameLen,
                                const char *value, UInt32 valueLen);

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize a decoder. maxTableSize is the SETTINGS_HEADER_TABLE_SIZE the
 * caller advertised to the peer (clamped to CN_HPACK_DYN_CAP); it is the hard
 * upper bound the peer's dynamic-table-size updates may not exceed.
 */
void CN_HpackDecInit(CNHpackDec *d, UInt32 maxTableSize);

/*
 * Decode a complete header block, delivering each field to `sink`.
 *   noErr                  block fully decoded
 *   kCNErrHpackIncomplete  block ends mid-representation (truncated)
 *   kCNErrHpackBad*        malformed integer / index / Huffman
 *   kCNErrHpackOverflow    a decoded string exceeds CN_HPACK_STR_MAX
 *   (sink's status)        sink aborted
 */
OSStatus CN_HpackDecode(CNHpackDec *d, const unsigned char *block, UInt32 len,
                        CNHpackSink sink, void *ctx);

/*
 * Encode one header field (literal, never-indexed-free, Huffman-free) and
 * append it to out[*outLen .. outCap). Uses a static-table name index when the
 * name is known. Call once per field to build a header block.
 *   kCNErrBufferOverflow   the field does not fit
 */
OSStatus CN_HpackEncodeField(const char *name, UInt32 nameLen,
                             const char *value, UInt32 valueLen,
                             unsigned char *out, UInt32 outCap, UInt32 *outLen);

#ifdef __cplusplus
}
#endif

#endif /* CLASSICNET_CN_HPACK_H */
