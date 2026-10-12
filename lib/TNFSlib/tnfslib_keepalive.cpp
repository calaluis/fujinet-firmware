#include "tnfslib_keepalive.h"

void tnfsKeepAliveQueue::clear_due(Slot &slot)
{
    if (slot.due.exchange(false))
        _pending.fetch_sub(1);
}

bool tnfsKeepAliveQueue::add(void *client)
{
    if (client == nullptr)
        return false;
    std::lock_guard<std::mutex> lock(_lock);
    Slot *free_slot = nullptr;
    for (Slot &slot : _slots)
    {
        void *c = slot.client.load();
        if (c == client)
            return false;
        if (c == nullptr && free_slot == nullptr)
            free_slot = &slot;
    }
    if (free_slot == nullptr)
        return false;
    clear_due(*free_slot); // a late mark_due() may have hit the slot while it was free
    free_slot->client.store(client);
    return true;
}

void tnfsKeepAliveQueue::remove(void *client)
{
    std::lock_guard<std::mutex> lock(_lock); // also waits for a running service()
    for (Slot &slot : _slots)
    {
        if (slot.client.load() == client)
        {
            slot.client.store(nullptr);
            clear_due(slot);
        }
    }
}

void tnfsKeepAliveQueue::mark_due(void *client)
{
    if (client == nullptr)
        return;
    for (Slot &slot : _slots)
    {
        if (slot.client.load() == client)
        {
            if (!slot.due.exchange(true))
                _pending.fetch_add(1);
            return;
        }
    }
}

int tnfsKeepAliveQueue::service(Work work)
{
    if (_pending.load() <= 0)
        return 0;
    std::lock_guard<std::mutex> lock(_lock);
    int ran = 0;
    for (Slot &slot : _slots)
    {
        if (!slot.due.exchange(false))
            continue;
        _pending.fetch_sub(1);
        void *client = slot.client.load();
        if (client != nullptr && work != nullptr)
        {
            work(client);
            ran++;
        }
    }
    return ran;
}

bool tnfsKeepAliveQueue::is_due(void *client) const
{
    for (const Slot &slot : _slots)
        if (slot.client.load() == client)
            return slot.due.load();
    return false;
}

int tnfsKeepAliveQueue::registered() const
{
    int n = 0;
    for (const Slot &slot : _slots)
        n += slot.client.load() != nullptr;
    return n;
}
