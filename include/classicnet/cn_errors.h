#ifndef CLASSICNET_CN_ERRORS_H
#define CLASSICNET_CN_ERRORS_H

#include "cn_types.h"

/*
 * ClassicNet reserves a private OSStatus range starting at kCNErrBase and
 * counting downward (see DESIGN.md, locked decision #2: errors are OSStatus).
 */
enum {
    kCNErrBase              = -30000,
    kCNErrBadURL            = -30001,
    kCNErrUnsupportedScheme = -30002,
    kCNErrHostTooLong       = -30003,
    kCNErrPathTooLong       = -30004,
    kCNErrBadPort           = -30005,

    /* HTTP/1.x response parser */
    kCNErrHeadersIncomplete = -30010,  /* need more bytes; not an error per se */
    kCNErrBadStatusLine     = -30011,
    kCNErrBadHeader         = -30012,
    kCNErrTooManyHeaders    = -30013,
    kCNErrHeaderTooLong     = -30014,

    /* WebSocket frame parser */
    kCNErrFrameIncomplete   = -30020,  /* need more bytes; not an error per se */
    kCNErrBadFrame          = -30021,
    kCNErrFrameTooLarge     = -30022   /* 64-bit length exceeds 32-bit on target */
};

#endif /* CLASSICNET_CN_ERRORS_H */
