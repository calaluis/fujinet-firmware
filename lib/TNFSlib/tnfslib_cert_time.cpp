#include "tnfslib_cert_time.h"

namespace
{
// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's days_from_civil).
int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t to_seconds(const tnfsCertTime &t)
{
    return days_from_civil(t.year, t.mon, t.day) * 86400 + t.hour * 3600 + t.min * 60 + t.sec;
}
} // namespace

tnfsCertValidity tnfs_cert_validity(const tnfsCertTime &from, const tnfsCertTime &to,
                                    bool clock_synced, time_t now)
{
    if (!clock_synced || now < TNFS_CERT_EARLIEST_CLOCK)
        return tnfsCertValidity::NO_CLOCK;
    int64_t t = now;
    if (t < to_seconds(from))
        return tnfsCertValidity::NOT_YET_VALID;
    if (t > to_seconds(to))
        return tnfsCertValidity::EXPIRED;
    return tnfsCertValidity::VALID;
}
