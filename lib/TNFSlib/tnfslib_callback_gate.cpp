#include "tnfslib_callback_gate.h"

tnfsCallbackGate::Slot *tnfsCallbackGate::find(void *client)
{
    for (Slot &slot : _slots)
        if (client != nullptr && slot.client == client)
            return &slot;
    return nullptr;
}

bool tnfsCallbackGate::open(void *client)
{
    std::lock_guard<std::mutex> lock(_lock);
    if (client == nullptr || find(client) != nullptr)
        return false;
    for (Slot &slot : _slots)
    {
        if (slot.client == nullptr) // a free slot is never busy or closing
        {
            slot.client = client;
            return true;
        }
    }
    return false;
}

void tnfsCallbackGate::close(void *client)
{
    std::unique_lock<std::mutex> lock(_lock);
    Slot *slot = find(client);
    if (slot == nullptr)
        return;
    slot->closing = true; // no new run() gets in while we wait
    _idle.wait(lock, [slot] { return slot->busy == 0; });
    if (slot->client == client) // a concurrent close() may have finished and the slot moved on
    {
        slot->client = nullptr;
        slot->closing = false;
    }
}

bool tnfsCallbackGate::run(void *client, Work work)
{
    Slot *slot;
    {
        std::lock_guard<std::mutex> lock(_lock);
        slot = find(client);
        if (slot == nullptr || slot->closing || work == nullptr)
            return false;
        slot->busy++; // keeps close() waiting and the slot from being reused
    }
    work(client);
    {
        std::lock_guard<std::mutex> lock(_lock);
        slot->busy--;
    }
    _idle.notify_all();
    return true;
}

bool tnfsCallbackGate::is_open(void *client) const
{
    std::lock_guard<std::mutex> lock(_lock);
    for (const Slot &slot : _slots)
        if (client != nullptr && slot.client == client && !slot.closing)
            return true;
    return false;
}
