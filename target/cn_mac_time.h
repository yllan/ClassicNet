#ifndef CN_MAC_TIME_H
#define CN_MAC_TIME_H

/*
 * Classic Mac platform glue shared by the on-target TLS apps (cn_mac_time.c).
 *   - cn_mac_time       : the mbedTLS time source (also wired via the
 *                         MBEDTLS_PLATFORM_TIME_MACRO in mbedtls_userconfig.h).
 *   - cn_collect_jitter : fill buf[0,n) with timing jitter to feed
 *                         CN_TlsAddEntropy before a handshake (every TLS app
 *                         should, since the machine has no hardware RNG).
 */
long cn_mac_time(long *timer);
void cn_collect_jitter(unsigned char *buf, unsigned long n);

#endif /* CN_MAC_TIME_H */
