/*
 * Time platform layer for mbedTLS on classic Mac OS, so X.509 validity dates
 * are checked against the real clock. Wired in by target/mbedtls_userconfig.h:
 *   - cn_mac_time                 (MBEDTLS_PLATFORM_TIME_MACRO): now, Unix secs
 *   - mbedtls_platform_gmtime_r   (MBEDTLS_PLATFORM_GMTIME_R_ALT): UTC breakdown
 *   - mbedtls_ms_time             (MBEDTLS_PLATFORM_MS_TIME_ALT): monotonic ms
 */
#include "mbedtls/build_info.h"
#include "mbedtls/platform_util.h"

#include "cn_mac_time.h"

#include <OSUtils.h>   /* GetDateTime */
#include <Events.h>    /* TickCount */
#include <Timer.h>     /* Microseconds */
#include <time.h>      /* struct tm */

/* Seconds between the classic Mac epoch (1904-01-01) and Unix (1970-01-01). */
#define CN_MAC_TO_UNIX 2082844800UL

long cn_mac_time(long *timer)
{
    unsigned long macSecs = 0;
    long unixSecs;

    GetDateTime(&macSecs);                       /* seconds since 1904-01-01 */
    unixSecs = (long)(macSecs - CN_MAC_TO_UNIX); /* -> seconds since 1970 */
    if (timer)
        *timer = unixSecs;
    return unixSecs;
}

/* Portable UTC breakdown of Unix seconds (Howard Hinnant's civil-from-days).
   mbedTLS only reads year/mon/mday/hour/min/sec for X.509 date comparison. */
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf)
{
    long t = (long)*tt;
    long days = t / 86400;
    long rem  = t % 86400;
    long z, era, y;
    unsigned long doe, yoe, doy, mp, d, m;

    if (rem < 0) { rem += 86400; days -= 1; }
    tm_buf->tm_hour = (int)(rem / 3600); rem %= 3600;
    tm_buf->tm_min  = (int)(rem / 60);
    tm_buf->tm_sec  = (int)(rem % 60);
    tm_buf->tm_wday = (int)((days % 7 + 4 + 7) % 7);  /* 1970-01-01 = Thu (4) */

    z   = days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned long)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y   = (long)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    d   = doy - (153 * mp + 2) / 5 + 1;
    m   = mp < 10 ? mp + 3 : mp - 9;

    tm_buf->tm_year  = (int)(y + (m <= 2)) - 1900;
    tm_buf->tm_mon   = (int)m - 1;
    tm_buf->tm_mday  = (int)d;
    tm_buf->tm_yday  = 0;
    tm_buf->tm_isdst = 0;
    return tm_buf;
}

/* Monotonic-ish milliseconds for TLS timers (TickCount is 1/60 s). */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)TickCount() * 1000 / 60;
}

/* Gather a little timing jitter to stir into the TLS RNG before the handshake:
   Microseconds reads spaced by variable work, plus uninitialised stack. This is
   supplementary mixing, not a real entropy source -- see CN_TlsAddEntropy. */
void cn_collect_jitter(unsigned char *buf, unsigned long n)
{
    UnsignedWide t;
    unsigned char junk[32];          /* deliberately uninitialised */
    unsigned long i, acc;
    int j;
    for (i = 0; i < n; i++) {
        Microseconds(&t);
        acc = (unsigned long)t.lo ^ ((unsigned long)t.hi << 13) ^ (unsigned long)TickCount();
        for (j = 0; j < (int)(t.lo & 7); j++) acc = acc * 1103515245u + 12345u;
        buf[i] = (unsigned char)(acc ^ junk[i & 31]);
    }
}
