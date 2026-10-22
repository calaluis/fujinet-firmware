#include "tnfslib_keep_alive.h"

#include "tnfslib.h"

#include "../../include/debug.h"

tnfsKeepAliveResult tnfs_keep_alive(tnfsMountInfo *m_info, tnfsWsStream &stream, uint32_t now_ms)
{
    // A mount that replied recently is fresh for TNFSD and for the bridge: skip the round trip.
    if (now_ms - m_info->last_reply_ms.load() < TNFS_KEEP_ALIVE_RECENT_MS)
        return tnfsKeepAliveResult::RECENT;

    // A transaction running in another task keeps the session alive anyway; never wait.
    std::unique_lock<std::recursive_mutex> lock(m_info->transaction_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return tnfsKeepAliveResult::BUSY;
    if (!stream.is_open())
        return tnfsKeepAliveResult::CLOSED;

    // One short attempt over the open connection: no retry, no retry delay, and connecting is
    // refused outright (a session recovery inside the request would otherwise reconnect).
    uint8_t retries = m_info->max_retries;
    int timeout = m_info->timeout_ms;
    uint16_t retry_delay = m_info->min_retry_ms;
    m_info->max_retries = 1;
    m_info->timeout_ms = TNFS_KEEP_ALIVE_TIMEOUT_MS;
    m_info->min_retry_ms = 0;
    stream.allow_connect(false);

    tnfsStat st;
    // Any TNFS answer, even an error, renews the session; -1 means no answer at all.
    int result = tnfs_stat(m_info, &st, "keep-alive");

    stream.allow_connect(true);
    m_info->min_retry_ms = retry_delay;
    m_info->timeout_ms = timeout;
    m_info->max_retries = retries;

    if (result >= 0)
        return tnfsKeepAliveResult::SENT;
    Debug_printf("TNFS keep-alive to %s got no reply\r\n", m_info->hostname);
    return tnfsKeepAliveResult::FAILED;
}

success_is_true tnfsKeepAliveRegistry::add(tnfsMountInfo *m_info, tnfsWsStream *stream, uint32_t now_ms)
{
    std::lock_guard<std::mutex> lock(_lock);
    Entry *free_entry = nullptr;
    for (Entry &e : _entries)
    {
        if (e.mount == m_info)
            RETURN_ERROR_AS_FALSE();
        if (e.mount == nullptr && free_entry == nullptr)
            free_entry = &e;
    }
    if (free_entry == nullptr || stream == nullptr)
        RETURN_ERROR_AS_FALSE();
    *free_entry = Entry{m_info, stream, now_ms + TNFS_KEEP_ALIVE_PERIOD_MS};
    RETURN_SUCCESS_AS_TRUE();
}

void tnfsKeepAliveRegistry::remove(tnfsMountInfo *m_info)
{
    std::lock_guard<std::mutex> lock(_lock);
    for (Entry &e : _entries)
    {
        if (e.mount == m_info)
            e = Entry{};
    }
}

int tnfsKeepAliveRegistry::service(uint32_t now_ms)
{
    std::lock_guard<std::mutex> lock(_lock);
    if (_scanned && now_ms - _last_scan < SCAN_MS)
        return 0;
    _scanned = true;
    _last_scan = now_ms;

    for (int i = 0; i < CAPACITY; i++)
    {
        int index = (_cursor + i) % CAPACITY;
        Entry &e = _entries[index];
        // Signed difference: ticks compare correctly across the 49-day millis() wrap.
        if (e.mount == nullptr || static_cast<int32_t>(now_ms - e.next_tick) < 0)
            continue;
        // This tick is spent whatever happens; a tick that fell far behind restarts the period.
        e.next_tick += TNFS_KEEP_ALIVE_PERIOD_MS;
        if (static_cast<int32_t>(now_ms - e.next_tick) >= 0)
            e.next_tick = now_ms + TNFS_KEEP_ALIVE_PERIOD_MS;

        tnfsKeepAliveResult r = tnfs_keep_alive(e.mount, *e.stream, now_ms);
        if (r == tnfsKeepAliveResult::SENT || r == tnfsKeepAliveResult::FAILED)
        {
            _cursor = (index + 1) % CAPACITY;
            return 1;
        }
    }
    return 0;
}

int tnfsKeepAliveRegistry::registered() const
{
    std::lock_guard<std::mutex> lock(_lock);
    int n = 0;
    for (const Entry &e : _entries)
        n += e.mount != nullptr;
    return n;
}
