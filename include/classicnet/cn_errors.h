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
    kCNErrBadPort           = -30005
};

#endif /* CLASSICNET_CN_ERRORS_H */
