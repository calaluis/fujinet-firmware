#ifndef _TNFSLIB_CALLBACK_GATE_H
#define _TNFSLIB_CALLBACK_GATE_H

// Lets a timer callback reach an object only while the object is alive.
//
// esp_timer_stop() and esp_timer_delete() do not wait for a callback that is already
// running (ESP-IDF 5.4: timer_process_alarm() unlocks the timer list before calling it),
// so a destructor cannot rely on them alone. The callback passes its raw `arg` to run(),
// which only compares addresses until it has found the object registered; close()
// unregisters and waits for a callback already inside. The lock is never held while the
// work runs, so the work may block (network I/O) without holding up other objects.
// Fixed capacity, no heap.

#include <condition_variable>
#include <mutex>

class tnfsCallbackGate
{
public:
    static constexpr int CAPACITY = 16; // host slots plus the global TNFS file system
    using Work = void (*)(void *client);

    // False if the gate is full, `client` is null or already open.
    bool open(void *client);

    // Unregister `client` and wait until no run() for it is in progress. Afterwards run()
    // never calls work for it, so it may be destroyed. Never call it from inside the work
    // for the same client: that would wait for itself.
    void close(void *client);

    // Call `work(client)` if `client` is open and not closing. Returns whether it ran.
    bool run(void *client, Work work);

    bool is_open(void *client) const;

private:
    struct Slot
    {
        void *client = nullptr;
        bool closing = false;
        int busy = 0; // run() calls in progress
    };

    Slot *find(void *client);

    Slot _slots[CAPACITY];
    mutable std::mutex _lock;
    std::condition_variable _idle;
};

#endif // _TNFSLIB_CALLBACK_GATE_H
