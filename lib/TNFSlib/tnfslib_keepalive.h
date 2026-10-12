#ifndef _TNFSLIB_KEEPALIVE_H
#define _TNFSLIB_KEEPALIVE_H

// Hands keep-alive work from a timer to a task that can afford network I/O. A timer
// callback only marks its client due (lock-free, never blocks, no I/O); the owning task
// later runs the work for every due client. Fixed capacity, no heap.

#include <atomic>
#include <mutex>

class tnfsKeepAliveQueue
{
public:
    static constexpr int CAPACITY = 8; // one per host slot
    using Work = void (*)(void *client);

    // False if the queue is full or `client` is already registered.
    bool add(void *client);

    // Unregister. Waits for a running service() call, so once this returns `client` is
    // never touched again and may be destroyed.
    void remove(void *client);

    // Ask for `client` to be serviced. Safe from any task or timer callback; never blocks.
    // A no-op for unregistered clients.
    void mark_due(void *client);

    // Run `work` once for every due client, in the calling task. Returns how many ran.
    // Cheap when nothing is due (one atomic load).
    int service(Work work);

    bool is_due(void *client) const;
    int registered() const;

private:
    struct Slot
    {
        std::atomic<void *> client{nullptr};
        std::atomic<bool> due{false};
    };

    void clear_due(Slot &slot);

    Slot _slots[CAPACITY];
    std::atomic<int> _pending{0};
    mutable std::mutex _lock; // serialises add, remove and service
};

#endif // _TNFSLIB_KEEPALIVE_H
