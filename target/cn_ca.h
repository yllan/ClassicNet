#ifndef CN_CA_H
#define CN_CA_H

/*
 * Trust-anchor selection shared by the on-target HTTPS/HTTP-2 apps.
 *   -DCN_CA_BUNDLE  embed the real Mozilla roots (scripts/gen-ca-bundle.sh
 *                   -> target/ca_bundle.h) to verify real public servers.
 *   -DCN_VERIFY     embed the throwaway test CA (scripts/gen-test-pki.sh
 *                   -> target/test_ca.h) for the local demo server.
 *   neither         no CA: certificate verification disabled (insecure).
 * When CN_CA is defined, use CN_CA / CN_CA_LEN with CN_TlsCreate and announce
 * CN_CA_DESC.
 */
#if defined(CN_CA_BUNDLE)
#include "ca_bundle.h"
#define CN_CA      kCABundle
#define CN_CA_LEN  ((UInt32)sizeof(kCABundle))
#define CN_CA_DESC "Mozilla root bundle"
#elif defined(CN_VERIFY)
#include "test_ca.h"
#define CN_CA      kTestCA
#define CN_CA_LEN  ((UInt32)sizeof(kTestCA))
#define CN_CA_DESC "embedded test CA"
#endif

#endif /* CN_CA_H */
