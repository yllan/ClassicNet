/*
 * ClassicNet mbedTLS user config (appended to the vendored config).
 *
 * Turns ON X.509 validity-date checking and backs mbedTLS's clock with the Mac
 * Time Manager via a compile-time macro -> cn_mac_time (target/cn_mac_time.c).
 * Compile-time (not the runtime mbedtls_platform_set_time hook) so there is no
 * NULL-function-pointer window. Apply by building mbedTLS with
 *   -DMBEDTLS_USER_CONFIG_FILE="<this file>"
 * (see scripts/setup-mbedtls.sh). Any app linking that mbedTLS must provide
 * cn_mac_time; only the TLS apps do.
 */
#ifndef CN_MBEDTLS_USERCONFIG_H
#define CN_MBEDTLS_USERCONFIG_H

#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_TIME_TYPE_MACRO long
#define MBEDTLS_PLATFORM_TIME_MACRO     cn_mac_time
/* classic Mac has no gmtime_r / clock_gettime; supply our own (cn_mac_time.c) */
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT

#ifndef __ASSEMBLER__
long cn_mac_time(long *timer);
#endif

#endif /* CN_MBEDTLS_USERCONFIG_H */
