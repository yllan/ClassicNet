#ifndef CLASSICNET_CN_TYPES_H
#define CLASSICNET_CN_TYPES_H

/*
 * Portable type seam.
 *
 * On the real target (Retro68 / CodeWarrior) these types come from the Mac
 * Universal / Multiversal Interfaces.  On the host (Linux / macOS) we define
 * compatible types so the portable protocol code can be compiled and tested
 * natively under sanitizers and fuzzers -- tools that do not exist on
 * Classic Mac OS.  Define CN_HOST for host builds.
 */

#ifdef CN_HOST

typedef int            OSStatus;   /* Mac OSStatus is a 32-bit signed value */
typedef unsigned char  Boolean;
typedef unsigned char  UInt8;
typedef unsigned short UInt16;
typedef unsigned int   UInt32;

#ifndef noErr
#define noErr 0
#endif
#ifndef true
#define true  1
#endif
#ifndef false
#define false 0
#endif

#else  /* real Mac target */

#include <MacTypes.h>   /* Boolean, UInt8/16/32, SInt16/32, OSErr, noErr */

/*
 * Retro68's (and classic non-Carbon) interfaces define OSErr (16-bit) but not
 * OSStatus (the 32-bit Carbon-era type).  We use OSStatus throughout
 * (DESIGN.md decision #2), so provide it.  No header in CIncludes typedefs it,
 * so this is the single definition.
 */
typedef SInt32 OSStatus;

#ifndef true
#define true  1
#endif
#ifndef false
#define false 0
#endif
/* noErr is already provided as an enum constant by MacTypes.h. */

#endif /* CN_HOST */

#endif /* CLASSICNET_CN_TYPES_H */
