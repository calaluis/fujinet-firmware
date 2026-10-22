#ifndef _TNFSLIB_KEEP_ALIVE_H
#define _TNFSLIB_KEEP_ALIVE_H

// Keep-alives for tnfsw:// mounts, run from the main loop (the esp_timer stack cannot hold
// TLS): one request per idle mount over its open connection, with no retry or reconnect.

#include <cstdint>
#include <mutex>

#include "tnfslibMountInfo.h"
#include "tnfslib_ws_stream.h"

constexpr uint32_t TNFS_KEEP_ALIVE_PERIOD_MS = 60000; // as the esp_timer of TCP and UDP mounts
// A reply this recent makes a tick skip its request: a mount is never silent for 90 s or more.
constexpr uint32_t TNFS_KEEP_ALIVE_RECENT_MS = 30000;
constexpr int TNFS_KEEP_ALIVE_TIMEOUT_MS = 1500; // reply wait of the single attempt

enum class tnfsKeepAliveResult
{
    RECENT, // a reply came less than TNFS_KEEP_ALIVE_RECENT_MS ago
    BUSY,   // another task is in a transaction on this mount
    CLOSED, // no open connection; a keep-alive never connects
    SENT,   // the server answered
    FAILED, // no answer; the next tick tries again
};

// One keep-alive tick for `m_info`, whose transport is `stream`. Never waits for another
// task's transaction and never connects; the mount's budget is restored afterwards.
tnfsKeepAliveResult tnfs_keep_alive(tnfsMountInfo *m_info, tnfsWsStream &stream, uint32_t now_ms);

// The mounts that rely on keep-alives from service(). Fixed capacity, no heap.
class tnfsKeepAliveRegistry
{
public:
    static constexpr int CAPACITY = 10; // MAX_HOSTS host slots, the boot mount and a spare
    // service() looks at the mounts at most this often; between looks it costs one compare.
    static constexpr uint32_t SCAN_MS = 1000;

    // Refused when the registry is full or already holds `m_info`. The first tick is
    // TNFS_KEEP_ALIVE_PERIOD_MS later.
    success_is_true add(tnfsMountInfo *m_info, tnfsWsStream *stream, uint32_t now_ms);
    // After this returns, service() never touches `m_info`; a keep-alive running on it is
    // waited for. Never call it while holding the mount's transaction_mutex.
    void remove(tnfsMountInfo *m_info);
    // At most one request per call, taking due mounts in turn so none starves.
    // Returns 1 if a request was made (answered or not), else 0.
    int service(uint32_t now_ms);

    int registered() const;

private:
    struct Entry
    {
        tnfsMountInfo *mount = nullptr;
        tnfsWsStream *stream = nullptr;
        uint32_t next_tick = 0;
    };

    Entry _entries[CAPACITY];
    int _cursor = 0;
    uint32_t _last_scan = 0;
    bool _scanned = false;
    mutable std::mutex _lock; // add, remove and service; held during the one request
};

#endif // _TNFSLIB_KEEP_ALIVE_H
