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
typedef int            SInt32;

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

/*
 * Apple's Universal Interfaces (3.4) provide OSStatus, Boolean, UInt*, OSErr,
 * noErr, true and false. MacTypes.h pulls them in. (When the toolchain was
 * built against the open-source multiversal interfaces instead, OSStatus was
 * missing and had to be defined here -- no longer needed with UI 3.4.)
 */
#include <MacTypes.h>

#endif /* CN_HOST */

#endif /* CLASSICNET_CN_TYPES_H */
