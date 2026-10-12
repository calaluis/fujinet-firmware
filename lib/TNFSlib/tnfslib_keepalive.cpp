#include "tnfslib_keepalive.h"

static uintptr_t word_of(void *client) { return reinterpret_cast<uintptr_t>(client); }

bool tnfsKeepAliveQueue::add(void *client)
{
    uintptr_t word = word_of(client);
    if (word == 0 || (word & DUE) != 0)
        return false;
    std::lock_guard<std::mutex> lock(_lock);
    std::atomic<uintptr_t> *free_slot = nullptr;
    for (auto &slot : _slots)
    {
        uintptr_t w = slot.load();
        if ((w & ~DUE) == word)
            return false;
        if (w == 0 && free_slot == nullptr)
            free_slot = &slot;
    }
    if (free_slot == nullptr)
        return false;
    free_slot->store(word); // free slots are always 0: marks only land on registered clients
    return true;
}

void tnfsKeepAliveQueue::remove(void *client)
{
    uintptr_t word = word_of(client);
    if (word == 0)
        return;
    std::lock_guard<std::mutex> lock(_lock); // also waits for a running service()
    for (auto &slot : _slots)
    {
        if ((slot.load() & ~DUE) == word && (slot.exchange(0) & DUE) != 0)
            _pending.fetch_sub(1);
    }
}

void tnfsKeepAliveQueue::mark_due(void *client)
{
    uintptr_t word = word_of(client);
    if (word == 0)
        return;
    for (auto &slot : _slots)
    {
        uintptr_t w = slot.load();
        // A failed exchange reloads `w`: the slot was freed, reused or marked meanwhile.
        while ((w & ~DUE) == word)
        {
            if ((w & DUE) != 0)
                return; // already due
            if (slot.compare_exchange_weak(w, w | DUE))
            {
                _pending.fetch_add(1);
                return;
            }
        }
    }
}

int tnfsKeepAliveQueue::service(Work work)
{
    if (_pending.load() <= 0)
        return 0;
    std::lock_guard<std::mutex> lock(_lock);
    int ran = 0;
    for (auto &slot : _slots)
    {
        uintptr_t w = slot.fetch_and(~DUE);
        if ((w & DUE) == 0)
            continue;
        _pending.fetch_sub(1);
        if (work != nullptr)
        {
            work(reinterpret_cast<void *>(w & ~DUE));
            ran++;
        }
    }
    return ran;
}

bool tnfsKeepAliveQueue::is_due(void *client) const
{
    uintptr_t word = word_of(client);
    for (const auto &slot : _slots)
    {
        uintptr_t w = slot.load();
        if (word != 0 && (w & ~DUE) == word)
            return (w & DUE) != 0;
    }
    return false;
}

int tnfsKeepAliveQueue::registered() const
{
    int n = 0;
    for (const auto &slot : _slots)
        n += slot.load() != 0;
    return n;
}
