#ifndef _TNFSLIB_CERT_TIME_H
#define _TNFSLIB_CERT_TIME_H

// Certificate validity dates for TNFS over WSS. FujiNet builds mbedTLS without
// MBEDTLS_HAVE_TIME_DATE, so the library never checks them; the WSS transport does, here, and
// only for its own connections. Without a trusted clock no certificate is accepted.

#include <cstdint>
#include <ctime>

// UTC, as in mbedtls_x509_time.
struct tnfsCertTime
{
    int year, mon, day, hour, min, sec;
};

enum class tnfsCertValidity
{
    VALID,
    EXPIRED,
    NOT_YET_VALID,
    NO_CLOCK, // the clock has not been set by SNTP, or is plainly wrong
};

// Earlier than this, the clock cannot be right (2024-01-01T00:00:00Z).
constexpr time_t TNFS_CERT_EARLIEST_CLOCK = 1704067200;

// `now` is UTC seconds since the epoch; `clock_synced` is whether SNTP has set it since boot.
tnfsCertValidity tnfs_cert_validity(const tnfsCertTime &from, const tnfsCertTime &to,
                                    bool clock_synced, time_t now);

#endif // _TNFSLIB_CERT_TIME_H
